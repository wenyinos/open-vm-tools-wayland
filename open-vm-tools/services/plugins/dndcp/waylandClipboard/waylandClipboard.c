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
 * waylandClipboard.c --
 *
 *    Own and read the Wayland selection through ext-data-control-v1.
 *
 *    This runs alongside GDK, which the plugin pins to its X11 backend for
 *    the drag detection window, so it keeps its own connection to the
 *    compositor and drives it from the GLib main loop.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define G_LOG_DOMAIN "dndcp"

#include "waylandClipboard.h"
#include "wayland-data-control.h"

#include <wayland-client.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

/*
 * The host accepts nothing larger than CPCLIPITEM_MAX_SIZE_V3 from
 * dndClipboard.h, so a larger selection is not worth buffering. The X
 * selection fallback in copyPasteUIWayland.cpp stops at the same limit.
 */
#include "dndClipboard.h"

/* A peer may advertise as many types as it likes; keep a bounded number. */
#define WAYLAND_CLIP_MAX_TYPES 256
#define WAYLAND_CLIP_MAX_MIME_LEN 256

/* How long to keep draining a selection that is already being dropped. */
#define WAYLAND_CLIP_DRAIN_US (200 * 1000)

/*
 * Time SourceSend() may spend writing to a pipe and waiting for its reader to
 * drain it. Every
 * send draws on one allowance, which refills at a tenth of the rate of wall
 * clock time. A burst of requests or a steady stream of them then cannot hold
 * the main loop for more than about a tenth of its time. A reader that keeps
 * up never waits long enough to reach the limit.
 */
#define WAYLAND_CLIP_SEND_BUDGET_US (2 * G_USEC_PER_SEC)
#define WAYLAND_CLIP_SEND_REFILL_DIV 10

static gint64 gSendBudget = WAYLAND_CLIP_SEND_BUDGET_US;
static gint64 gSendRefilled = 0;


typedef struct {
   char *mimeType;
   void *data;
   size_t size;
} ClipItem;

static struct {
   struct wl_display *display;
   struct wl_registry *registry;
   struct wl_seat *seat;
   /*
    * These are ext_data_control_* or zwlr_data_control_* depending on what
    * the compositor offers, so they are kept as the proxies they are. Every
    * call on them goes through wayland-data-control.h.
    */
   void *manager;
   void *device;
   const char *protoName;          /* For log messages. */

   /* What this client offers while it holds the selection. */
   void *source;
   ClipItem *items;
   size_t nItems;

   /* What the current selection owner offers. */
   void *offer;
   GPtrArray *offerTypes;          /* char *, owned by gClip.offer */
   guint64 selectionSerial;        /* Selection events seen. */

   guint watch;
   gboolean inited;
} gClip;


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

   for (i = 0; i < gClip.nItems; i++) {
      g_free(gClip.items[i].mimeType);
      g_free(gClip.items[i].data);
   }
   g_free(gClip.items);
   gClip.items = NULL;
   gClip.nItems = 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SourceSend --
 *
 *    A client is pasting. Write the bytes for the requested type.
 *
 *    Writing can block if the reader is slow or goes away, so the pipe is put
 *    into non-blocking mode and the wait for it is charged to a shared time
 *    allowance. A stuck reader must not stall the main loop, which also
 *    carries the RPC channel.
 *
 *-----------------------------------------------------------------------------
 */

static void
SourceSend(void *data,                                 // IN: unused
           void *source,                               // IN
           const char *mimeType,                       // IN
           int32_t fd)                                 // IN
{
   size_t i;
   struct stat st;

   /*
    * Only a pipe or a socket can be waited on with poll(). Close anything
    * else rather than write to it from the main loop.
    */
   if (fstat(fd, &st) != 0 || !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
      g_debug("%s: refusing %s to a descriptor that is not a pipe\n",
              __FUNCTION__, mimeType);
      close(fd);
      return;
   }

   for (i = 0; i < gClip.nItems; i++) {
      if (strcmp(gClip.items[i].mimeType, mimeType) == 0) {
         const char *p = gClip.items[i].data;
         size_t left = gClip.items[i].size;
         gint64 start = g_get_monotonic_time();
         gint64 deadline;
         gboolean timedOut = FALSE;
         int lastErrno = 0;

         if (gSendRefilled != 0) {
            gSendBudget += (start - gSendRefilled) /
                           WAYLAND_CLIP_SEND_REFILL_DIV;
            gSendBudget = MIN(gSendBudget, WAYLAND_CLIP_SEND_BUDGET_US);
         }
         gSendRefilled = start;
         deadline = start + gSendBudget;

         if (!g_unix_set_fd_nonblocking(fd, TRUE, NULL)) {
            g_debug("%s: could not make the pipe non-blocking\n", __FUNCTION__);
         }

         /*
          * Always write what fits without waiting, so a small selection is
          * still delivered when the allowance has run out.
          */
         while (left > 0) {
            ssize_t n = write(fd, p, left);

            if (n > 0) {
               p += n;
               left -= (size_t) n;
               continue;
            }
            lastErrno = errno;
            if (n < 0 && lastErrno == EINTR) {
               continue;
            }
            if (n < 0 && lastErrno == EAGAIN) {
               struct pollfd pfd;
               gint64 wait = deadline - g_get_monotonic_time();

               if (wait <= 0) {
                  timedOut = TRUE;
                  break;
               }
               pfd.fd = fd;
               pfd.events = POLLOUT;
               pfd.revents = 0;
               if (poll(&pfd, 1, (int) MIN(wait / 1000 + 1, 20)) > 0 &&
                   (pfd.revents & (POLLERR | POLLHUP))) {
                  /* The reader closed the pipe. */
                  lastErrno = EPIPE;
                  break;
               }
               continue;
            }
            break;
         }

         gSendBudget -= MIN(gSendBudget, g_get_monotonic_time() - start);

         if (left > 0) {
            /*
             * EPIPE here is routine. A clipboard manager may ask for a type,
             * decide it does not want it and close the pipe. Log any other
             * error as a warning.
             */
            if (lastErrno == EPIPE) {
               g_debug("%s: reader closed %s early, %" G_GSIZE_FORMAT
                       " bytes unsent\n", __FUNCTION__, mimeType, (gsize) left);
            } else {
               g_warning("%s: %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT
                         " bytes of %s unsent: %s\n", __FUNCTION__,
                         (gsize) left, (gsize) gClip.items[i].size, mimeType,
                         timedOut ? "reader too slow" :
                                    g_strerror(lastErrno));
            }
         }
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
 * SourceCancelled --
 *
 *    Another client took the selection. Destroy the source, and drop the
 *    items if it is still the current one.
 *
 *-----------------------------------------------------------------------------
 */

static void
SourceCancelled(void *data,   // IN: unused
                void *source) // IN
{
   if (gClip.source == source) {
      gClip.source = NULL;
      FreeItems();
   }
   WaylandDataSourceDestroy(source);
}


static const WaylandDataSourceListener gSourceListener = {
   .send = SourceSend,
   .cancelled = SourceCancelled,
};


/*
 *-----------------------------------------------------------------------------
 *
 * OfferType --
 *
 *    Collect the types the current owner advertises.
 *
 *    The type list is stored on the offer object rather than on gClip. The
 *    compositor describes a primary selection offer the same way as a
 *    regular selection offer, so a single global list would describe
 *    whichever offer arrived last.
 *
 *-----------------------------------------------------------------------------
 */

static void
OfferType(void *data,           // IN: unused
          void *offer,          // IN
          const char *mimeType) // IN
{
   GPtrArray *types;

   if (offer == NULL || mimeType == NULL) {
      return;
   }
   types = WaylandDataOfferGetUserData(offer);
   if (types == NULL) {
      return;
   }
   if (types->len >= WAYLAND_CLIP_MAX_TYPES) {
      g_debug("%s: more than %d types offered, ignoring %s\n", __FUNCTION__,
              WAYLAND_CLIP_MAX_TYPES, mimeType);
      return;
   }
   if (strlen(mimeType) > WAYLAND_CLIP_MAX_MIME_LEN) {
      g_debug("%s: over-long MIME type ignored\n", __FUNCTION__);
      return;
   }
   g_ptr_array_add(types, g_strdup(mimeType));
}


static const WaylandDataOfferListener gOfferListener = {
   .offer = OfferType,
};


/*
 *-----------------------------------------------------------------------------
 *
 * OfferDestroy --
 *
 *    Drop an offer along with the type list it owns.
 *
 * Side effects:
 *    This is the only place that frees the list, so callers must clear any
 *    borrowed pointer to it first.
 *
 *-----------------------------------------------------------------------------
 */

static void
OfferDestroy(void *offer)  // IN
{
   GPtrArray *types;

   if (offer == NULL) {
      return;
   }
   types = WaylandDataOfferGetUserData(offer);
   if (types != NULL) {
      WaylandDataOfferSetUserData(offer, NULL);
      g_ptr_array_free(types, TRUE);
   }
   WaylandDataOfferDestroy(offer);
}


static void
DeviceDataOffer(void *data,   // IN: unused
                void *device, // IN
                void *offer)  // IN
{
   /*
    * The compositor is describing a new offer, and its types arrive next.
    * Add the listener before setting the user data, because adding a
    * listener also sets the proxy user data.
    */
   WaylandDataOfferAddListener(offer, &gOfferListener, NULL);
   WaylandDataOfferSetUserData(offer, g_ptr_array_new_with_free_func(g_free));
}


static void
DeviceSelection(void *data,   // IN: unused
                void *device, // IN
                void *offer)  // IN
{
   if (gClip.offer != NULL && gClip.offer != offer) {
      gClip.offerTypes = NULL;
      OfferDestroy(gClip.offer);
   }
   gClip.offer = offer;
   gClip.offerTypes = offer != NULL ?
      WaylandDataOfferGetUserData(offer) : NULL;
   gClip.selectionSerial++;
}


static void
DevicePrimarySelection(void *data,   // IN: unused
                       void *device, // IN
                       void *offer)  // IN
{
   /* The primary selection is not mirrored to the host. */
   if (offer != NULL && offer != gClip.offer) {
      OfferDestroy(offer);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DeviceFinished --
 *
 *    The compositor has finished with the device, and any further request on
 *    it is a protocol error. Destroy it and mark the module unavailable, so
 *    the callers fall back to the X selection.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeviceFinished(void *data,   // IN: unused
               void *device) // IN
{
   g_debug("%s: the compositor finished the data device\n", __FUNCTION__);
   if (gClip.device != NULL) {
      WaylandDataControlDeviceDestroy(gClip.device);
      gClip.device = NULL;
   }
   gClip.inited = FALSE;
}


static const WaylandDataDeviceListener gDeviceListener = {
   .data_offer = DeviceDataOffer,
   .selection = DeviceSelection,
   .finished = DeviceFinished,
   .primary_selection = DevicePrimarySelection,
};


static void
RegistryGlobal(void *data,                  // IN: unused
               struct wl_registry *registry,// IN
               uint32_t name,               // IN
               const char *iface,           // IN
               uint32_t version)            // IN
{
   const WaylandDataControlProto *proto = WaylandDataControlMatch(iface);

   if (proto != NULL) {
      /*
       * Keep the first protocol that answers. ext-data-control-v1 comes
       * first in the list, so a compositor offering both is driven through
       * the standard protocol.
       */
      if (gClip.manager == NULL) {
         gClip.manager = WaylandDataControlBind(registry, name, proto, version);
         if (gClip.manager != NULL) {
            gClip.protoName = proto->name;
         }
      }
   } else if (strcmp(iface, wl_seat_interface.name) == 0 &&
              gClip.seat == NULL) {
      gClip.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
   }
}


static void
RegistryGlobalRemove(void *data,                  // IN: unused
                     struct wl_registry *registry,// IN
                     uint32_t name)               // IN
{
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
 *    Main loop callback for events from the compositor. Paste requests reach
 *    SourceSend() through here.
 *
 * Side effects:
 *    A dead display shuts the module down. IsAvailable() then returns FALSE,
 *    and the callers use the X selection instead of a connection that will
 *    never answer.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
DisplayReadable(gint fd,             // IN: unused
                GIOCondition cond,   // IN
                gpointer data)       // IN: unused
{
   if ((cond & (G_IO_HUP | G_IO_ERR)) != 0) {
      g_warning("%s: lost the compositor connection\n", __FUNCTION__);
      /*
       * Returning G_SOURCE_REMOVE removes the watch. Clear the id so that
       * Exit() does not remove it again.
       */
      gClip.watch = 0;
      WaylandClipboard_Exit();
      return G_SOURCE_REMOVE;
   }
   if (wl_display_dispatch(gClip.display) < 0) {
      g_warning("%s: dispatch failed: %s\n", __FUNCTION__, g_strerror(errno));
      gClip.watch = 0;
      WaylandClipboard_Exit();
      return G_SOURCE_REMOVE;
   }
   wl_display_flush(gClip.display);

   return G_SOURCE_CONTINUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_Init --
 *
 *    Connect to the compositor, bind ext-data-control-v1 and the seat, and
 *    watch the connection from the GLib main loop.
 *
 * Results:
 *    TRUE when the protocol is available. FALSE otherwise, and the caller
 *    then falls back to the X selection.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandClipboard_Init(void)
{
   if (gClip.inited) {
      return TRUE;
   }
   memset(&gClip, 0, sizeof gClip);

   gClip.display = wl_display_connect(NULL);
   if (gClip.display == NULL) {
      g_debug("%s: no Wayland display\n", __FUNCTION__);
      return FALSE;
   }

   gClip.registry = wl_display_get_registry(gClip.display);
   wl_registry_add_listener(gClip.registry, &gRegistryListener, NULL);
   wl_display_roundtrip(gClip.display);

   if (gClip.manager == NULL || gClip.seat == NULL) {
      g_debug("%s: no data control protocol is available\n", __FUNCTION__);
      wl_display_disconnect(gClip.display);
      memset(&gClip, 0, sizeof gClip);
      return FALSE;
   }

   gClip.device = WaylandDataControlGetDevice(gClip.manager, gClip.seat);
   WaylandDataControlDeviceAddListener(gClip.device, &gDeviceListener, NULL);
   wl_display_roundtrip(gClip.display);

   gClip.watch = g_unix_fd_add(wl_display_get_fd(gClip.display),
                               G_IO_IN | G_IO_HUP | G_IO_ERR,
                               DisplayReadable, NULL);
   gClip.inited = TRUE;
   g_debug("%s: using %s\n", __FUNCTION__, gClip.protoName);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_Exit --
 *
 *    Release every object and disconnect. DeviceFinished() clears the inited
 *    flag but keeps the display, so this checks the display rather than the
 *    inited flag to decide whether there is work to do.
 *
 *-----------------------------------------------------------------------------
 */

void
WaylandClipboard_Exit(void)
{
   if (gClip.display == NULL) {
      return;
   }
   if (gClip.watch != 0) {
      g_source_remove(gClip.watch);
   }
   if (gClip.source != NULL) {
      WaylandDataSourceDestroy(gClip.source);
   }
   FreeItems();
   gClip.offerTypes = NULL;
   OfferDestroy(gClip.offer);
   if (gClip.device != NULL) {
      WaylandDataControlDeviceDestroy(gClip.device);
   }
   if (gClip.manager != NULL) {
      WaylandDataControlManagerDestroy(gClip.manager);
   }
   if (gClip.seat != NULL) {
      wl_seat_destroy(gClip.seat);
   }
   if (gClip.registry != NULL) {
      wl_registry_destroy(gClip.registry);
   }
   wl_display_disconnect(gClip.display);
   memset(&gClip, 0, sizeof gClip);
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_IsAvailable --
 *
 *    Whether the selection is owned and read through ext-data-control-v1.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandClipboard_IsAvailable(void)
{
   return gClip.inited;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_IsOwner --
 *
 *    Whether this module holds the selection.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandClipboard_IsOwner(void)
{
   return gClip.inited && gClip.source != NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_GetSelectionSerial --
 *
 *    Count of the selection events seen so far. See DeviceSelection().
 *
 *-----------------------------------------------------------------------------
 */

guint64
WaylandClipboard_GetSelectionSerial(void)
{
   return gClip.selectionSerial;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_Offer --
 *
 *    Take the selection and offer every item. This copies the data, so the
 *    caller may free its buffers on return.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandClipboard_Offer(const WaylandClipItem *items,  // IN
                       size_t nItems)                 // IN
{
   size_t i;

   if (!gClip.inited || gClip.device == NULL || items == NULL || nItems == 0) {
      return FALSE;
   }

   if (gClip.source != NULL) {
      WaylandDataSourceDestroy(gClip.source);
      gClip.source = NULL;
   }
   FreeItems();

   gClip.items = g_new0(ClipItem, nItems);
   for (i = 0; i < nItems; i++) {
      gClip.items[i].mimeType = g_strdup(items[i].mimeType);
      if (items[i].data != NULL && items[i].size > 0) {
         gClip.items[i].data = g_malloc(items[i].size);
         memcpy(gClip.items[i].data, items[i].data, items[i].size);
         gClip.items[i].size = items[i].size;
      }
   }
   gClip.nItems = nItems;

   gClip.source = WaylandDataControlCreateSource(gClip.manager);
   WaylandDataSourceAddListener(gClip.source, &gSourceListener, NULL);
   for (i = 0; i < nItems; i++) {
      WaylandDataSourceOffer(gClip.source, items[i].mimeType);
   }
   WaylandDataControlSetSelection(gClip.device, gClip.source);
   wl_display_flush(gClip.display);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_HasType --
 *
 *    Whether mimeType is among the types the current selection owner offers.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
WaylandClipboard_HasType(const char *mimeType)  // IN
{
   guint i;

   if (!gClip.inited || gClip.offerTypes == NULL || mimeType == NULL) {
      return FALSE;
   }
   for (i = 0; i < gClip.offerTypes->len; i++) {
      if (strcmp((const char *) g_ptr_array_index(gClip.offerTypes, i),
                 mimeType) == 0) {
         return TRUE;
      }
   }

   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandClipboard_Receive --
 *
 *    Read the current selection as mimeType.
 *
 *    The owner writes at its own pace, so this polls the read against a
 *    deadline instead of blocking. It runs on the main loop that also carries
 *    the RPC channel, and a peer that stops writing must not stall that loop.
 *
 *-----------------------------------------------------------------------------
 */

void *
WaylandClipboard_Receive(const char *mimeType,    // IN
                         size_t *size,            // OUT
                         unsigned int timeoutMs)  // IN
{
   int fds[2];
   GByteArray *acc;
   gint64 deadline;
   gboolean dropped = FALSE;
   gboolean complete = FALSE;

   if (size != NULL) {
      *size = 0;
   }
   if (!gClip.inited || gClip.offer == NULL || mimeType == NULL) {
      return NULL;
   }
   if (!WaylandClipboard_HasType(mimeType)) {
      return NULL;
   }
   /*
    * pipe2() rather than g_unix_open_pipe(), which accepts O_CLOEXEC only
    * from GLib 2.78 on.
    */
   if (pipe2(fds, O_CLOEXEC) != 0) {
      g_debug("%s: pipe2 failed: %s\n", __FUNCTION__, g_strerror(errno));
      return NULL;
   }

   WaylandDataOfferReceive(gClip.offer, mimeType, fds[1]);
   wl_display_flush(gClip.display);
   close(fds[1]);

   if (!g_unix_set_fd_nonblocking(fds[0], TRUE, NULL)) {
      g_debug("%s: could not make the pipe non-blocking\n", __FUNCTION__);
   }

   acc = g_byte_array_new();
   deadline = g_get_monotonic_time() + (gint64) timeoutMs * 1000;

   for (;;) {
      char buf[16 * 1024];
      ssize_t n;

      /*
       * Check the deadline at the top of the loop. A peer that keeps the
       * pipe full never causes EAGAIN, and this loop holds the main loop
       * that also carries the RPC channel.
       */
      if (g_get_monotonic_time() >= deadline) {
         g_debug("%s: %s timed out after %ums\n", __FUNCTION__, mimeType,
                 timeoutMs);
         break;
      }

      n = read(fds[0], buf, sizeof buf);

      if (n > 0) {
         if (!dropped &&
             (gsize) acc->len + (gsize) n > (gsize) CPCLIPITEM_MAX_SIZE_V3) {
            /*
             * The host would refuse a selection past the ceiling. Keep
             * reading for a moment so the owner's write does not block on a
             * full pipe, but discard the bytes.
             */
            g_debug("%s: %s selection exceeds the V3 limit, dropping\n",
                    __FUNCTION__, mimeType);
            dropped = TRUE;
            deadline = MIN(deadline,
                           g_get_monotonic_time() + WAYLAND_CLIP_DRAIN_US);
         }
         if (!dropped) {
            g_byte_array_append(acc, (const guint8 *) buf, (guint) n);
         }
         continue;
      }
      if (n == 0) {
         complete = TRUE;              /* the owner finished */
         break;
      }
      if (errno == EAGAIN || errno == EINTR) {
         struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
         gint64 left = (deadline - g_get_monotonic_time()) / 1000;

         if (left > 0) {
            poll(&pfd, 1, (int) left);
         }
         continue;
      }
      break;
   }
   close(fds[0]);

   /*
    * A read cut short by the deadline or an error is incomplete. Returning
    * the part that arrived would send the host a truncated copy.
    */
   if (dropped || !complete) {
      g_byte_array_free(acc, TRUE);
      return NULL;
   }
   if (acc->len == 0) {
      g_byte_array_free(acc, TRUE);
      return NULL;
   }
   if (size != NULL) {
      *size = acc->len;
   }

   return g_byte_array_free(acc, FALSE);
}
