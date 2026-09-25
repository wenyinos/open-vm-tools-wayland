/*********************************************************
 * Copyright (C) 2026 GoProSlowYo
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * waylandDrag.c --
 *
 *    Start a host to guest drag natively, through wl_data_device.
 *
 *    Like the clipboard backend this keeps its own connection to the
 *    compositor, because GDK is pinned to its x11 backend for the detection
 *    window and cannot be asked for a Wayland surface.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define G_LOG_DOMAIN "dndcp"

#include "waylandDrag.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <wayland-client.h>
#include <glib-unix.h>
#include <errno.h>
#include <sys/poll.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * There is one overlay per output. A Wayland client is not told where the
 * pointer is, so it has to cover every place the pointer might be. With more
 * outputs than this the pointer may be outside every overlay, and the drag
 * then falls back to XDND.
 */
#define WAYLAND_DRAG_MAX_OUTPUTS 8

/*
 * A limit on the overlay size the compositor asks for. An overlay covers one
 * output, and no output is this big. A larger size is refused before the
 * width is multiplied up into a buffer size.
 */
#define WAYLAND_DRAG_MAX_DIM 16384

/* How long to keep the pipe open for a drop target that reads slowly. */
#define WAYLAND_DRAG_SEND_TIMEOUT_US (2 * G_USEC_PER_SEC)

/*
 * How long all the sends of one drag may take together. A target can ask
 * for the data as often as it likes, and each send blocks the main loop
 * while the target reads.
 */
#define WAYLAND_DRAG_SEND_BUDGET_US (4 * G_USEC_PER_SEC)

/*
 * How long this module waits for the compositor in WaylandDrag_Init() and
 * WaylandDrag_Arm(). These waits run on the plugin's main loop, which also
 * carries the RPC channel, and a compositor that has stopped answering must
 * not stall it.
 */
#define WAYLAND_DRAG_REPLY_TIMEOUT_MS 500

/*
 * How long the compositor has to answer the wl_display.sync sent after
 * start_drag before the drag is taken to have never started. A running
 * compositor answers one in milliseconds.
 */
#define WAYLAND_DRAG_WATCHDOG_S 5

/*
 * How long a target has after the drop to send dnd_finished. A target asks
 * for the bytes as soon as it gets the drop, and reads the files from the
 * staging directory, which the end of the drag does not remove. Ending the
 * drag after this long takes nothing a working target still needs.
 */
#define WAYLAND_DRAG_FINISH_TIMEOUT_S 10

typedef struct {
   char *mimeType;
   void *data;
   size_t size;
} DragItem;

typedef struct {
   uint32_t name;                          /* registry name, for removal */
   struct wl_output *output;
   struct wl_surface *surface;
   struct zwlr_layer_surface_v1 *layer;
   struct wl_buffer *buffer;
   int width;
   int height;
   /* The output's current mode, in pixels, and its transform. */
   int modeWidth;
   int modeHeight;
   int32_t transform;
} DragOverlay;

static struct {
   struct wl_display *display;
   struct wl_registry *registry;
   struct wl_compositor *compositor;
   struct wl_shm *shm;
   struct wl_seat *seat;
   struct wl_pointer *pointer;
   struct wl_data_device_manager *manager;
   struct wl_data_device *device;
   struct zwlr_layer_shell_v1 *layerShell;

   DragOverlay overlays[WAYLAND_DRAG_MAX_OUTPUTS];
   size_t nOverlays;
   gboolean mapped;
   /* The overlays were unmapped for the drag. A configure must not remap. */
   gboolean hidden;

   /*
    * The surface the pointer is over. A press can come only from that
    * surface, so it is the only valid drag origin.
    */
   struct wl_surface *focus;

   struct wl_data_source *source;
   /*
    * The latest offer the compositor has sent this connection. Each offer is
    * a proxy this module has to destroy, and it reads none of them.
    */
   struct wl_data_offer *offer;
   DragItem *items;
   size_t nItems;
   gboolean dragging;
   /*
    * Counts start_drag requests, so WaylandDrag_StartOnPress() can see that
    * one was made even when the drag it made has already ended.
    */
   unsigned int starts;
   /* The target's latest answer, which the compositor decides the drop on. */
   gboolean accepted;
   /* What is left of WAYLAND_DRAG_SEND_BUDGET_US for this drag. */
   gint64 sendBudgetUs;

   WaylandDragDoneFunc done;
   void *clientData;

   guint watch;
   /*
    * Sent after start_drag. An answer with no cancelled before it means the
    * compositor took the drag.
    */
   struct wl_callback *startSync;
   /* Fires if startSync is never answered at all. */
   guint watchdog;
   /* Fires if the target never finishes a drop it was given. */
   guint finishTimer;
   gboolean inited;
} gDrag;

/*
 * The compositor lacks a global that a native drag needs. That does not
 * change for the life of the session, so WaylandDrag_Init() does not try
 * again.
 */
static gboolean gUnsupported;


static void Finish(void);


/*
 *-----------------------------------------------------------------------------
 *
 * FreeItems --
 *
 *    Free the copy of the offered items.
 *
 *-----------------------------------------------------------------------------
 */

static void
FreeItems(void)
{
   size_t i;

   for (i = 0; i < gDrag.nItems; i++) {
      g_free(gDrag.items[i].mimeType);
      g_free(gDrag.items[i].data);
   }
   g_free(gDrag.items);
   gDrag.items = NULL;
   gDrag.nItems = 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ReadOnce --
 *
 *    Dispatch whatever is queued, then wait up to timeoutMs for more from
 *    the compositor and dispatch that.
 *
 *    wl_display_roundtrip() and wl_display_dispatch() wait for as long as
 *    the compositor takes, and these waits run on the main loop that also
 *    carries the RPC channel. This is their bounded form.
 *
 * Results:
 *    -1 if the connection is lost, 0 if nothing came in time, 1 if something
 *    was dispatched, *stop was set, or the wait was interrupted.
 *
 *-----------------------------------------------------------------------------
 */

static int
ReadOnce(int timeoutMs,          // IN
         const gboolean *stop)   // IN/OPT: return as soon as this is set
{
   struct pollfd pfd;
   int n;

   while (wl_display_prepare_read(gDrag.display) != 0) {
      if (wl_display_dispatch_pending(gDrag.display) < 0) {
         return -1;
      }
      if (stop != NULL && *stop) {
         return 1;
      }
   }
   wl_display_flush(gDrag.display);

   pfd.fd = wl_display_get_fd(gDrag.display);
   pfd.events = POLLIN;
   pfd.revents = 0;
   n = poll(&pfd, 1, MAX(timeoutMs, 0));
   if (n <= 0) {
      wl_display_cancel_read(gDrag.display);
      if (n < 0) {
         return errno == EINTR ? 1 : -1;
      }
      return 0;
   }
   if (wl_display_read_events(gDrag.display) < 0 ||
       wl_display_dispatch_pending(gDrag.display) < 0) {
      return -1;
   }

   return 1;
}


static void
SyncDone(void *data,               // IN: the caller's flag
         struct wl_callback *cb,   // IN
         uint32_t serial)          // IN: unused
{
   *(gboolean *) data = TRUE;
   wl_callback_destroy(cb);
}


static const struct wl_callback_listener gSyncListener = {
   .done = SyncDone,
};


/*
 *-----------------------------------------------------------------------------
 *
 * SyncUntil --
 *
 *    wl_display_roundtrip(), but giving up at deadline.
 *
 * Results:
 *    1 if the compositor answered, 0 if the deadline passed first, -1 if
 *    the connection is lost.
 *
 *-----------------------------------------------------------------------------
 */

static int
SyncUntil(gint64 deadline)   // IN: monotonic time
{
   gboolean answered = FALSE;
   struct wl_callback *cb = wl_display_sync(gDrag.display);
   int r = 1;

   wl_callback_add_listener(cb, &gSyncListener, &answered);
   while (!answered) {
      gint64 left = (deadline - g_get_monotonic_time()) / 1000;

      if (left <= 0) {
         r = 0;
         break;
      }
      r = ReadOnce((int) left, &answered);
      if (r < 0) {
         break;
      }
   }
   if (!answered) {
      /*
       * The answer can still come, and the flag it would set is on this
       * stack. Destroying the proxy makes libwayland discard it instead.
       */
      wl_callback_destroy(cb);
      return r < 0 ? -1 : 0;
   }

   return 1;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragIsReal, DragNeverStarted --
 *
 *    The module has to find out whether start_drag succeeded. Waiting on
 *    cancelled or dnd_finished alone would leave it believing in a drag that
 *    never began, for the rest of the session.
 *
 *    The events a drag produces do not settle it. target, action and a data
 *    device enter arrive only while the pointer is over a surface whose
 *    client has a data device. A drag across anything else produces no events
 *    for as long as it lasts, and a timeout on them would end it. So a
 *    wl_display.sync follows start_drag. The compositor answers a refused
 *    start_drag with cancelled, and sends that before it answers the sync. A
 *    sync answered while the source still exists therefore means the grab
 *    exists. The watchdog covers a compositor that answers nothing at all.
 *
 *    Mutter sends no event at all for a refused start_drag, which this would
 *    take for a drag. Mutter has no layer shell, so it never gets here, and a
 *    drag taken in error lasts only until the caller cancels it when the
 *    next one begins.
 *
 *-----------------------------------------------------------------------------
 */

static void
DragIsReal(void)
{
   if (gDrag.watchdog != 0) {
      g_source_remove(gDrag.watchdog);
      gDrag.watchdog = 0;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * ForgetFinishTimer, ForgetStartSync --
 *
 *    Remove the timer that ends a drop the target never finishes, and the
 *    sync that follows start_drag.
 *
 *-----------------------------------------------------------------------------
 */

static void
ForgetFinishTimer(void)
{
   if (gDrag.finishTimer != 0) {
      g_source_remove(gDrag.finishTimer);
      gDrag.finishTimer = 0;
   }
}


static void
ForgetStartSync(void)
{
   if (gDrag.startSync != NULL) {
      wl_callback_destroy(gDrag.startSync);
      gDrag.startSync = NULL;
   }
}


static gboolean
DragNeverStarted(gpointer data)   // IN: unused
{
   gDrag.watchdog = 0;
   g_warning("%s: the compositor never answered start_drag; giving it up\n",
             __FUNCTION__);
   Finish();

   return G_SOURCE_REMOVE;
}


static void
StartSyncDone(void *data,                  // IN: unused
              struct wl_callback *cb,      // IN
              uint32_t serial)             // IN: unused
{
   if (cb == gDrag.startSync) {
      gDrag.startSync = NULL;
   }
   wl_callback_destroy(cb);

   if (gDrag.dragging) {
      g_debug("%s: the compositor took the drag\n", __FUNCTION__);
      DragIsReal();
   }
}


static const struct wl_callback_listener gStartSyncListener = {
   .done = StartSyncDone,
};


/*
 *-----------------------------------------------------------------------------
 *
 * SourceSend --
 *
 *    The target asks for the data. Write the bytes for the requested type.
 *
 *    Writing can block if the reader is slow or goes away, so the pipe is put
 *    into non-blocking mode, and the writes stop at a deadline and at the
 *    drag's send budget. A stuck reader must not stall the main loop, which
 *    also carries the RPC channel.
 *
 *-----------------------------------------------------------------------------
 */

static void
SourceSend(void *data,                     // IN: unused
           struct wl_data_source *source,  // IN: unused
           const char *mimeType,           // IN
           int32_t fd)                     // IN
{
   size_t i;
   struct stat st;

   g_debug("%s: serving %s\n", __FUNCTION__, mimeType);

   /*
    * The fd comes from the target through the compositor, and should be a
    * pipe. O_NONBLOCK bounds the writes below. A regular file ignores
    * O_NONBLOCK, and a write to one on a FUSE mount can block for as long as
    * the file system takes.
    */
   if (fstat(fd, &st) != 0 || !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
      g_warning("%s: not writing %s to something that is not a pipe\n",
                __FUNCTION__, mimeType);
      close(fd);
      return;
   }
   if (gDrag.sendBudgetUs <= 0) {
      g_warning("%s: this drag has spent its time on sends; not serving %s\n",
                __FUNCTION__, mimeType);
      close(fd);
      return;
   }

   for (i = 0; i < gDrag.nItems; i++) {
      if (strcmp(gDrag.items[i].mimeType, mimeType) == 0) {
         const char *p = gDrag.items[i].data;
         size_t left = gDrag.items[i].size;
         gint64 started = g_get_monotonic_time();
         gint64 deadline = started + MIN(WAYLAND_DRAG_SEND_TIMEOUT_US,
                                         gDrag.sendBudgetUs);
         int lastErrno = 0;

         /*
          * WaylandDrag_Arm() keeps no empty item, so this should not happen.
          * Log it if it does. The target reads an empty send as a successful
          * transfer of nothing, and some targets crash on that.
          */
         if (left == 0) {
            g_warning("%s: %s is on offer with no bytes behind it\n",
                      __FUNCTION__, mimeType);
         }

         if (!g_unix_set_fd_nonblocking(fd, TRUE, NULL)) {
            g_debug("%s: could not make the pipe non-blocking\n", __FUNCTION__);
         }
         while (left > 0 && g_get_monotonic_time() < deadline) {
            ssize_t n = write(fd, p, left);

            if (n > 0) {
               p += n;
               left -= (size_t) n;
               continue;
            }
            lastErrno = errno;
            if (n < 0 && (lastErrno == EAGAIN || lastErrno == EINTR)) {
               struct pollfd pfd;

               pfd.fd = fd;
               pfd.events = POLLOUT;
               pfd.revents = 0;
               if (poll(&pfd, 1, 20) > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
                  break;
               }
               continue;
            }
            break;
         }
         if (left > 0) {
            g_warning("%s: %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT
                      " bytes of %s unsent: %s\n", __FUNCTION__,
                      (gsize) left, (gsize) gDrag.items[i].size, mimeType,
                      lastErrno != 0 ? g_strerror(lastErrno) : "deadline");
         }
         gDrag.sendBudgetUs -= g_get_monotonic_time() - started;
         close(fd);
         return;
      }
   }

   g_debug("%s: nothing on offer for %s\n", __FUNCTION__, mimeType);
   close(fd);
}


/*
 *-----------------------------------------------------------------------------
 *
 * SourceTarget --
 *
 *    The target under the drag has answered. Record whether it accepts.
 *
 *-----------------------------------------------------------------------------
 */

static void
SourceTarget(void *data,                     // IN: unused
             struct wl_data_source *source,  // IN: unused
             const char *mimeType)           // IN: NULL for a refusal
{
   gboolean accepted = mimeType != NULL;

   DragIsReal();

   /*
    * A file manager answers a single pointer motion with an accept and two
    * refusals, so logging every answer produces thousands of lines per drag.
    * Log only the changes.
    */
   if (accepted != gDrag.accepted) {
      g_debug("%s: target accepts %s\n", __FUNCTION__,
              accepted ? mimeType : "nothing");
   }
   gDrag.accepted = accepted;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SourceCancelled, SourceDndFinished --
 *
 *    The drag is over. Some compositors send both for a successful drop,
 *    dnd_finished first, so Finish() has to tolerate being called twice.
 *
 *    No other event reports the end of the drag. The caller's press is still
 *    down at this point, and only the callback releases it. Missing these
 *    events would leave the pointer held in a drag for the rest of the
 *    session.
 *
 *-----------------------------------------------------------------------------
 */

static void
SourceCancelled(void *data,                    // IN: unused
                struct wl_data_source *source) // IN: unused
{
   g_debug("%s: the drag was cancelled\n", __FUNCTION__);
   Finish();
}


static gboolean
DropNeverFinished(gpointer data)   // IN: unused
{
   gDrag.finishTimer = 0;
   g_warning("%s: the target never finished the drop; ending the drag\n",
             __FUNCTION__);
   Finish();

   return G_SOURCE_REMOVE;
}


static void
SourceDndDropPerformed(void *data,                    // IN: unused
                       struct wl_data_source *source) // IN: unused
{
   /* The target has yet to ask for the bytes, so keep the source. */
   g_debug("%s: dropped\n", __FUNCTION__);
   DragIsReal();

   /*
    * From here only the target's dnd_finished or a cancel ends the drag,
    * and the protocol allows a target never to send dnd_finished. Until the
    * drag ends no new native drag can start, and every later host to guest
    * drag would fall back to XDND.
    */
   if (gDrag.finishTimer == 0) {
      gDrag.finishTimer = g_timeout_add_seconds(WAYLAND_DRAG_FINISH_TIMEOUT_S,
                                                DropNeverFinished, NULL);
   }
}


static void
SourceDndFinished(void *data,                    // IN: unused
                  struct wl_data_source *source) // IN: unused
{
   g_debug("%s: the target took the drop\n", __FUNCTION__);
   Finish();
}


static void
SourceAction(void *data,                    // IN: unused
             struct wl_data_source *source, // IN: unused
             uint32_t action)               // IN
{
   DragIsReal();
}


/*
 *-----------------------------------------------------------------------------
 *
 * OfferDestroy --
 *
 *    Destroy an offer this connection received, if there is one.
 *
 *-----------------------------------------------------------------------------
 */

static void
OfferDestroy(struct wl_data_offer *offer)   // IN/OPT
{
   if (offer != NULL) {
      wl_data_offer_destroy(offer);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DeviceDataOffer, DeviceEnter, DeviceLeave, DeviceMotion, DeviceDrop,
 * DeviceSelection --
 *
 *    The drag as this client's own data device sees it.
 *
 *    The compositor must stop offering the drag to the overlay as soon as
 *    HideOverlays() runs. These data device events are the only way to see
 *    that, because the rest of this module sees only the source's side of
 *    the drag. An enter that is not followed straight away by a leave, or any
 *    drop here, means the overlay is taking the drop away from the
 *    application under it.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeviceDataOffer(void *d,                    // IN: unused
                struct wl_data_device *dd,  // IN: unused
                struct wl_data_offer *o)    // IN
{
   /*
    * A new offer replaces the previous one. Without this the offers
    * accumulate for the life of the connection, one per drag and one per
    * selection anyone on the seat makes.
    */
   OfferDestroy(gDrag.offer);
   gDrag.offer = o;
}

static void
DeviceEnter(void *d,                     // IN: unused
            struct wl_data_device *dd,   // IN: unused
            uint32_t serial,             // IN: unused
            struct wl_surface *surface,  // IN: unused
            wl_fixed_t x,                // IN: unused
            wl_fixed_t y,                // IN: unused
            struct wl_data_offer *o)     // IN: unused
{
   DragIsReal();
   g_debug("%s: the overlay was offered the drag\n", __FUNCTION__);
}

static void
DeviceLeave(void *d,                    // IN: unused
            struct wl_data_device *dd)  // IN: unused
{
   g_debug("%s: the drag left the overlay\n", __FUNCTION__);
   OfferDestroy(gDrag.offer);
   gDrag.offer = NULL;
}

static void
DeviceMotion(void *d,                    // IN: unused
             struct wl_data_device *dd,  // IN: unused
             uint32_t t,                 // IN: unused
             wl_fixed_t x,               // IN: unused
             wl_fixed_t y)               // IN: unused
{
}

static void
DeviceDrop(void *d,                    // IN: unused
           struct wl_data_device *dd)  // IN: unused
{
   g_warning("%s: the overlay took the drop meant for the target\n",
             __FUNCTION__);
}

static void
DeviceSelection(void *d,                    // IN: unused
                struct wl_data_device *dd,  // IN: unused
                struct wl_data_offer *o)    // IN/OPT
{
   /* The clipboard backend handles the selection. */
   if (o != NULL) {
      if (o == gDrag.offer) {
         gDrag.offer = NULL;
      }
      OfferDestroy(o);
   }
}

static const struct wl_data_device_listener gDeviceListener = {
   .data_offer = DeviceDataOffer,
   .enter = DeviceEnter,
   .leave = DeviceLeave,
   .motion = DeviceMotion,
   .drop = DeviceDrop,
   .selection = DeviceSelection,
};


static const struct wl_data_source_listener gSourceListener = {
   .target = SourceTarget,
   .send = SourceSend,
   .cancelled = SourceCancelled,
   .dnd_drop_performed = SourceDndDropPerformed,
   .dnd_finished = SourceDndFinished,
   .action = SourceAction,
};


/*
 *-----------------------------------------------------------------------------
 *
 * HideOverlays --
 *
 *    Take the overlays out of the drag that start_drag has just begun.
 *
 *    An overlay covers its output, so once the drag is under way it is the
 *    surface under the cursor. The compositor would then offer the drop to
 *    this client rather than to the application the host dropped on.
 *
 *-----------------------------------------------------------------------------
 */

static void
HideOverlays(void)
{
   struct wl_region *empty = wl_compositor_create_region(gDrag.compositor);
   size_t i;

   for (i = 0; i < gDrag.nOverlays; i++) {
      if (gDrag.overlays[i].surface != NULL) {
         wl_surface_set_input_region(gDrag.overlays[i].surface, empty);
         wl_surface_commit(gDrag.overlays[i].surface);
      }
   }
   wl_region_destroy(empty);

   /*
    * sway and KWin honour an empty input region. Unmapping takes the surface
    * out of the drag as well, on any compositor. The compositor handles
    * start_drag before this commit, so the drag continues without the
    * surface.
    */
   gDrag.hidden = TRUE;
   for (i = 0; i < gDrag.nOverlays; i++) {
      if (gDrag.overlays[i].surface != NULL) {
         wl_surface_attach(gDrag.overlays[i].surface, NULL, 0, 0);
         wl_surface_commit(gDrag.overlays[i].surface);
      }
   }
   g_debug("%s: %u overlays out of hit testing and unmapped\n", __FUNCTION__,
           (unsigned) gDrag.nOverlays);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MakeOverlay --
 *
 *    Ask for a transparent layer surface covering o->output. It maps when
 *    LayerSurfaceConfigure() gets its configure.
 *
 *-----------------------------------------------------------------------------
 */

static const struct zwlr_layer_surface_v1_listener gLayerSurfaceListener;

static void
MakeOverlay(DragOverlay *o)   // IN/OUT: output set, the rest empty
{
   o->surface = wl_compositor_create_surface(gDrag.compositor);
   o->layer = zwlr_layer_shell_v1_get_layer_surface(
                 gDrag.layerShell, o->surface, o->output,
                 ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "vmware-dnd");
   zwlr_layer_surface_v1_add_listener(o->layer, &gLayerSurfaceListener, o);
   zwlr_layer_surface_v1_set_anchor(o->layer,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
   /*
    * Anchored to all four edges with an exclusive zone of -1, so the overlay
    * covers the whole output and no window is moved or resized for it.
    */
   zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
   zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
   wl_surface_commit(o->surface);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DestroyOverlay --
 *
 *    Destroy an overlay's objects, keeping its output.
 *
 *-----------------------------------------------------------------------------
 */

static void
DestroyOverlay(DragOverlay *o)   // IN/OUT
{
   if (o->layer != NULL) {
      zwlr_layer_surface_v1_destroy(o->layer);
      o->layer = NULL;
   }
   if (o->surface != NULL) {
      wl_surface_destroy(o->surface);
      o->surface = NULL;
   }
   if (o->buffer != NULL) {
      wl_buffer_destroy(o->buffer);
      o->buffer = NULL;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Unmap --
 *
 *    Destroy the overlays. The connection and the bound globals stay.
 *
 *-----------------------------------------------------------------------------
 */

static void
Unmap(void)
{
   size_t i;

   for (i = 0; i < gDrag.nOverlays; i++) {
      DestroyOverlay(&gDrag.overlays[i]);
   }
   gDrag.focus = NULL;
   gDrag.mapped = FALSE;
   gDrag.hidden = FALSE;
   if (gDrag.display != NULL) {
      wl_display_flush(gDrag.display);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Finish --
 *
 *    End the drag and tell the caller, once.
 *
 *-----------------------------------------------------------------------------
 */

static void
Finish(void)
{
   WaylandDragDoneFunc done = gDrag.done;
   void *clientData = gDrag.clientData;

   /*
    * Some compositors send cancelled after dnd_finished even when the drop
    * succeeded, so this can run twice. Clearing the callback first and
    * destroying the source early keep the second run from releasing the
    * button again or freeing the items twice.
    */
   gDrag.done = NULL;
   gDrag.dragging = FALSE;
   DragIsReal();
   ForgetStartSync();
   ForgetFinishTimer();
   if (gDrag.source != NULL) {
      wl_data_source_destroy(gDrag.source);
      gDrag.source = NULL;
   }
   FreeItems();
   Unmap();

   if (done != NULL) {
      done(clientData);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PointerEnter, PointerLeave --
 *
 *    Track which overlay the pointer is over. start_drag names an origin
 *    surface, and the compositor checks that the press came from it, so the
 *    drag has to start from the overlay the pointer entered.
 *
 *-----------------------------------------------------------------------------
 */

static void
PointerEnter(void *data,                  // IN: unused
             struct wl_pointer *pointer,  // IN: unused
             uint32_t serial,             // IN
             struct wl_surface *surface,  // IN
             wl_fixed_t x,                // IN: unused
             wl_fixed_t y)                // IN: unused
{
   gDrag.focus = surface;
}


static void
PointerLeave(void *data,                  // IN: unused
             struct wl_pointer *pointer,  // IN: unused
             uint32_t serial,             // IN
             struct wl_surface *surface)  // IN
{
   if (gDrag.focus == surface) {
      gDrag.focus = NULL;
   }
}


static void
PointerMotion(void *data, struct wl_pointer *pointer, uint32_t time,
              wl_fixed_t x, wl_fixed_t y)
{
}


/*
 *-----------------------------------------------------------------------------
 *
 * PointerButton --
 *
 *    Start the drag from the handler of the press that authorises it.
 *
 *    The serial has to come from a button press on a surface this client
 *    owns. The compositor refuses a wl_pointer.enter serial with an immediate
 *    wl_data_source.cancelled, so the caller cannot start the drag later.
 *
 *-----------------------------------------------------------------------------
 */

static void
PointerButton(void *data,                 // IN: unused
              struct wl_pointer *pointer, // IN: unused
              uint32_t serial,            // IN
              uint32_t time,              // IN: unused
              uint32_t button,            // IN
              uint32_t state)             // IN
{
   size_t i;

   if (state != WL_POINTER_BUTTON_STATE_PRESSED ||
       gDrag.dragging || gDrag.focus == NULL || gDrag.source != NULL) {
      return;
   }

   gDrag.source = wl_data_device_manager_create_data_source(gDrag.manager);
   wl_data_source_add_listener(gDrag.source, &gSourceListener, NULL);
   for (i = 0; i < gDrag.nItems; i++) {
      wl_data_source_offer(gDrag.source, gDrag.items[i].mimeType);
      g_debug("%s: offering %s\n", __FUNCTION__, gDrag.items[i].mimeType);
   }
   /*
    * set_actions is v3. Without it a v3 target sees no action, refuses the
    * drop and never sends dnd_finished.
    */
   if (wl_data_device_manager_get_version(gDrag.manager) >=
       WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION) {
      wl_data_source_set_actions(gDrag.source,
                                 WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
   }

   gDrag.sendBudgetUs = WAYLAND_DRAG_SEND_BUDGET_US;

   /* No drag icon. The host draws one on its side of the window. */
   wl_data_device_start_drag(gDrag.device, gDrag.source, gDrag.focus, NULL,
                             serial);
   ForgetStartSync();
   gDrag.startSync = wl_display_sync(gDrag.display);
   wl_callback_add_listener(gDrag.startSync, &gStartSyncListener, NULL);
   HideOverlays();
   wl_display_flush(gDrag.display);
   gDrag.dragging = TRUE;
   gDrag.starts++;
   gDrag.watchdog = g_timeout_add_seconds(WAYLAND_DRAG_WATCHDOG_S,
                                          DragNeverStarted, NULL);
   g_debug("%s: drag started from serial %u\n", __FUNCTION__, serial);
}


static void
PointerAxis(void *data, struct wl_pointer *pointer, uint32_t time,
            uint32_t axis, wl_fixed_t value)
{
}


static void
PointerFrame(void *data, struct wl_pointer *pointer)
{
}


static void
PointerAxisSource(void *data, struct wl_pointer *pointer, uint32_t axisSource)
{
}


static void
PointerAxisStop(void *data, struct wl_pointer *pointer, uint32_t time,
                uint32_t axis)
{
}


static void
PointerAxisDiscrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                    int32_t discrete)
{
}


static void
PointerAxisValue120(void *data, struct wl_pointer *pointer, uint32_t axis,
                    int32_t value120)
{
}


static void
PointerAxisRelativeDirection(void *data, struct wl_pointer *pointer,
                             uint32_t axis, uint32_t direction)
{
}


static const struct wl_pointer_listener gPointerListener = {
   .enter = PointerEnter,
   .leave = PointerLeave,
   .motion = PointerMotion,
   .button = PointerButton,
   .axis = PointerAxis,
   .frame = PointerFrame,
   .axis_source = PointerAxisSource,
   .axis_stop = PointerAxisStop,
   .axis_discrete = PointerAxisDiscrete,
   .axis_value120 = PointerAxisValue120,
   .axis_relative_direction = PointerAxisRelativeDirection,
};


static void
SeatCapabilities(void *data,             // IN: unused
                 struct wl_seat *seat,   // IN
                 uint32_t capabilities)  // IN
{
   if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 &&
       gDrag.pointer == NULL) {
      gDrag.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(gDrag.pointer, &gPointerListener, NULL);
   }
}


static void
SeatName(void *data, struct wl_seat *seat, const char *name)
{
}


static const struct wl_seat_listener gSeatListener = {
   .capabilities = SeatCapabilities,
   .name = SeatName,
};


/*
 *-----------------------------------------------------------------------------
 *
 * MakeBuffer --
 *
 *    Create a fully transparent buffer the size of the output.
 *
 *    A surface has to have a buffer to be mapped, and only a mapped surface
 *    receives pointer events. memfd pages read as zero, which in ARGB8888 is
 *    fully transparent, so nothing has to be written to it.
 *
 *-----------------------------------------------------------------------------
 */

static struct wl_buffer *
MakeBuffer(int width,   // IN
           int height)  // IN
{
   struct wl_shm_pool *pool;
   struct wl_buffer *buffer;
   int64_t stride = (int64_t) width * 4;
   int64_t size = stride * height;
   int fd;

   /*
    * wl_shm takes the pool size and the stride as int32_t. Computing them in
    * 64 bits keeps a size the compositor asked for from overflowing, which
    * would declare a pool smaller than the buffer laid out in it.
    */
   if (width <= 0 || height <= 0 || size > INT32_MAX) {
      g_warning("%s: refusing a %dx%d buffer\n", __FUNCTION__, width, height);
      return NULL;
   }

   fd = memfd_create("vmware-dnd", MFD_CLOEXEC);
   if (fd < 0) {
      g_warning("%s: memfd_create: %s\n", __FUNCTION__, g_strerror(errno));
      return NULL;
   }
   if (ftruncate(fd, (off_t) size) < 0) {
      g_warning("%s: ftruncate: %s\n", __FUNCTION__, g_strerror(errno));
      close(fd);
      return NULL;
   }

   pool = wl_shm_create_pool(gDrag.shm, fd, (int32_t) size);
   buffer = wl_shm_pool_create_buffer(pool, 0, width, height, (int32_t) stride,
                                      WL_SHM_FORMAT_ARGB8888);
   wl_shm_pool_destroy(pool);
   close(fd);

   return buffer;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LayerSurfaceConfigure --
 *
 *    The compositor has sized the overlay. Attach a transparent buffer of
 *    that size and declare no part of it opaque, so that what is underneath
 *    still shows.
 *
 *-----------------------------------------------------------------------------
 */

static void
LayerSurfaceConfigure(void *data,                             // IN: DragOverlay
                      struct zwlr_layer_surface_v1 *layer,    // IN
                      uint32_t serial,                        // IN
                      uint32_t width,                         // IN
                      uint32_t height)                        // IN
{
   DragOverlay *o = data;
   struct wl_region *empty;

   zwlr_layer_surface_v1_ack_configure(layer, serial);
   /* The entry may already have been destroyed. Nothing is left to map. */
   if (o->surface == NULL) {
      return;
   }
   if (width == 0 || height == 0) {
      return;
   }
   if (width > WAYLAND_DRAG_MAX_DIM || height > WAYLAND_DRAG_MAX_DIM) {
      g_warning("%s: refusing a %ux%u overlay\n", __FUNCTION__, width, height);
      return;
   }

   /*
    * Committing a NULL buffer unmaps a layer surface, and some compositors
    * send a configure after the unmap. Attaching the buffer again here would
    * put the overlay back over the cursor in the middle of the drag, undoing
    * what HideOverlays() just did.
    */
   if (gDrag.hidden) {
      g_debug("%s: configure %ux%u while hidden, staying unmapped\n",
              __FUNCTION__, width, height);
      return;
   }

   if (o->buffer == NULL || (int) width != o->width ||
       (int) height != o->height) {
      if (o->buffer != NULL) {
         wl_buffer_destroy(o->buffer);
      }
      o->width = (int) width;
      o->height = (int) height;
      o->buffer = MakeBuffer(o->width, o->height);
   }
   if (o->buffer == NULL) {
      return;
   }

   empty = wl_compositor_create_region(gDrag.compositor);
   wl_surface_set_opaque_region(o->surface, empty);
   wl_region_destroy(empty);
   wl_surface_attach(o->surface, o->buffer, 0, 0);
   /* wl_surface.damage_buffer needs v4, and the rest of this works at v1. */
   wl_surface_damage(o->surface, 0, 0, o->width, o->height);
   wl_surface_commit(o->surface);
   gDrag.mapped = TRUE;
}


static void
LayerSurfaceClosed(void *data,                          // IN: DragOverlay
                   struct zwlr_layer_surface_v1 *layer) // IN: unused
{
   DragOverlay *o = data;

   g_debug("%s: the compositor closed the overlay\n", __FUNCTION__);
   if (o->surface == NULL) {
      return;
   }
   if (gDrag.focus == o->surface) {
      gDrag.focus = NULL;
   }
}


static const struct zwlr_layer_surface_v1_listener gLayerSurfaceListener = {
   .configure = LayerSurfaceConfigure,
   .closed = LayerSurfaceClosed,
};


/*
 *-----------------------------------------------------------------------------
 *
 * OutputGeometry --
 * OutputMode --
 *
 *    Record an output's transform and current mode. The host gives pointer
 *    positions in pixels of the guest's screen, and with one output that is
 *    this mode. See WaylandDrag_GetOutputSize().
 *
 *-----------------------------------------------------------------------------
 */

static void
OutputGeometry(void *data,                // IN: the DragOverlay
               struct wl_output *output,  // IN: unused
               int32_t x,                 // IN: unused
               int32_t y,                 // IN: unused
               int32_t physicalWidth,     // IN: unused
               int32_t physicalHeight,    // IN: unused
               int32_t subpixel,          // IN: unused
               const char *make,          // IN: unused
               const char *model,         // IN: unused
               int32_t transform)         // IN
{
   DragOverlay *o = data;

   o->transform = transform;
}


static void
OutputMode(void *data,                // IN: the DragOverlay
           struct wl_output *output,  // IN: unused
           uint32_t flags,            // IN
           int32_t width,             // IN
           int32_t height,            // IN
           int32_t refresh)           // IN: unused
{
   DragOverlay *o = data;

   if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
      o->modeWidth = width;
      o->modeHeight = height;
   }
}


/* Version 1 of wl_output sends only these two events. */
static const struct wl_output_listener gOutputListener = {
   .geometry = OutputGeometry,
   .mode = OutputMode,
};


static void
RegistryGlobal(void *data,                    // IN: unused
               struct wl_registry *registry,  // IN
               uint32_t name,                 // IN
               const char *iface,             // IN
               uint32_t version)              // IN
{
   if (strcmp(iface, wl_compositor_interface.name) == 0) {
      gDrag.compositor = wl_registry_bind(registry, name,
                                          &wl_compositor_interface,
                                          MIN(version, 4));
   } else if (strcmp(iface, wl_shm_interface.name) == 0) {
      gDrag.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
   } else if (strcmp(iface, wl_seat_interface.name) == 0 &&
              gDrag.seat == NULL) {
      gDrag.seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                    MIN(version, 5));
   } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
      gDrag.manager = wl_registry_bind(registry, name,
                                       &wl_data_device_manager_interface,
                                       MIN(version, 3));
   } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
      gDrag.layerShell = wl_registry_bind(registry, name,
                                          &zwlr_layer_shell_v1_interface,
                                          MIN(version, 2));
   } else if (strcmp(iface, wl_output_interface.name) == 0 &&
              gDrag.nOverlays < WAYLAND_DRAG_MAX_OUTPUTS) {
      DragOverlay *o = &gDrag.overlays[gDrag.nOverlays++];

      o->name = name;
      o->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
      wl_output_add_listener(o->output, &gOutputListener, o);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * RegistryGlobalRemove --
 *
 *    An output was removed. Using its wl_output afterwards is a protocol
 *    error, which would close the connection, so destroy it.
 *
 *-----------------------------------------------------------------------------
 */

static void
RegistryGlobalRemove(void *data,                   // IN: unused
                     struct wl_registry *registry, // IN: unused
                     uint32_t name)                // IN
{
   size_t i;

   for (i = 0; i < gDrag.nOverlays; i++) {
      DragOverlay *o = &gDrag.overlays[i];

      if (o->output == NULL || o->name != name) {
         continue;
      }
      if (o->layer != NULL) {
         zwlr_layer_surface_v1_destroy(o->layer);
      }
      if (o->surface != NULL) {
         if (gDrag.focus == o->surface) {
            gDrag.focus = NULL;
         }
         wl_surface_destroy(o->surface);
      }
      if (o->buffer != NULL) {
         wl_buffer_destroy(o->buffer);
      }
      wl_output_destroy(o->output);
      gDrag.overlays[i] = gDrag.overlays[--gDrag.nOverlays];
      memset(&gDrag.overlays[gDrag.nOverlays], 0, sizeof *o);
      /*
       * The output and layer surface listeners of the moved entry have the
       * address of the slot it came from, which has just been cleared. Point
       * them at the new slot. No other listener holds a pointer into
       * gDrag.overlays.
       */
      if (i < gDrag.nOverlays) {
         wl_output_set_user_data(gDrag.overlays[i].output, &gDrag.overlays[i]);
         if (gDrag.overlays[i].layer != NULL) {
            zwlr_layer_surface_v1_set_user_data(gDrag.overlays[i].layer,
                                                &gDrag.overlays[i]);
         }
      }
      return;
   }
}


static const struct wl_registry_listener gRegistryListener = {
   .global = RegistryGlobal,
   .global_remove = RegistryGlobalRemove,
};


/*
 *-----------------------------------------------------------------------------
 *
 * DisplayReadable --
 *
 *    Main loop callback for events from the compositor. Data requests reach
 *    SourceSend(), and the end of a drag reaches Finish(), through here.
 *
 * Side effects:
 *    A dead display shuts the module down. WaylandDrag_IsAvailable() then
 *    returns FALSE, and host to guest drags use XDND.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
DisplayReadable(gint fd,            // IN: unused
                GIOCondition cond,  // IN
                gpointer data)      // IN: unused
{
   /*
    * Do not wait here. The data this callback was woken for may already be
    * gone, because the other functions of this module read the same socket.
    * wl_display_dispatch() would then block the main loop, and the RPC
    * channel with it, until the compositor next sent something.
    */
   if ((cond & (G_IO_HUP | G_IO_ERR)) != 0 || ReadOnce(0, NULL) < 0) {
      g_warning("%s: lost the compositor connection\n", __FUNCTION__);
      /*
       * Returning G_SOURCE_REMOVE removes the watch. Clear the id so that
       * WaylandDrag_Exit() does not remove it again.
       */
      gDrag.watch = 0;
      WaylandDrag_Exit();
      return G_SOURCE_REMOVE;
   }
   wl_display_flush(gDrag.display);

   return G_SOURCE_CONTINUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DestroyProxy --
 *
 *    Destroy the client side of a bound global, if there is one. No request
 *    goes to the compositor. The disconnect that follows ends the object on
 *    its side.
 *
 *-----------------------------------------------------------------------------
 */

static void
DestroyProxy(void *proxy)   // IN/OPT
{
   if (proxy != NULL) {
      wl_proxy_destroy((struct wl_proxy *) proxy);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Disconnect --
 *
 *    Destroy the overlays and the bound globals, disconnect, and clear the
 *    module's state. Nothing of a drag may be left when this runs.
 *
 *-----------------------------------------------------------------------------
 */

static void
Disconnect(void)
{
   size_t i;

   Unmap();
   for (i = 0; i < gDrag.nOverlays; i++) {
      DestroyProxy(gDrag.overlays[i].output);
   }
   DestroyProxy(gDrag.device);
   DestroyProxy(gDrag.pointer);
   DestroyProxy(gDrag.seat);
   DestroyProxy(gDrag.manager);
   DestroyProxy(gDrag.layerShell);
   DestroyProxy(gDrag.shm);
   DestroyProxy(gDrag.compositor);
   DestroyProxy(gDrag.registry);
   wl_display_disconnect(gDrag.display);
   memset(&gDrag, 0, sizeof gDrag);
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_Init --
 *
 *    Connect to the compositor named by WAYLAND_DISPLAY, and bind the
 *    globals a native drag needs.
 *
 *    This can be called again after a failure. It returns at once when the
 *    compositor was found to lack one of those globals, since that does not
 *    change.
 *
 * Results:
 *    TRUE if a native drag can be started.
 *
 * Side effects:
 *    Adds a main loop watch on the connection.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandDrag_Init(void)
{
   const char *name = getenv("WAYLAND_DISPLAY");

   if (gDrag.inited) {
      return TRUE;
   }
   /*
    * wl_display_connect(NULL) falls back to wayland-0 when WAYLAND_DISPLAY
    * is unset. In an Xorg session that socket, if it exists, belongs to some
    * other compositor.
    */
   if (gUnsupported || name == NULL || *name == '\0') {
      return FALSE;
   }
   memset(&gDrag, 0, sizeof gDrag);

   gDrag.display = wl_display_connect(name);
   if (gDrag.display == NULL) {
      g_debug("%s: no Wayland display\n", __FUNCTION__);
      return FALSE;
   }

   gDrag.registry = wl_display_get_registry(gDrag.display);
   wl_registry_add_listener(gDrag.registry, &gRegistryListener, NULL);
   if (SyncUntil(g_get_monotonic_time() +
                 WAYLAND_DRAG_REPLY_TIMEOUT_MS * 1000) != 1) {
      g_debug("%s: the compositor did not answer\n", __FUNCTION__);
      WaylandDrag_Exit();
      return FALSE;
   }

   if (gDrag.compositor == NULL || gDrag.shm == NULL || gDrag.seat == NULL ||
       gDrag.manager == NULL || gDrag.layerShell == NULL) {
      /*
       * Mutter does not implement wlr-layer-shell, and on Mutter the XDND
       * path already reaches Wayland clients.
       */
      g_debug("%s: no native drag source here (layer shell %s)\n",
              __FUNCTION__, gDrag.layerShell != NULL ? "yes" : "no");
      gUnsupported = TRUE;
      Disconnect();
      return FALSE;
   }
   if (gDrag.nOverlays == 0) {
      g_debug("%s: the compositor has no output\n", __FUNCTION__);
      Disconnect();
      return FALSE;
   }

   wl_seat_add_listener(gDrag.seat, &gSeatListener, NULL);
   gDrag.device = wl_data_device_manager_get_data_device(gDrag.manager,
                                                         gDrag.seat);
   wl_data_device_add_listener(gDrag.device, &gDeviceListener, NULL);
   if (SyncUntil(g_get_monotonic_time() +
                 WAYLAND_DRAG_REPLY_TIMEOUT_MS * 1000) != 1) {
      g_debug("%s: the compositor did not answer\n", __FUNCTION__);
      WaylandDrag_Exit();
      return FALSE;
   }

   if (gDrag.pointer == NULL) {
      g_debug("%s: the seat has no pointer\n", __FUNCTION__);
      Disconnect();
      return FALSE;
   }

   gDrag.watch = g_unix_fd_add(wl_display_get_fd(gDrag.display),
                               G_IO_IN | G_IO_HUP | G_IO_ERR,
                               DisplayReadable, NULL);
   gDrag.inited = TRUE;
   g_debug("%s: native drag source ready, wl_data_device_manager v%u\n",
           __FUNCTION__, wl_data_device_manager_get_version(gDrag.manager));

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_Exit --
 *
 *    Release every object and disconnect. This is safe on a half torn down
 *    module, so it checks the display rather than the inited flag to decide
 *    whether there is work to do.
 *
 *-----------------------------------------------------------------------------
 */

void
WaylandDrag_Exit(void)
{
   WaylandDragDoneFunc done = gDrag.done;
   void *clientData = gDrag.clientData;

   if (gDrag.display == NULL) {
      return;
   }
   /*
    * Take the callback and call it once at the end, as Finish() does. A dead
    * display can bring this in the middle of a drag, while the caller's
    * faked press is still down, and only the callback releases it.
    */
   gDrag.done = NULL;
   gDrag.dragging = FALSE;
   /*
    * Remove the watchdog. The memset in Disconnect() would otherwise leave it
    * armed against a re-initialised module, where it would end a later drag.
    */
   DragIsReal();
   ForgetStartSync();
   ForgetFinishTimer();
   if (gDrag.watch != 0) {
      g_source_remove(gDrag.watch);
      gDrag.watch = 0;
   }
   if (gDrag.source != NULL) {
      wl_data_source_destroy(gDrag.source);
      gDrag.source = NULL;
   }
   OfferDestroy(gDrag.offer);
   gDrag.offer = NULL;
   FreeItems();
   Disconnect();

   if (done != NULL) {
      done(clientData);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_IsAvailable --
 *
 *    Whether WaylandDrag_Init() has connected, and the connection is still
 *    up.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandDrag_IsAvailable(void)
{
   return gDrag.inited;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_GetOutputSize --
 *
 *    The size of the only output, in pixels, after its transform.
 *
 *    The uinput pointer declares an absolute range that the compositor
 *    stretches over the output, so the range must be the size the host's
 *    coordinates are in. The X screen is not a reliable source for it under
 *    Wayland. xwayland-satellite, which niri runs, keeps its own screen size,
 *    and Xwayland reports a scaled output in logical pixels.
 *
 * Results:
 *    FALSE without a connection, before the compositor has sent a mode, or
 *    with more than one output, where the caller keeps its X screen size.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandDrag_GetOutputSize(int *width,   // OUT
                          int *height)  // OUT
{
   const DragOverlay *o;

   if (!gDrag.inited || gDrag.nOverlays != 1) {
      return FALSE;
   }
   o = &gDrag.overlays[0];
   if (o->modeWidth <= 0 || o->modeHeight <= 0) {
      return FALSE;
   }
   /* 90 and 270 degrees, flipped or not, are the odd transforms. */
   if ((o->transform & 1) != 0) {
      *width = o->modeHeight;
      *height = o->modeWidth;
   } else {
      *width = o->modeWidth;
      *height = o->modeHeight;
   }
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_Arm --
 *
 *    Offer these formats and map a transparent overlay on every output, so
 *    that the press the caller is about to fake lands on a surface of this
 *    client. That press yields a serial that start_drag accepts.
 *
 *    A Wayland client is not told where it is on screen and cannot place
 *    itself, so covering every output is the only way to be under a pointer
 *    the host is positioning.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandDrag_Arm(const WaylandDragItem *items,  // IN
                size_t nItems)                 // IN
{
   size_t i;
   gint64 deadline;
   int r;

   if (!gDrag.inited || items == NULL || nItems == 0) {
      return FALSE;
   }
   if (gDrag.dragging) {
      g_debug("%s: a drag is already running\n", __FUNCTION__);
      return FALSE;
   }
   WaylandDrag_Disarm();

   /*
    * Offer only the formats that have bytes. A target does not see an empty
    * send as a failure. A wl_data_source.send can only end in a closed pipe,
    * so an empty answer is a successful transfer of zero bytes. GTK turns an
    * empty text/uri-list into a NULL vector rather than an empty one, and
    * libfm walks that vector without checking it, so an empty type must not
    * be offered.
    */
   gDrag.items = g_new0(DragItem, nItems);
   gDrag.nItems = 0;
   for (i = 0; i < nItems; i++) {
      if (items[i].mimeType == NULL || items[i].data == NULL ||
          items[i].size == 0) {
         g_debug("%s: not offering %s, there are no bytes for it\n",
                 __FUNCTION__,
                 items[i].mimeType != NULL ? items[i].mimeType : "(null)");
         continue;
      }
      gDrag.items[gDrag.nItems].mimeType = g_strdup(items[i].mimeType);
      gDrag.items[gDrag.nItems].data = g_malloc(items[i].size);
      memcpy(gDrag.items[gDrag.nItems].data, items[i].data, items[i].size);
      gDrag.items[gDrag.nItems].size = items[i].size;
      gDrag.nItems++;
   }
   if (gDrag.nItems == 0) {
      g_warning("%s: nothing to offer; leaving the drag to XDND\n",
                __FUNCTION__);
      FreeItems();
      return FALSE;
   }

   for (i = 0; i < gDrag.nOverlays; i++) {
      MakeOverlay(&gDrag.overlays[i]);
   }

   /*
    * Wait on this module's own connection. Iterating the caller's main loop
    * here would dispatch RPC commands from inside an RPC handler. The first
    * sync brings the configure, and the second the commit that maps the
    * overlay.
    */
   deadline = g_get_monotonic_time() + WAYLAND_DRAG_REPLY_TIMEOUT_MS * 1000;
   r = SyncUntil(deadline);
   if (r == 1) {
      r = SyncUntil(deadline);
   }
   if (r < 0) {
      g_warning("%s: lost the compositor connection\n", __FUNCTION__);
      WaylandDrag_Exit();
      return FALSE;
   }
   if (r == 0) {
      g_warning("%s: the compositor did not answer in %ums\n", __FUNCTION__,
                (unsigned) WAYLAND_DRAG_REPLY_TIMEOUT_MS);
   }

   if (!gDrag.mapped) {
      g_debug("%s: the overlay did not map\n", __FUNCTION__);
      WaylandDrag_Disarm();
      return FALSE;
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_StartOnPress --
 *
 *    Read this module's connection until the press the caller faked arrives
 *    and PointerButton() turns it into a drag, or the deadline passes.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandDrag_StartOnPress(unsigned int timeoutMs,     // IN
                         WaylandDragDoneFunc done,   // IN
                         void *clientData)           // IN
{
   gint64 deadline = g_get_monotonic_time() + (gint64) timeoutMs * 1000;
   unsigned int startsBefore = gDrag.starts;

   if (!gDrag.inited || !gDrag.mapped) {
      return FALSE;
   }
   /*
    * Set the callback only once the drag is known to be running. Otherwise
    * a drag the compositor cancels inside this loop would run done(), this
    * function would also return FALSE, and the caller would clean up twice.
    */
   gDrag.done = NULL;

   while (gDrag.starts == startsBefore) {
      struct pollfd pfd;
      int left = (int) ((deadline - g_get_monotonic_time()) / 1000);
      int n;

      if (left <= 0) {
         break;
      }
      while (wl_display_prepare_read(gDrag.display) != 0) {
         if (wl_display_dispatch_pending(gDrag.display) < 0) {
            goto lost;
         }
         if (gDrag.starts != startsBefore) {
            /* prepare_read() failed, so there is no read to cancel. */
            break;
         }
      }
      if (gDrag.starts != startsBefore) {
         break;
      }
      wl_display_flush(gDrag.display);

      pfd.fd = wl_display_get_fd(gDrag.display);
      pfd.events = POLLIN;
      pfd.revents = 0;
      n = poll(&pfd, 1, left);
      if (n <= 0) {
         wl_display_cancel_read(gDrag.display);
         if (n < 0 && errno == EINTR) {
            continue;
         }
         break;
      }
      if (wl_display_read_events(gDrag.display) < 0 ||
          wl_display_dispatch_pending(gDrag.display) < 0) {
         goto lost;
      }
   }

   if (gDrag.starts == startsBefore) {
      g_debug("%s: no press reached the overlay in %ums\n", __FUNCTION__,
              timeoutMs);
      return FALSE;
   }
   if (!gDrag.dragging) {
      g_debug("%s: the drag ended as soon as it started\n", __FUNCTION__);
      return FALSE;
   }
   gDrag.done = done;
   gDrag.clientData = clientData;

   return TRUE;

lost:
   g_warning("%s: lost the compositor connection\n", __FUNCTION__);
   WaylandDrag_Exit();

   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_Pump --
 *
 *    Handle whatever the compositor has sent, without waiting.
 *
 *    Otherwise the connection is read only when the main loop reaches its
 *    watch, and the host's pointer motions come in bursts that the RPC
 *    channel handles in one go. A target that asks for the offer's contents
 *    on entering then waits through the whole burst. If the pointer has left
 *    the target by the time the contents arrive, GTK reports a failed
 *    transfer, and libfm 1.4.1 crashes on that.
 *
 *-----------------------------------------------------------------------------
 */

void
WaylandDrag_Pump(void)
{
   if (!gDrag.inited || !gDrag.dragging) {
      return;
   }
   if (ReadOnce(0, NULL) < 0) {
      g_warning("%s: lost the compositor connection\n", __FUNCTION__);
      WaylandDrag_Exit();
      return;
   }
   wl_display_flush(gDrag.display);
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_Disarm --
 *
 *    Discard an offer that never became a drag.
 *
 *-----------------------------------------------------------------------------
 */

void
WaylandDrag_Disarm(void)
{
   if (gDrag.display == NULL || gDrag.dragging) {
      return;
   }
   gDrag.done = NULL;
   if (gDrag.source != NULL) {
      wl_data_source_destroy(gDrag.source);
      gDrag.source = NULL;
   }
   FreeItems();
   Unmap();
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDrag_Cancel --
 *
 *    End the drag when the caller asks.
 *
 *    The caller moves on to the next drag as soon as the host says the last
 *    one is over, which can be well before the compositor says so. If the old
 *    drag kept running, its end would arrive in the middle of the next drag,
 *    and no new native drag could start meanwhile. Destroying the source ends
 *    the compositor's grab at once.
 *
 *-----------------------------------------------------------------------------
 */

void
WaylandDrag_Cancel(void)
{
   if (gDrag.display == NULL) {
      return;
   }
   if (!gDrag.dragging) {
      WaylandDrag_Disarm();
      return;
   }
   g_debug("%s: ending the drag\n", __FUNCTION__);
   Finish();
}
