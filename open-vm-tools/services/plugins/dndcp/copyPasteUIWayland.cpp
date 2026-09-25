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

/**
 * @file copyPasteUIWayland.cpp
 *
 * Wayland implementation of host<->guest copy/paste, derived from
 * copyPasteUIX11.cpp. Rather than owning an X selection through GtkClipboard,
 * the local selection is owned and read through ext-data-control-v1 in
 * waylandClipboard.c. Where the compositor does not implement it, the
 * selection is owned and read on the X CLIPBOARD selection through Xwayland,
 * which GNOME keeps in step with the Wayland one. A data control client and
 * an X client need no keyboard focus, unlike the core wl_data_device
 * protocol that GtkClipboard uses on Wayland. That is what allows a
 * background daemon to bridge the clipboard.
 */

#define G_LOG_DOMAIN "dndcp"

#include <glib.h>
#include <glib-unix.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <dirent.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <gtk/gtk.h>

#include "vmware.h"
#include "copyPasteUIWayland.h"
#include "waylandClipboard.h"
#include "copyPasteDnDUtil.h"
#include "guestDnDCPMgr.hh"
#include "dndFileList.hh"
#include "dndMsg.h"

extern "C" {
#include "dynbuf.h"
#include "file.h"
#include "str.h"
#include "util.h"
#include "dndCPMsgV4.h"
#include "vmblock.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
}


/* What FileBlockMonitorThread passes to the main loop. */
typedef struct {
   CopyPasteUIWayland *cp;
   unsigned int generation;
} WaylandPasteReport;


/*
 * One request from the host can ask for several types in turn. Each read is
 * bounded, but a sequence of them is not, and this runs on the main loop that
 * also carries the RPC channel. Give the whole request one budget and let each
 * read have what is left of it.
 */
#define CP_REQUEST_BUDGET_MS 2000

/*
 * KDE keeps the copy or cut distinction out of the URI list, in a target of
 * its own that holds "1" for a cut and "0" for a copy.
 */
#define FCP_TARGET_NAME_KDE_CUT_SELECTION "application/x-kde-cutselection"
#define FCP_KDE_CUT_SELECTION_COPY        "0"


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandRemainingMs --
 *
 *    Time left until deadline.
 *
 * Results:
 *    Milliseconds, 0 once the deadline has passed.
 *
 *-----------------------------------------------------------------------------
 */

static unsigned int
CopyPasteWaylandRemainingMs(gint64 deadline)   // IN: monotonic, microseconds
{
   gint64 left = deadline - g_get_monotonic_time();

   if (left <= 0) {
      return 0;
   }
   return (unsigned int)(left / 1000);
}


/*
 * Reads through Xwayland where the compositor has no ext-data-control-v1.
 * See CopyPasteXReaderOpen().
 */
static Display *gXReader = NULL;
static Window gXReaderWindow = 0;
/* Each CopyPasteUIWayland that opened the reader holds one reference. */
static unsigned int gXReaderRefs = 0;

/*
 * Each read takes the next of these properties on the reader's window. An
 * owner that answers a read after its deadline, or that still waits to send
 * the next INCR chunk of an abandoned read, then writes to a property the
 * current read does not look at.
 */
#define CP_XREADER_PROPS 16
static unsigned int gXReaderSerial = 0;


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXReaderOpen --
 *
 *    Open an X connection to read the selection with, where the compositor
 *    has no ext-data-control-v1.
 *
 *    GNOME has no data control protocol, and a core Wayland client reads the
 *    selection only while it has the keyboard focus. Mutter keeps the X
 *    CLIPBOARD selection in step with the Wayland one and answers any X
 *    client for it, focused or not, so reading there needs no mapped window.
 *    An unmapped InputOnly window receives the data.
 *
 * Results:
 *    True if the connection is open.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteXReaderOpen(void)
{
   XSetWindowAttributes attrs;

   if (gXReader != NULL) {
      gXReaderRefs++;
      return true;
   }
   if (getenv("DISPLAY") == NULL || (gXReader = XOpenDisplay(NULL)) == NULL) {
      return false;
   }
   gXReaderRefs = 1;
   attrs.event_mask = PropertyChangeMask;
   gXReaderWindow = XCreateWindow(gXReader, DefaultRootWindow(gXReader),
                                  -10, -10, 1, 1, 0, CopyFromParent,
                                  InputOnly, CopyFromParent, CWEventMask,
                                  &attrs);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXReaderClose --
 *
 *    Drop one reference to the X connection, and close it with the last one.
 *
 *-----------------------------------------------------------------------------
 */

static void
CopyPasteXReaderClose(void)
{
   if (gXReader != NULL && --gXReaderRefs == 0) {
      XDestroyWindow(gXReader, gXReaderWindow);
      XCloseDisplay(gXReader);
      gXReader = NULL;
      gXReaderWindow = 0;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXReaderUsable --
 *
 *    Whether a read can go through X. Not when the X selection has no owner,
 *    which is also how it looks under a compositor that does not keep it in
 *    step with the Wayland one. Copy and paste then has nothing to read.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteXReaderUsable(void)
{
   return gXReader != NULL &&
          XGetSelectionOwner(gXReader,
                             XInternAtom(gXReader, "CLIPBOARD", False)) != None;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXReaderWait --
 *
 *    Wait until the deadline for the next event of the given type on the
 *    reader's window. For SelectionNotify, the event must answer a
 *    conversion to target into prop, or refuse one to target. For
 *    PropertyNotify, it must report a new value of prop. Other events are
 *    dropped.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteXReaderWait(int type,          // IN
                     Atom target,       // IN: SelectionNotify only
                     Atom prop,         // IN
                     gint64 deadline,   // IN: monotonic, microseconds
                     XEvent *event)     // OUT
{
   for (;;) {
      while (XPending(gXReader) > 0) {
         XNextEvent(gXReader, event);
         if (event->type == type && type == SelectionNotify &&
             event->xselection.requestor == gXReaderWindow &&
             event->xselection.target == target &&
             (event->xselection.property == prop ||
              event->xselection.property == None)) {
            return true;
         }
         if (event->type == type && type == PropertyNotify &&
             event->xproperty.window == gXReaderWindow &&
             event->xproperty.atom == prop &&
             event->xproperty.state == PropertyNewValue) {
            return true;
         }
      }

      unsigned int left = CopyPasteWaylandRemainingMs(deadline);
      struct pollfd pfd;

      if (left == 0) {
         return false;
      }
      pfd.fd = ConnectionNumber(gXReader);
      pfd.events = POLLIN;
      pfd.revents = 0;
      poll(&pfd, 1, (int) MIN(left, (unsigned int) G_MAXINT));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXReaderGet --
 *
 *    Convert the selection to target and return the bytes, following INCR
 *    for large ones. For a 32-bit format the items come back as longs, as
 *    Xlib stores them. Stops at the V3 size ceiling, as the data control path
 *    does.
 *
 * Results:
 *    The data, to be freed with g_free(), or NULL if the owner refused,
 *    did not answer before the deadline, or sent too much.
 *
 *-----------------------------------------------------------------------------
 */

static guchar *
CopyPasteXReaderGet(const char *target,  // IN
                    gint64 deadline,     // IN: monotonic, microseconds
                    gsize *outLen,       // OUT
                    Atom *outType)       // OUT: may be NULL
{
   Display *d = gXReader;
   Atom selection = XInternAtom(d, "CLIPBOARD", False);
   Atom targetAtom = XInternAtom(d, target, False);
   Atom incr = XInternAtom(d, "INCR", False);
   char propName[32];
   Atom prop;
   GByteArray *acc;
   XEvent event;
   bool incremental = false;

   *outLen = 0;

   g_snprintf(propName, sizeof propName, "VMWARE_CP_READ_%u",
              gXReaderSerial++ % CP_XREADER_PROPS);
   prop = XInternAtom(d, propName, False);

   /* Drop answers to earlier reads that ran out of time. */
   XSync(d, False);
   while (XPending(d) > 0) {
      XNextEvent(d, &event);
   }
   XDeleteProperty(d, gXReaderWindow, prop);
   XConvertSelection(d, selection, targetAtom, prop, gXReaderWindow,
                     CurrentTime);
   XFlush(d);
   if (!CopyPasteXReaderWait(SelectionNotify, targetAtom, prop, deadline,
                             &event) ||
       event.xselection.property == None) {
      return NULL;
   }

   acc = g_byte_array_new();
   for (;;) {
      Atom type = None;
      int format = 0;
      unsigned long items = 0;
      unsigned long after = 0;
      unsigned char *data = NULL;
      gsize bytes;

      if (XGetWindowProperty(d, gXReaderWindow, prop, 0, G_MAXLONG / 4, True,
                             AnyPropertyType, &type, &format, &items, &after,
                             &data) != Success) {
         break;
      }
      if (type == incr) {
         /* Deleting the property, done above, asks for the first chunk. */
         XFree(data);
         XFlush(d);
         incremental = true;
      } else {
         bytes = (gsize) items * (format == 32 ? sizeof(long) :
                                  (gsize) format / 8);
         if (outType != NULL) {
            *outType = type;
         }
         if (acc->len + bytes > (gsize) CPCLIPITEM_MAX_SIZE_V3) {
            g_debug("%s: %s exceeds the V3 limit, dropping\n", __FUNCTION__,
                    target);
            XFree(data);
            break;
         }
         g_byte_array_append(acc, data, (guint) bytes);
         XFree(data);
         XFlush(d);
         if (!incremental || bytes == 0) {
            *outLen = acc->len;
            return (guchar *) g_byte_array_free(acc, FALSE);
         }
      }
      if (!CopyPasteXReaderWait(PropertyNotify, None, prop, deadline,
                                &event)) {
         break;
      }
   }
   g_byte_array_free(acc, TRUE);
   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXReaderHasType --
 *
 *    Whether the X selection offers mimeType. Mutter names its X targets
 *    after the Wayland MIME types.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteXReaderHasType(const gchar *mimeType,  // IN
                        gint64 deadline)        // IN: monotonic, microseconds
{
   gsize len = 0;
   Atom type = None;
   guchar *data = CopyPasteXReaderGet("TARGETS", deadline, &len, &type);
   Atom wanted = XInternAtom(gXReader, mimeType, False);
   bool found = false;

   if (data != NULL && type == XA_ATOM) {
      const long *atoms = (const long *) data;

      for (gsize i = 0; i < len / sizeof(long); i++) {
         if ((Atom) atoms[i] == wanted) {
            found = true;
            break;
         }
      }
   }
   g_free(data);
   return found;
}


#ifndef GTK4
/*
 * What CopyPasteXWriterOffer() serves in GTK3. GtkClipboard passes the index
 * of a target as its info.
 */
typedef struct {
   std::vector<std::string> types;
   std::vector<std::string> data;
} CopyPasteXWriterContent;

/* The content GtkClipboard serves for this process, NULL once it lost it. */
static CopyPasteXWriterContent *gXWriterContent = NULL;


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXWriterGet --
 *
 *    GtkClipboardGetFunc. Answer a request for the target at index info.
 *
 *-----------------------------------------------------------------------------
 */

static void
CopyPasteXWriterGet(GtkClipboard *clipboard,      // IN: unused
                    GtkSelectionData *selection,  // OUT
                    guint info,                   // IN
                    gpointer userData)            // IN
{
   CopyPasteXWriterContent *content = (CopyPasteXWriterContent *) userData;

   if (info < content->data.size()) {
      gtk_selection_data_set(selection,
                             gtk_selection_data_get_target(selection), 8,
                             (const guchar *) content->data[info].data(),
                             (gint) content->data[info].size());
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXWriterFree --
 *
 *    GtkClipboardClearFunc. GtkClipboard no longer serves the content,
 *    because it was replaced or cleared, or another client took the
 *    selection.
 *
 *-----------------------------------------------------------------------------
 */

static void
CopyPasteXWriterFree(GtkClipboard *clipboard,  // IN: unused
                     gpointer userData)        // IN
{
   CopyPasteXWriterContent *content = (CopyPasteXWriterContent *) userData;

   if (gXWriterContent == content) {
      gXWriterContent = NULL;
   }
   delete content;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXWriterOffer --
 *
 *    Own the X CLIPBOARD selection through GDK and offer every item on it,
 *    where the compositor has no ext-data-control-v1.
 *
 *    On GNOME a Wayland client sets the selection only from a window with the
 *    keyboard focus, which shows in the dock, takes the focus and can take a
 *    Ctrl+V meant for the application. Mutter makes the X selection the
 *    Wayland selection, with every type the X owner offers, and asks the
 *    owner for the data when a Wayland client pastes. An X client needs no
 *    focus to own the selection.
 *
 *    GDK copies the data and serves it from the main loop until the
 *    selection is replaced, cleared or taken by another client. GDK on X11
 *    offers UTF8_STRING, STRING, TEXT and COMPOUND_TEXT for
 *    text/plain;charset=utf-8 by itself, so the GTK4 path leaves the
 *    UTF8_STRING item out. GTK3 serves only the targets it is given.
 *
 * Results:
 *    true if this process owns the selection with the items on it.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteXWriterOffer(gpointer clipboard,           // IN: from WatchSelection()
                      const WaylandClipItem *items, // IN
                      size_t count)                 // IN
{
#ifdef GTK4
   GdkContentProvider **providers = g_new(GdkContentProvider *, count);
   GdkContentProvider *content;
   size_t n = 0;
   bool ok;

   for (size_t i = 0; i < count; i++) {
      GBytes *bytes;

      if (strcmp(items[i].mimeType, "UTF8_STRING") == 0) {
         continue;
      }
      bytes = g_bytes_new(items[i].data, items[i].size);
      providers[n++] = gdk_content_provider_new_for_bytes(items[i].mimeType,
                                                          bytes);
      g_bytes_unref(bytes);
   }
   /* The union takes the providers and copies the array. */
   content = gdk_content_provider_new_union(providers, n);
   g_free(providers);
   ok = gdk_clipboard_set_content(GDK_CLIPBOARD(clipboard), content);
   g_object_unref(content);
   return ok;
#else
   CopyPasteXWriterContent *content = new CopyPasteXWriterContent;
   std::vector<GtkTargetEntry> targets(count);

   for (size_t i = 0; i < count; i++) {
      content->types.push_back(items[i].mimeType);
      content->data.push_back(std::string((const char *) items[i].data,
                                          items[i].size));
   }
   for (size_t i = 0; i < count; i++) {
      targets[i].target = (gchar *) content->types[i].c_str();
      targets[i].flags = 0;
      targets[i].info = (guint) i;
   }
   /* This frees the content it replaces through CopyPasteXWriterFree(). */
   if (!gtk_clipboard_set_with_data(GTK_CLIPBOARD(clipboard), &targets[0],
                                    (guint) count, CopyPasteXWriterGet,
                                    CopyPasteXWriterFree, content)) {
      delete content;
      return false;
   }
   gXWriterContent = content;
   return true;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXWriterIsOwner --
 *
 *    Whether the X selection holds what CopyPasteXWriterOffer() put on it.
 *    This turns false when another client takes the selection.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteXWriterIsOwner(gpointer clipboard)  // IN/OPT: from WatchSelection()
{
   if (clipboard == NULL) {
      return false;
   }
#ifdef GTK4
   return gdk_clipboard_is_local(GDK_CLIPBOARD(clipboard));
#else
   return gXWriterContent != NULL;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteXWriterClear --
 *
 *    Give up the selection if CopyPasteXWriterOffer() still owns it, and
 *    free what it served.
 *
 *-----------------------------------------------------------------------------
 */

static void
CopyPasteXWriterClear(gpointer clipboard)  // IN/OPT: from WatchSelection()
{
   if (!CopyPasteXWriterIsOwner(clipboard)) {
      return;
   }
#ifdef GTK4
   gdk_clipboard_set_content(GDK_CLIPBOARD(clipboard), NULL);
#else
   gtk_clipboard_clear(GTK_CLIPBOARD(clipboard));
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandHasType --
 *
 *    Return true when the local selection advertises mimeType. Callers check
 *    this before reading a type.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteWaylandHasType(const gchar *mimeType,  // IN
                        gint64 deadline)        // IN: monotonic, microseconds
{
   if (WaylandClipboard_IsAvailable()) {
      return WaylandClipboard_HasType(mimeType);
   }

   if (CopyPasteXReaderUsable()) {
      return CopyPasteXReaderHasType(mimeType, deadline);
   }
   return false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandGetData --
 *
 *    Read the local selection as mimeType, binary safe. Returns NULL and
 *    leaves *outLen at 0 for an empty, unreadable, oversized or late
 *    selection. The caller frees the result with g_free().
 *
 *-----------------------------------------------------------------------------
 */

static guchar *
CopyPasteWaylandGetData(const gchar *mimeType,  // IN
                        gint64 deadline,       // IN: monotonic, microseconds
                        gsize *outLen)          // OUT
{
   if (WaylandClipboard_IsAvailable()) {
      size_t n = 0;
      void *d = WaylandClipboard_Receive(mimeType, &n,
                                         CopyPasteWaylandRemainingMs(deadline));

      if (outLen != NULL) {
         *outLen = (gsize) n;
      }
      return (guchar *) d;
   }

   if (CopyPasteXReaderUsable()) {
      gsize n = 0;
      guchar *d = CopyPasteXReaderGet(mimeType, deadline, &n, NULL);

      if (d != NULL && n == 0) {
         g_free(d);
         d = NULL;
      }
      if (outLen != NULL) {
         *outLen = d != NULL ? n : 0;
      }
      return d;
   }

   if (outLen != NULL) {
      *outLen = 0;
   }
   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandGetText --
 *
 *    Read the local Wayland clipboard as text. Returns a newly allocated,
 *    NUL-terminated buffer, to be freed with g_free(), and stores its length
 *    without the terminator in *outLen. Returns NULL when the clipboard is
 *    empty, unreadable or not delivered before the deadline.
 *
 *-----------------------------------------------------------------------------
 */

static gchar *
CopyPasteWaylandGetText(gsize *outLen,   // OUT
                        gint64 deadline) // IN: monotonic, microseconds
{
   static const char *types[] = {
      "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", NULL
   };

   *outLen = 0;

   for (int i = 0; types[i] != NULL; i++) {
      gsize n = 0;
      guchar *d;

      if (!CopyPasteWaylandHasType(types[i], deadline)) {
         continue;
      }
      d = CopyPasteWaylandGetData(types[i], deadline, &n);
      if (d != NULL) {
         /* The caller treats this as a NUL terminated string. */
         gchar *text = g_strndup((const gchar *) d, n);

         g_free(d);
         *outLen = n;
         return text;
      }
   }
   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::CopyPasteUIWayland --
 *
 *    Constructor.
 *
 *-----------------------------------------------------------------------------
 */

CopyPasteUIWayland::CopyPasteUIWayland()
 : mCP(NULL),
   mInited(false),
   mWatchedClipboard(NULL),
   mWatchHandler(0),
   mXReaderOpen(false),
   mSelectionGeneration(0),
   mReadGeneration(G_MAXUINT64),
   mRetriedGeneration(G_MAXUINT64),
   mBlockCtrl(NULL),
   mBlockAdded(false),
   mBlockGeneration(0),
   mThread(0),
   mThreadStarted(false),
   mThreadInited(false)
{
   GuestDnDCPMgr *p = GuestDnDCPMgr::GetInstance();
   ASSERT(p);
   mCP = p->GetCopyPasteMgr();
   ASSERT(mCP);
   CPClipboard_Init(&mClipboard);
   mThreadParams.mainContext = NULL;
   mThreadParams.pendingPaste = NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::~CopyPasteUIWayland --
 *
 *    Destructor.
 *
 *-----------------------------------------------------------------------------
 */

CopyPasteUIWayland::~CopyPasteUIWayland()
{
   if (mWatchedClipboard != NULL) {
      g_signal_handler_disconnect(mWatchedClipboard, mWatchHandler);
      CopyPasteXWriterClear(XWriterClipboard());
      g_object_unref(mWatchedClipboard);
      mWatchedClipboard = NULL;
   }
   if (mXReaderOpen) {
      CopyPasteXReaderClose();
      mXReaderOpen = false;
   }

   /*
    * Remove the block first. The monitor thread may be blocked in a read on
    * the notify path, and removing the block ends that read. Joining before
    * that would wait for a paste that may never happen.
    */
   RemoveHGBlock();

   TerminateThread();

   /*
    * Destroy any report the thread queued before the join. It would
    * otherwise run after this object is gone.
    */
   if (mThreadParams.pendingPaste != NULL) {
      g_source_destroy(mThreadParams.pendingPaste);
      g_source_unref(mThreadParams.pendingPaste);
      mThreadParams.pendingPaste = NULL;
   }
   if (mThreadParams.mainContext != NULL) {
      g_main_context_unref(mThreadParams.mainContext);
      mThreadParams.mainContext = NULL;
   }

   WaylandClipboard_Exit();
   if (mThreadInited) {
      pthread_cond_destroy(&mThreadParams.fileBlockCond);
      pthread_mutex_destroy(&mThreadParams.fileBlockMutex);
      mThreadInited = false;
   }
   CPClipboard_Destroy(&mClipboard);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::Init --
 *
 *    Choose how to own and read the local selection, connect to the
 *    common-layer copy/paste signals and start the vmblock monitor thread.
 *    The RPC transport runs on the toolbox GLib main loop.
 *
 *-----------------------------------------------------------------------------
 */

bool
CopyPasteUIWayland::Init()
{
   if (mInited) {
      return true;
   }

   /*
    * Own the selection directly when the compositor implements
    * ext-data-control-v1. Otherwise, which is the case on GNOME, fall back to
    * the X selection, see WatchSelection().
    *
    * This has to happen before the baseline read below, or that read goes
    * through X even when the native path is the one in use.
    */
   if (WaylandClipboard_Init()) {
      g_debug("%s: using the native Wayland selection\n", __FUNCTION__);
   } else {
      g_debug("%s: ext-data-control-v1 unavailable, using the X selection\n",
              __FUNCTION__);
   }

   /*
    * Establish a baseline before the first host request. Otherwise the guest's
    * pre-existing clipboard looks new and overwrites a newer host clipboard as
    * soon as the VM receives focus.
    */
   WatchSelection();
   GetSelectionGeneration(&mReadGeneration);
   gsize len = 0;
   gchar *text = CopyPasteWaylandGetText(&len,
      g_get_monotonic_time() + CP_REQUEST_BUDGET_MS * 1000);
   mLastClipboardText.assign(text != NULL ? text : "", len);
   g_free(text);

   if (!WaylandClipboard_IsAvailable() && XWriterClipboard() == NULL) {
      g_warning("%s: the compositor does not offer ext-data-control-v1 and "
                "there is no X display. Copy and paste is disabled.\n",
                __FUNCTION__);
   }

   mCP->srcRecvClipChanged.connect(
      sigc::mem_fun(this, &CopyPasteUIWayland::GetRemoteClipboardCB));
   mCP->destRequestClipChanged.connect(
      sigc::mem_fun(this, &CopyPasteUIWayland::GetLocalClipboard));

   mCP->getFilesDoneChanged.connect(
      sigc::mem_fun(this, &CopyPasteUIWayland::GetLocalFilesDone));

   /*
    * The monitor thread lets a host to guest file paste wait until the user
    * pastes. Start it once. It waits on its condition variable until a block
    * is armed.
    */
   pthread_mutex_init(&mThreadParams.fileBlockMutex, NULL);
   pthread_cond_init(&mThreadParams.fileBlockCond, NULL);
   mThreadInited = true;
   mThreadParams.fileBlockCondExit = false;
   mThreadParams.cp = this;
   mThreadParams.fileBlockName.clear();
   mThreadParams.fileBlockGeneration = 0;
   /* Init runs on the main loop, where paste reports must be dispatched. */
   mThreadParams.mainContext = g_main_context_ref_thread_default();
   mThreadParams.pendingPaste = NULL;
   if (pthread_create(&mThread, NULL,
                      CopyPasteUIWayland::FileBlockMonitorThread,
                      &mThreadParams) == 0) {
      mThreadStarted = true;
   } else {
      g_warning("%s: could not start the file block monitor thread; "
                "host -> guest file paste will not work\n", __FUNCTION__);
   }

   mInited = true;
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::VmxCopyPasteVersionChanged --
 *
 *    Update version information in mCP.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::VmxCopyPasteVersionChanged(RpcChannel *chan,  // IN: unused
                                               uint32 version)    // IN
{
   ASSERT(mCP);
   g_debug("%s: new version is %d\n", __FUNCTION__, version);
   mCP->VmxCopyPasteVersionChanged(version);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::ClearLastClipboard --
 *
 *    Forget every format recorded as last sent to or received from the host.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::ClearLastClipboard(void)
{
   mLastClipboardText.clear();
   mLastClipboardImage.clear();
   mLastClipboardRtf.clear();
   mLastClipboardFiles.clear();
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::LocalFileOfferIsLive --
 *
 *    Whether the files of the host clip on the selection can still be
 *    pasted. A file list stays pasteable while its staging directory exists.
 *    GetLocalFilesDone() forgets the directory when a transfer fails. File
 *    contents stay pasteable while the files written out for them exist.
 *    Those are plain files, so checking them does not wait on vmblock.
 *
 * Results:
 *    true for a clip without files.
 *
 *-----------------------------------------------------------------------------
 */

bool
CopyPasteUIWayland::LocalFileOfferIsLive(const CPClipboard *clip)  // IN
{
   if (CPClipboard_ItemExists(clip, CPFORMAT_FILELIST)) {
      return !mHGStagingDir.empty() &&
             g_file_test(mHGStagingDir.c_str(), G_FILE_TEST_IS_DIR);
   }
   if (CPClipboard_ItemExists(clip, CPFORMAT_FILECONTENTS)) {
      if (mLastClipboardFiles.empty()) {
         return false;
      }
      for (size_t i = 0; i < mLastClipboardFiles.size(); i++) {
         if (!g_file_test(mLastClipboardFiles[i].c_str(),
                          G_FILE_TEST_EXISTS)) {
            return false;
         }
      }
   }
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::GetRemoteClipboardCB --
 *
 *    Invoked when the host sends clipboard contents. Offer what it sent on
 *    the local Wayland selection.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::GetRemoteClipboardCB(const CPClipboard *clip)  // IN
{
   void *buf = NULL;
   size_t sz = 0;

   if (clip == NULL) {
      g_debug("%s: no clipboard contents.\n", __FUNCTION__);
      return;
   }

   /*
    * The host sends its clipboard again each time the guest gets the focus,
    * changed or not. Installing it again makes a new selection, and a
    * clipboard manager such as Klipper takes that for a new copy and may
    * pop up its actions menu. Leave the selection alone when it still holds
    * the same clip.
    */
   {
      DynBuf serialized;
      std::string current;

      DynBuf_Init(&serialized);
      if (CPClipboard_Serialize(clip, &serialized)) {
         current.assign((const char *) DynBuf_Get(&serialized),
                        DynBuf_GetSize(&serialized));
      }
      DynBuf_Destroy(&serialized);
      /*
       * A clip with files is kept only while its files can still be pasted.
       * Installing it again stages them anew, and that removes the files a
       * paste in progress may still be reading.
       */
      if (!current.empty() && current == mLastHostClip &&
          LocalFileOfferIsLive(clip) &&
          (WaylandClipboard_IsOwner() ||
           CopyPasteXWriterIsOwner(XWriterClipboard()))) {
         g_debug("%s: the guest still offers this host clip; leaving it\n",
                 __FUNCTION__);
         return;
      }
      mLastHostClip = current;
   }

   /*
    * The host clipboard has changed, whether or not the clip can be
    * installed here. Forget the earlier clips, so that a later guest copy
    * equal to one of them still reaches the host. Each path below records
    * what it installs. A guest selection left in place when nothing is
    * installed is not read again until it changes. See GetLocalClipboard().
    */
   ClearLastClipboard();

   /*
    * Drop any block left over from a previous file clip. Without this,
    * DnD_CreateStagingDirectory() hands back the same still-empty directory
    * on the next file copy and AddBlock refuses it, so only the first file
    * paste of a session would ever work.
    */
   RemoveHGBlock();

   /*
    * Log every format the host offered. The host may attach a text rendering,
    * such as a filename or URL, to an image copy, so the order in which the
    * formats are consumed decides which one is used.
    */
   g_debug("%s: host clip offers:%s%s%s%s\n", __FUNCTION__,
           CPClipboard_ItemExists(clip, CPFORMAT_IMG_PNG)  ? " png"   : "",
           CPClipboard_ItemExists(clip, CPFORMAT_TEXT)     ? " text"  : "",
           CPClipboard_ItemExists(clip, CPFORMAT_RTF)      ? " rtf"   : "",
           CPClipboard_ItemExists(clip, CPFORMAT_FILELIST) ? " files" : "");

   /*
    * Files first. A file selection normally also carries a text rendering of
    * the paths, and consuming that would turn a file paste into a paste of
    * filenames. copyPasteUIX11.cpp checks text first here, and files first
    * only in the guest to host direction.
    */
   if (CPClipboard_ItemExists(clip, CPFORMAT_FILELIST) &&
       SetRemoteFileList(clip)) {
      return;
   }

   /*
    * File contents rather than paths. The host sends the bytes when the
    * source has no file on disk to point at. Write them out and publish the
    * staged paths as for any other file list.
    */
   if (CPClipboard_ItemExists(clip, CPFORMAT_FILECONTENTS) &&
       SetRemoteFileContents(clip)) {
      return;
   }

   /*
    * Offer every format the host sent in one selection, so an application
    * asking for rich text gets RTF while one asking for plain text gets the
    * text rendering. The X11 backend offers text and RTF together the same
    * way.
    */
   std::vector<WaylandClipItem> items;
   void *rtfBuf = NULL;
   size_t rtfSz = 0;
   void *imgBuf = NULL;
   size_t imgSz = 0;
   const char *text = NULL;
   gsize textLen = 0;
   bool haveImg = false;
   bool haveRtf = false;

   if (CPClipboard_GetItem(clip, CPFORMAT_IMG_PNG, &imgBuf, &imgSz) &&
       imgBuf != NULL && imgSz > 0) {
      haveImg = true;
      items.push_back({ "image/png", imgBuf, imgSz });
   }
   if (CPClipboard_GetItem(clip, CPFORMAT_RTF, &rtfBuf, &rtfSz) &&
       rtfBuf != NULL && rtfSz > 0) {
      haveRtf = true;
      items.push_back({ TARGET_NAME_TEXT_RTF, rtfBuf, rtfSz });
      items.push_back({ TARGET_NAME_APPLICATION_RTF, rtfBuf, rtfSz });
   }
   if (CPClipboard_GetItem(clip, CPFORMAT_TEXT, &buf, &sz) &&
       buf != NULL && sz > 0) {
      /*
       * CPFORMAT_TEXT is NUL terminated UTF-8 and sz includes the terminator.
       * Do not put the trailing NUL on the Wayland selection.
       */
      text = (const char *) buf;
      textLen = (gsize) sz;
      if (textLen > 0 && text[textLen - 1] == '\0') {
         textLen--;
      }
      items.push_back({ "text/plain;charset=utf-8", text, textLen });
      items.push_back({ "text/plain", text, textLen });
      items.push_back({ "UTF8_STRING", text, textLen });
   }

   if (items.empty()) {
      g_debug("%s: nothing usable in the remote clipboard\n", __FUNCTION__);
      return;
   }

   g_debug("%s: offering %d formats locally\n", __FUNCTION__,
           (int) items.size());

   bool ok;
   if (WaylandClipboard_IsAvailable()) {
      ok = WaylandClipboard_Offer(&items[0], items.size());
   } else {
      ok = XWriterClipboard() != NULL &&
           CopyPasteXWriterOffer(XWriterClipboard(), &items[0], items.size());
   }

   if (!ok) {
      g_warning("%s: could not install the local selection\n", __FUNCTION__);
      return;
   }

   /*
    * Record every format now on the local selection. Without this, the next
    * host request would read a format off this client's own selection, take
    * it for a new guest copy and send it back to the host.
    */
   if (haveImg) {
      mLastClipboardImage.assign((const char *) imgBuf, imgSz);
   }
   if (haveRtf) {
      mLastClipboardRtf.assign((const char *) rtfBuf, rtfSz);
   }
   if (text != NULL) {
      mLastClipboardText.assign(text, textLen);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::WatchSelection --
 *
 *    On the fallback, count changes to the selection, so that
 *    GetLocalClipboard() reads it only after one, and open the X reader.
 *    Host clips go on the watched clipboard once the X reader is open. See
 *    XWriterClipboard().
 *
 *    The host asks for the guest clipboard every few seconds while the
 *    pointer moves in and out of the VM. Mutter keeps the X CLIPBOARD
 *    selection in step with the Wayland one and sets its owner again on
 *    every change. GDK on X11 reports that as "changed" in GTK4 and as
 *    "owner-change" in GTK3, without reading anything. GDK runs on X11 here,
 *    since drag and drop needs it. The native backend is left out, because
 *    waylandClipboard.c counts the changes itself.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::WatchSelection(void)
{
   GdkDisplay *display = gdk_display_get_default();

   if (WaylandClipboard_IsAvailable() || display == NULL) {
      return;
   }
#ifdef GTK4
   mWatchedClipboard = g_object_ref(gdk_display_get_clipboard(display));
   mWatchHandler = g_signal_connect_swapped(mWatchedClipboard, "changed",
                                            G_CALLBACK(OnSelectionChanged),
                                            this);
#else
   mWatchedClipboard = g_object_ref(
      gtk_clipboard_get_for_display(display, GDK_SELECTION_CLIPBOARD));
   mWatchHandler = g_signal_connect_swapped(mWatchedClipboard, "owner-change",
                                            G_CALLBACK(OnSelectionChanged),
                                            this);
#endif
   mXReaderOpen = CopyPasteXReaderOpen();
   g_debug("%s: reading the selection only after it changes, %s\n",
           __FUNCTION__,
           mXReaderOpen ? "through X" : "but there is no X display");
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::OnSelectionChanged --
 *
 *    The watched clipboard reports a change. The signal is connected
 *    swapped, so the object comes first, and the GTK4 and GTK3 signals can
 *    share this handler although their other arguments differ.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::OnSelectionChanged(CopyPasteUIWayland *self)  // IN
{
   self->mSelectionGeneration++;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::GetSelectionGeneration --
 *
 *    Get a number that changes whenever the local selection changes. The
 *    native backend counts selection events. The fallback counts the
 *    notifications WatchSelection() set up.
 *
 * Results:
 *    false when nothing counts the changes, which is the fallback without an
 *    X display. Every request then reads the selection.
 *
 *-----------------------------------------------------------------------------
 */

bool
CopyPasteUIWayland::GetSelectionGeneration(guint64 *generation)  // OUT
{
   if (WaylandClipboard_IsAvailable()) {
      *generation = WaylandClipboard_GetSelectionSerial();
      return true;
   }
   if (mWatchedClipboard != NULL) {
      *generation = mSelectionGeneration;
      return true;
   }
   return false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::GetLocalClipboard --
 *
 *    Read the local selection and send it to the host. Files come first, then
 *    an image, then rich text with its plain text rendering, then plain text.
 *    When copy and paste is allowed, every request gets one reply, with an
 *    empty or not-changed clip when there is nothing new, so that the host
 *    request does not hang.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::GetLocalClipboard(void)
{
   /*
    * One budget for the whole request, shared by every read below. The host
    * asks once, and a local selection owner must not hold the main loop for
    * one timeout per type it advertises.
    */
   gint64 deadline = g_get_monotonic_time() + CP_REQUEST_BUDGET_MS * 1000;
   guint64 generation = 0;
   bool haveGeneration;
   gsize len = 0;
   gchar *text;
   std::string currentRtf;
   bool rtfInClip = false;
   bool textInClip = false;

   g_debug("%s: enter.\n", __FUNCTION__);

   if (!mCP->IsCopyPasteAllowed()) {
      g_debug("%s: copyPaste is not allowed\n", __FUNCTION__);
      return;
   }

   /*
    * When this client owns the selection, the selection holds what the host
    * sent, so there is nothing new to report. Reading it back would send the
    * host its own data. For a file paste the uri-list also points into a
    * staging directory that is still blocked, and reading it would stall
    * this main loop and the RPC channel that lifts the block. On the
    * fallback this process owns the X selection after
    * CopyPasteXWriterOffer(). GDK answers a read of it from this main loop,
    * which the read holds, so the read would also wait out the budget.
    */
   if (WaylandClipboard_IsOwner() ||
       CopyPasteXWriterIsOwner(XWriterClipboard())) {
      g_debug("%s: we own the local selection, nothing new for the host\n",
              __FUNCTION__);
      SendClipNotChanged();
      return;
   }

   /*
    * Skip the reads when the selection has not changed since the last read.
    * This also keeps a guest selection that was read before from being sent
    * again after a host clip that could not be installed. The generation is
    * taken before the reads, so that a change during them is read at the
    * next request.
    */
   haveGeneration = GetSelectionGeneration(&generation);
   if (haveGeneration) {
      if (generation == mReadGeneration) {
         g_debug("%s: the selection has not changed since the last read\n",
                 __FUNCTION__);
         SendClipNotChanged();
         return;
      }
      mReadGeneration = generation;
   }

   CPClipboard_Clear(&mClipboard);

   /*
    * Files first. A file selection also advertises a text rendering of the
    * paths, and taking that would turn a file copy into a copy of filenames.
    */
   if (mCP->CheckCapability(DND_CP_CAP_FILE_CP) &&
       (CopyPasteWaylandHasType(FCP_TARGET_NAME_URI_LIST, deadline) ||
        CopyPasteWaylandHasType(FCP_TARGET_NAME_GNOME_COPIED_FILES,
                                 deadline))) {
      switch (GetLocalFileList(deadline)) {
      case FILELIST_SET:
         SendLocalClip();
         return;
      case FILELIST_UNCHANGED:
         SendClipNotChanged();
         return;
      case FILELIST_UNUSABLE:
         g_debug("%s: uri-list present but unusable, falling through\n",
                 __FUNCTION__);
         break;
      }
   }

   /*
    * Then an image, as in copyPasteUIX11.cpp. An image selection often also
    * advertises a text/plain rendering, such as a filename or a URL, and
    * reading the text would lose the image.
    */
   if (mCP->CheckCapability(DND_CP_CAP_IMAGE_CP) &&
       CopyPasteWaylandHasType("image/png", deadline)) {
      gsize imgLen = 0;
      guchar *img = CopyPasteWaylandGetData("image/png", deadline, &imgLen);

      if (img != NULL) {
         std::string currentImage((const char *) img, imgLen);

         if (currentImage == mLastClipboardImage) {
            g_debug("%s: local clipboard image is unchanged\n", __FUNCTION__);
            g_free(img);
            SendClipNotChanged();
            return;
         }
         if (CPClipboard_SetItem(&mClipboard, CPFORMAT_IMG_PNG, img, imgLen)) {
            g_debug("%s: got %" G_GSIZE_FORMAT " bytes of PNG\n",
                    __FUNCTION__, imgLen);
            /*
             * Record the image only once it is on the outgoing clip. Caching
             * it before CPClipboard_SetItem() succeeds would make a failed
             * attempt look unchanged the next time, and the same image could
             * then never be copied again.
             */
            ClearLastClipboard();
            mLastClipboardImage = currentImage;
            g_free(img);
            SendLocalClip();
            return;
         }
         g_debug("%s: failed to set PNG item, falling back to text\n",
                 __FUNCTION__);
         g_free(img);
      }
   }

   /*
    * Rich text before plain text. A rich text selection also offers a plain
    * rendering, and taking only that would drop the formatting on the way to
    * the host. The plain rendering goes in the same clip below, as the X11
    * backends send it, for a host application that takes plain text only.
    */
   if (mCP->CheckCapability(DND_CP_CAP_RTF_CP)) {
      const char *rtfTypes[] = {
         TARGET_NAME_TEXT_RTF, TARGET_NAME_APPLICATION_RTF, NULL
      };
      int i;

      for (i = 0; rtfTypes[i] != NULL && !rtfInClip; i++) {
         gsize rtfLen = 0;
         guchar *rtf;

         if (!CopyPasteWaylandHasType(rtfTypes[i], deadline)) {
            continue;
         }
         rtf = CopyPasteWaylandGetData(rtfTypes[i], deadline, &rtfLen);
         if (rtf == NULL || rtfLen == 0) {
            g_free(rtf);
            continue;
         }

         currentRtf.assign((const char *) rtf, rtfLen);
         if (currentRtf == mLastClipboardRtf) {
            g_debug("%s: local clipboard rtf is unchanged\n", __FUNCTION__);
            g_free(rtf);
            SendClipNotChanged();
            return;
         }
         if (CPClipboard_SetItem(&mClipboard, CPFORMAT_RTF, rtf, rtfLen)) {
            g_debug("%s: got %" G_GSIZE_FORMAT " bytes of %s\n", __FUNCTION__,
                    rtfLen, rtfTypes[i]);
            rtfInClip = true;
         }
         g_free(rtf);
      }
   }

   text = CopyPasteWaylandGetText(&len, deadline);

   if (text == NULL && !rtfInClip &&
       CopyPasteWaylandRemainingMs(deadline) == 0) {
      /*
       * The reads ran out of time, so this request learned nothing about the
       * selection. Read it again at the next request rather than wait for
       * the next change, which may never come. Only once per change,
       * because an owner that never answers would otherwise cost the full
       * budget on the main loop at every request.
       */
      g_debug("%s: the reads ran out of time\n", __FUNCTION__);
      if (haveGeneration && mRetriedGeneration != generation) {
         mRetriedGeneration = generation;
         mReadGeneration = G_MAXUINT64;
      }
      SendClipNotChanged();
      return;
   }

   /*
    * The generation above says that something changed. Copying the same
    * text again is such a change, and so is a clipboard manager taking over
    * the selection this client installed. So the content is compared too,
    * and the same text again reads as unchanged.
    */
   std::string currentText(text != NULL ? text : "", len);
   if (!rtfInClip && currentText == mLastClipboardText) {
      g_debug("%s: local clipboard is unchanged\n", __FUNCTION__);
      g_free(text);
      SendClipNotChanged();
      return;
   }
   if (text != NULL && len > 0) {
      if (!mCP->CheckCapability(DND_CP_CAP_PLAIN_TEXT_CP)) {
         g_debug("%s: plain text copy/paste not in caps\n", __FUNCTION__);
      } else if (!g_utf8_validate(text, len, NULL)) {
         g_debug("%s: clipboard data is not valid UTF-8 text, skipping\n",
                 __FUNCTION__);
      } else if (len + 1 > (gsize) CPCLIPITEM_MAX_SIZE_V3) {
         g_debug("%s: clipboard text too large (%" G_GSIZE_FORMAT " bytes)\n",
                 __FUNCTION__, len);
      } else if (CPClipboard_SetItem(&mClipboard, CPFORMAT_TEXT, text, len + 1)) {
         textInClip = true;
         g_debug("%s: got %" G_GSIZE_FORMAT " bytes of text\n", __FUNCTION__, len);
      }
   }
   g_free(text);

   if (rtfInClip || textInClip) {
      /*
       * Record what is on the outgoing clip, for the same reason as the
       * image above.
       */
      ClearLastClipboard();
      if (rtfInClip) {
         mLastClipboardRtf = currentRtf;
      }
      if (textInClip) {
         mLastClipboardText = currentText;
      }
   } else {
      g_debug("%s: no text, sending empty clip back.\n", __FUNCTION__);
   }
   SendLocalClip();
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandParseUriList --
 *
 *    Split a text/uri-list, or the x-special/gnome-copied-files rendering of
 *    one, into unescaped paths.
 *
 *    The same file selection arrives spelled several ways, with CRLF or LF
 *    line ends and with or without an operation line in front. Comparing
 *    parsed paths rather than bytes lets this client recognise a selection
 *    it published when it comes back in another rendering.
 *
 *-----------------------------------------------------------------------------
 */

static void
CopyPasteWaylandParseUriList(const std::string &source,        // IN
                             std::vector<std::string> &paths)  // OUT
{
   size_t first = source.find_first_not_of("\r\n ");
   size_t pos = 0;

   paths.clear();

   while (pos < source.size()) {
      size_t end = source.find_first_of("\r\n", pos);
      size_t start = pos;
      size_t index = 0;
      size_t len = 0;
      char *path;

      if (end == std::string::npos) {
         end = source.size();
      }
      pos = end + 1;

      start = source.find_first_not_of(' ', start);
      if (start == std::string::npos || start >= end) {
         continue;
      }

      std::string line = source.substr(start, end - start);

      /*
       * Some desktops prefix the list with the intended operation. KDE
       * signals this out of band with application/x-kde-cutselection
       * instead. Skip the prefix so one code path serves both.
       */
      if (start == first && (line == "copy" || line == "cut")) {
         continue;
      }

      /*
       * Take each entry on its own. DnD_UriListGetNextFile() stops at the
       * first entry it does not support, such as a trash: or recent: URI,
       * and the entries after it would be lost. A '#' line is a comment.
       */
      if (line[0] == '#' ||
          (line.compare(0, sizeof DND_URI_LIST_PRE_KDE - 1,
                        DND_URI_LIST_PRE_KDE) != 0 &&
           !DnD_UriIsNonFileSchemes(line.c_str()))) {
         g_debug("%s: skipping an unsupported entry\n", __FUNCTION__);
         continue;
      }

      path = DnD_UriListGetNextFile(line.c_str(), &index, &len);
      if (path != NULL) {
         /* Keep the unescaped length, because the path may contain a NUL. */
         paths.push_back(std::string(path, len));
         free(path);
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandNormalizePath --
 *
 *    Collapse repeated and trailing separators so a path can be compared
 *    against a directory by prefix.
 *
 * Results:
 *    false for a relative path or one with a "." or ".." component. File
 *    managers do not put those on a clipboard, and resolving ".." lexically
 *    disagrees with the kernel whenever a symlink precedes it, so such a
 *    path is refused.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteWaylandNormalizePath(const std::string &path,  // IN
                              std::string &out)         // OUT
{
   size_t i = 0;

   out.clear();
   if (path.empty() || path[0] != '/') {
      return false;
   }
   while (i < path.size()) {
      size_t end;

      while (i < path.size() && path[i] == '/') {
         i++;
      }
      if (i == path.size()) {
         break;
      }
      end = path.find('/', i);
      if (end == std::string::npos) {
         end = path.size();
      }

      std::string comp = path.substr(i, end - i);
      if (comp == "." || comp == "..") {
         return false;
      }
      out += '/';
      out += comp;
      i = end;
   }
   if (out.empty()) {
      out = "/";
   }
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandHasControlChar --
 *
 *    Whether s holds an ASCII control character.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteWaylandHasControlChar(const std::string &s)  // IN
{
   size_t i;

   for (i = 0; i < s.size(); i++) {
      unsigned char c = (unsigned char) s[i];

      if (c < 0x20 || c == 0x7f) {
         return true;
      }
   }
   return false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandIsUnderVmblock --
 *
 *    Is path inside the vmblock file system?
 *
 *    A lookup of a blocked entry there does not return until the block is
 *    lifted, and GetLocalFilesDone() lifts it from the main loop. A stat of
 *    such a path from the main loop would therefore never return. The host
 *    has no reason to pull anything under the mount, since the entries are
 *    its own files on their way in.
 *
 *    /var/run is normally a symlink to /run, so both spellings are checked.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteWaylandIsUnderVmblock(const std::string &path,  // IN: normalized
                               const char *blockRoot)    // IN/OPT
{
   const char *roots[] = {
      VMBLOCK_MOUNT_POINT,
      VMBLOCK_FUSE_MOUNT_POINT,
      VMBLOCK_FS_ROOT,
      VMBLOCK_FUSE_NOTIFY_ROOT,
      blockRoot,
   };
   size_t i;

   for (i = 0; i < G_N_ELEMENTS(roots); i++) {
      std::string root;
      int alias;

      if (roots[i] == NULL || roots[i][0] != '/' ||
          !CopyPasteWaylandNormalizePath(roots[i], root)) {
         continue;
      }
      for (alias = 0; alias < 2; alias++) {
         if (alias == 1) {
            if (root.compare(0, 9, "/var/run/") != 0) {
               break;
            }
            root.erase(0, 4);
         }
         if (path == root ||
             (path.size() > root.size() &&
              path.compare(0, root.size(), root) == 0 &&
              path[root.size()] == '/')) {
            return true;
         }
      }
   }
   return false;
}


/*
 * Sizing a guest file selection for the host.
 *
 * The paths come from whichever client owns the selection. Sizing a
 * directory means walking it, and "/" is a valid entry. A lookup that passes
 * through a symlink into the vmblock mount also blocks until the main loop
 * lifts the block. The walk therefore runs on a thread of its own, and the
 * main loop waits for it only until the request deadline. A walk still
 * running then is told to stop, and finishes on its own. The walk does not
 * follow symlinks and opens nothing but directories, so a FIFO cannot stall
 * it. The host uses the size for information only, so a partial count is
 * sent rather than no list.
 */

#define CP_SIZE_MAX_ENTRIES 100000
#define CP_SIZE_MAX_DEPTH   64

typedef struct {
   gint refs;
   gint cancelled;
   GMutex lock;
   GCond cond;
   bool done;
   uint64 total;           /* Protected by lock. */
   unsigned int entries;   /* Only touched by the walker. */
   std::vector<std::string> paths;
} CopyPasteSizeJob;

/*
 * At most one walk runs at a time. A walk that never finishes stops later
 * sizing instead of adding threads.
 */
static gint gSizeJobsRunning = 0;

/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteSizeJobUnref --
 *
 *    Drop one reference to job, and free it with the last one.
 *
 *-----------------------------------------------------------------------------
 */

static void
CopyPasteSizeJobUnref(CopyPasteSizeJob *job)  // IN
{
   if (g_atomic_int_dec_and_test(&job->refs)) {
      g_mutex_clear(&job->lock);
      g_cond_clear(&job->cond);
      delete job;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteSizeJobStop --
 *
 *    Whether the walk should stop, because the main loop gave up waiting for it
 *    or it has visited CP_SIZE_MAX_ENTRIES entries.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteSizeJobStop(CopyPasteSizeJob *job)  // IN
{
   return g_atomic_int_get(&job->cancelled) ||
          job->entries >= CP_SIZE_MAX_ENTRIES;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteSizeDir --
 *
 *    Add up the sizes of the regular files under a directory, down to
 *    CP_SIZE_MAX_DEPTH levels, without following symlinks.
 *
 * Results:
 *    The total in bytes. dirFd is closed.
 *
 *-----------------------------------------------------------------------------
 */

static uint64
CopyPasteSizeDir(CopyPasteSizeJob *job,  // IN
                 int dirFd,              // IN: consumed
                 unsigned int depth)     // IN
{
   DIR *dir = fdopendir(dirFd);
   struct dirent *ent;
   uint64 total = 0;

   if (dir == NULL) {
      close(dirFd);
      return 0;
   }
   while (!CopyPasteSizeJobStop(job) && (ent = readdir(dir)) != NULL) {
      struct stat st;

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
         continue;
      }
      job->entries++;
      if (fstatat(dirfd(dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
         continue;
      }
      if (S_ISREG(st.st_mode)) {
         total += (uint64) st.st_size;
      } else if (S_ISDIR(st.st_mode) && depth < CP_SIZE_MAX_DEPTH) {
         int fd = openat(dirfd(dir), ent->d_name,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK |
                         O_CLOEXEC);

         if (fd >= 0) {
            total += CopyPasteSizeDir(job, fd, depth + 1);
         }
      }
   }
   closedir(dir);
   return total;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteSizeThread --
 *
 *    The thread that walks job->paths. It adds each path's size to job->total,
 *    signals job->cond when it is done, and drops its reference to job.
 *
 *-----------------------------------------------------------------------------
 */

static gpointer
CopyPasteSizeThread(gpointer data)  // IN
{
   CopyPasteSizeJob *job = (CopyPasteSizeJob *) data;
   size_t i;

   for (i = 0; i < job->paths.size() && !CopyPasteSizeJobStop(job); i++) {
      const char *path = job->paths[i].c_str();
      struct stat st;
      uint64 size = 0;

      job->entries++;
      if (fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) != 0) {
         continue;
      }
      if (S_ISREG(st.st_mode)) {
         size = (uint64) st.st_size;
      } else if (S_ISDIR(st.st_mode)) {
         int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK |
                             O_CLOEXEC);

         if (fd >= 0) {
            size = CopyPasteSizeDir(job, fd, 0);
         }
      }

      g_mutex_lock(&job->lock);
      job->total += size;
      g_mutex_unlock(&job->lock);
   }

   g_mutex_lock(&job->lock);
   job->done = true;
   g_cond_signal(&job->cond);
   g_mutex_unlock(&job->lock);

   g_atomic_int_add(&gSizeJobsRunning, -1);
   CopyPasteSizeJobUnref(job);
   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandSizePaths --
 *
 *    Add up the sizes of paths, giving up at deadline.
 *
 * Results:
 *    The total counted, which is short of the truth when the walk was cut
 *    off or could not be started.
 *
 *-----------------------------------------------------------------------------
 */

static uint64
CopyPasteWaylandSizePaths(const std::vector<std::string> &paths,  // IN
                          gint64 deadline)                        // IN
{
   CopyPasteSizeJob *job;
   GThread *thread;
   GError *err = NULL;
   uint64 total;
   bool done;

   if (paths.empty()) {
      return 0;
   }
   if (g_atomic_int_get(&gSizeJobsRunning) > 0) {
      g_debug("%s: an earlier walk is still running, not sizing\n",
              __FUNCTION__);
      return 0;
   }

   job = new CopyPasteSizeJob;
   job->refs = 2;
   job->cancelled = 0;
   g_mutex_init(&job->lock);
   g_cond_init(&job->cond);
   job->done = false;
   job->total = 0;
   job->entries = 0;
   job->paths = paths;

   g_atomic_int_inc(&gSizeJobsRunning);
   thread = g_thread_try_new("dndcp-size", CopyPasteSizeThread, job, &err);
   if (thread == NULL) {
      g_debug("%s: could not start the walk: %s\n", __FUNCTION__,
              err != NULL ? err->message : "unknown error");
      g_clear_error(&err);
      g_atomic_int_add(&gSizeJobsRunning, -1);
      job->refs = 1;
      CopyPasteSizeJobUnref(job);
      return 0;
   }
   g_thread_unref(thread);

   g_mutex_lock(&job->lock);
   while (!job->done) {
      if (!g_cond_wait_until(&job->cond, &job->lock, deadline)) {
         break;
      }
   }
   done = job->done;
   total = job->total;
   g_mutex_unlock(&job->lock);

   if (!done) {
      g_debug("%s: walk cut off at the deadline\n", __FUNCTION__);
      g_atomic_int_set(&job->cancelled, 1);
   }
   CopyPasteSizeJobUnref(job);
   return total;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::GetLocalFileList --
 *
 *    Turn the local text/uri-list selection into a CPFORMAT_FILELIST item for
 *    the host. No vmblock is involved in this direction, because the host
 *    pulls the contents itself once it has the list.
 *
 *    Mirrors CopyPasteUIX11::LocalGetSelectionFileList.
 *
 * Results:
 *    FILELIST_SET when a file list was placed on mClipboard, FILELIST_UNCHANGED
 *    when the selection is the one already reported to the host, and
 *    FILELIST_UNUSABLE when there is nothing here to send. The caller sends
 *    the reply in every case, so that exactly one reply reaches the host.
 *
 * Side effects:
 *    May add an item to mClipboard.
 *
 *-----------------------------------------------------------------------------
 */

CopyPasteUIWayland::FileListResult
CopyPasteUIWayland::GetLocalFileList(gint64 deadline)  // IN: monotonic, us
{
   gsize uriLen = 0;
   guchar *uriData = CopyPasteWaylandGetData(FCP_TARGET_NAME_URI_LIST,
                                             deadline, &uriLen);

   if (uriData == NULL) {
      /*
       * A selection may offer only the rendering that carries the operation
       * with the URIs, and no plain list. That is all some file managers put
       * on the clipboard.
       */
      uriData = CopyPasteWaylandGetData(FCP_TARGET_NAME_GNOME_COPIED_FILES,
                                        deadline, &uriLen);
   }

   if (uriData == NULL) {
      return FILELIST_UNUSABLE;
   }

   std::vector<std::string> paths;

   CopyPasteWaylandParseUriList(std::string((const char *) uriData, uriLen),
                                paths);
   g_free(uriData);
   if (paths.empty()) {
      return FILELIST_UNUSABLE;
   }

   /*
    * Check this before touching the file system. When this is the list
    * published for a host paste, its paths are under a block that the main
    * loop has not lifted yet.
    */
   if (paths == mLastClipboardFiles) {
      g_debug("%s: local clipboard file list is unchanged\n", __FUNCTION__);
      return FILELIST_UNCHANGED;
   }

   DnDFileList fileList;
   std::vector<std::string> usable;
   uint64 totalSize = 0;
   unsigned int count = 0;
   const char *blockRoot = mBlockCtrl != NULL ? mBlockCtrl->blockRoot : NULL;
   size_t i;

   for (i = 0; i < paths.size(); i++) {
      std::string path;

      if (DnD_UriIsNonFileSchemes(paths[i].c_str())) {
         /*
          * ssh, sftp, smb, dav and ftp URIs. The X11 backend maps these to
          * local gvfs paths with GFile. This file does not use GIO, so skip
          * them.
          */
         g_debug("%s: skipping non-file uri\n", __FUNCTION__);
         continue;
      }
      if (!CopyPasteWaylandNormalizePath(paths[i], path)) {
         g_debug("%s: skipping a relative path or one with . or ..\n",
                 __FUNCTION__);
         continue;
      }
      /*
       * The host names its copy after the last component, so the path must
       * have one, and it must be a plain name. Unescaping can put a line
       * break or a NUL into it.
       */
      if (path == "/" || CopyPasteWaylandHasControlChar(paths[i])) {
         g_debug("%s: skipping a path with no usable name\n", __FUNCTION__);
         continue;
      }
      if (CopyPasteWaylandIsUnderVmblock(path, blockRoot)) {
         g_debug("%s: skipping %s, which is inside vmblock\n", __FUNCTION__,
                 path.c_str());
         continue;
      }

      const char *relPath = strrchr(path.c_str(), DIRSEPC) + 1;

      fileList.AddFile(path.c_str(), relPath);
      usable.push_back(path);
      count++;
   }

   if (count == 0) {
      g_debug("%s: uri list produced no usable paths\n", __FUNCTION__);
      return FILELIST_UNUSABLE;
   }
   totalSize = CopyPasteWaylandSizePaths(usable, deadline);

   DynBuf buf;
   DynBuf_Init(&buf);
   fileList.SetFileSize(totalSize);
   fileList.ToCPClipboard(&buf, false);

   bool ok = CPClipboard_SetItem(&mClipboard, CPFORMAT_FILELIST,
                                 DynBuf_Get(&buf), DynBuf_GetSize(&buf));
   DynBuf_Destroy(&buf);

   if (!ok) {
      g_warning("%s: could not set the file list item\n", __FUNCTION__);
      return FILELIST_UNUSABLE;
   }

   g_debug("%s: sending %u file(s), %" FMT64 "u bytes total\n",
           __FUNCTION__, count, totalSize);

   ClearLastClipboard();
   mLastClipboardFiles = paths;
   return FILELIST_SET;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::FileBlockMonitorThread --
 *
 *    Wait for the user to paste. After SetRemoteFileList() publishes a URI
 *    list pointing into the vmblock file system, nothing has been transferred
 *    yet. When the pasting application opens one of those paths,
 *    vmblock-fuse stalls its open(2) and the read(2) here returns. The main
 *    loop then asks the host for the bytes. A host-side copy of a large file
 *    therefore costs nothing until it is pasted.
 *
 *    Ported from CopyPasteUIX11::FileBlockMonitorThread.
 *
 *-----------------------------------------------------------------------------
 */

void *
CopyPasteUIWayland::FileBlockMonitorThread(void *arg)  // IN
{
   WaylandThreadParams *params = (WaylandThreadParams *) arg;

   pthread_mutex_lock(&params->fileBlockMutex);
   while (!params->fileBlockCondExit) {
      /*
       * Test the predicate before waiting and again after every wake.
       * TerminateThread() can signal while this thread is in the read below
       * rather than on the condition variable. A wait entered without this
       * check would miss that signal, and the join would never return.
       */
      if (params->fileBlockName.empty()) {
         g_debug("%s: waiting for a block to monitor\n", __FUNCTION__);
         pthread_cond_wait(&params->fileBlockCond, &params->fileBlockMutex);
         continue;
      }

      /*
       * The read below blocks until the block is touched or lifted, which may
       * be never. Take a copy of the path and drop the mutex first, or
       * TerminateThread() cannot acquire it to ask this thread to exit.
       */
      std::string blockName = params->fileBlockName;
      unsigned int generation = params->fileBlockGeneration;

      /*
       * Consume the request. The path is armed for this pass only, so a wake
       * with no new block sends the thread back to waiting rather than
       * reopening the path just handled.
       */
      params->fileBlockName.clear();
      pthread_mutex_unlock(&params->fileBlockMutex);

      int fd = open(blockName.c_str(), O_RDONLY);
      if (fd < 0) {
         g_debug("%s: failed to open %s: %s\n", __FUNCTION__,
                 blockName.c_str(), g_strerror(errno));
         pthread_mutex_lock(&params->fileBlockMutex);
         continue;
      }

      char buf[sizeof(VMBLOCK_FUSE_READ_RESPONSE)];
      ssize_t size = read(fd, buf, sizeof buf);

      /*
       * The read above blocks until another process touches the blocked path
       * or the block is lifted. The content of the response does not matter.
       */
      g_debug("%s: notify read returned %" FMTSZ "d\n", __FUNCTION__, size);

      close(fd);

      /*
       * Lock before checking the exit flag. The destructor removes the block,
       * which ends the read, and only then asks this thread to exit, so the
       * request can arrive at any point above.
       */
      pthread_mutex_lock(&params->fileBlockMutex);
      if (params->fileBlockCondExit) {
         break;
      }

      /*
       * Report to the main loop, with the generation of the block this read
       * was for. The block may have been lifted or replaced by the time the
       * report is dispatched. This report replaces any earlier one that has
       * not been dispatched.
       */
      WaylandPasteReport *report = g_new(WaylandPasteReport, 1);
      report->cp = params->cp;
      report->generation = generation;

      if (params->pendingPaste != NULL) {
         g_source_destroy(params->pendingPaste);
         g_source_unref(params->pendingPaste);
      }
      params->pendingPaste = g_idle_source_new();
      g_source_set_callback(params->pendingPaste,
                            CopyPasteUIWayland::PasteObserved, report,
                            g_free);
      g_source_attach(params->pendingPaste, params->mainContext);
   }
   pthread_mutex_unlock(&params->fileBlockMutex);
   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::PasteObserved --
 *
 *    The main loop part of FileBlockMonitorThread(). A reader touched a
 *    blocked path, so ask the host for the files if the block the thread was
 *    watching is still in place.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
CopyPasteUIWayland::PasteObserved(gpointer data)  // IN: WaylandPasteReport
{
   WaylandPasteReport *report = (WaylandPasteReport *) data;
   CopyPasteUIWayland *cp = report->cp;

   pthread_mutex_lock(&cp->mThreadParams.fileBlockMutex);
   if (cp->mThreadParams.pendingPaste == g_main_current_source()) {
      g_source_unref(cp->mThreadParams.pendingPaste);
      cp->mThreadParams.pendingPaste = NULL;
   }
   pthread_mutex_unlock(&cp->mThreadParams.fileBlockMutex);

   if (report->generation == cp->mBlockGeneration && cp->mBlockAdded) {
      g_debug("%s: paste observed, requesting files\n", __FUNCTION__);
      cp->RequestFiles();
   } else {
      g_debug("%s: block already removed or replaced, nothing to request\n",
              __FUNCTION__);
   }
   return G_SOURCE_REMOVE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::TerminateThread --
 *
 *    Signal the monitor thread to exit and join it.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::TerminateThread(void)
{
   if (!mThreadStarted) {
      return;
   }
   pthread_mutex_lock(&mThreadParams.fileBlockMutex);
   mThreadParams.fileBlockCondExit = true;
   pthread_cond_signal(&mThreadParams.fileBlockCond);
   pthread_mutex_unlock(&mThreadParams.fileBlockMutex);
   pthread_join(mThread, NULL);
   mThreadStarted = false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandPathToUri --
 *
 *    Turn an absolute path into a file:// URI for a uri-list.
 *
 *    Escape every byte a receiver would read as syntax. Otherwise CR or LF
 *    ends the entry, # or ? cuts the path short, and a literal "%2E%2E"
 *    decodes to "..". The set is the one CopyPasteUIX11::GetNextPath()
 *    escapes, plus the other control characters and space, which a URI may
 *    not contain unescaped.
 *
 *-----------------------------------------------------------------------------
 */

static std::string
CopyPasteWaylandPathToUri(const std::string &path)  // IN
{
   static const char Dec2Hex[] = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
   };
   std::string uri = DND_URI_LIST_PRE;
   size_t i;

   for (i = 0; i < path.size(); i++) {
      unsigned char ubyte = (unsigned char) path[i];

      if (ubyte == '#' ||   /* Fragment identifier delimiter */
          ubyte == '?' ||   /* Query string delimiter */
          ubyte == '*' ||   /* Special significance in some schemes */
          ubyte == '!' ||   /* Special significance in some schemes */
          ubyte == '%' ||   /* Escape character */
          ubyte == ' ' ||   /* Not allowed in a URI */
          ubyte < 0x20 ||   /* Controls, including CR and LF */
          ubyte == 0x7f ||
          ubyte >= 0x80) {  /* UTF-8 encoding bytes */
         uri += '%';
         uri += Dec2Hex[ubyte >> 4];
         uri += Dec2Hex[ubyte & 0xF];
      } else {
         uri += (char) ubyte;
      }
   }
   return uri;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteWaylandOfferFileList --
 *
 *    Publish file URIs as a local clipboard selection, on the X selection
 *    through xClipboard when it is not NULL and the protocol is absent.
 *    Return in published the rendering that a later read of the selection
 *    yields. The caller caches it to recognise its own selection.
 *
 *    A text/uri-list on its own is not enough. Thunar leaves Paste greyed out
 *    for a selection that offers only that, however the URIs are spelled,
 *    because the selection does not say whether it is a copy or a cut. GNOME
 *    puts a "copy" or "cut" line ahead of the URIs. KDE puts it in a target
 *    of its own. Offer all three renderings, and each application takes the
 *    one it understands.
 *
 *    The URIs carry the file:// authority. A file manager may reject the
 *    spelling without it, and file managers put "file:///path" on their own
 *    clipboards, not "file:/path".
 *
 *    Neither GDK nor the data control code opens the paths while serving
 *    the list. They may be under a vmblock block that the main loop lifts.
 *
 *-----------------------------------------------------------------------------
 */

static bool
CopyPasteWaylandOfferFileList(const std::vector<std::string> &uris,  // IN
                              gpointer xClipboard,                   // IN/OPT
                              std::string &published)                // OUT
{
   std::string plainList;
   std::string gnomeList = "copy\n";

   published.clear();
   for (size_t i = 0; i < uris.size(); i++) {
      published += uris[i];
      published += DND_URI_LIST_POST;
      if (i > 0) {
         plainList += "\n";
      }
      plainList += uris[i];
   }
   gnomeList += plainList;

   WaylandClipItem items[3];

   items[0].mimeType = FCP_TARGET_NAME_URI_LIST;
   items[0].data = published.c_str();
   items[0].size = published.size();
   items[1].mimeType = FCP_TARGET_NAME_GNOME_COPIED_FILES;
   items[1].data = gnomeList.c_str();
   items[1].size = gnomeList.size();
   items[2].mimeType = FCP_TARGET_NAME_KDE_CUT_SELECTION;
   items[2].data = FCP_KDE_CUT_SELECTION_COPY;
   items[2].size = sizeof FCP_KDE_CUT_SELECTION_COPY - 1;

   if (WaylandClipboard_IsAvailable()) {
      return WaylandClipboard_Offer(items, G_N_ELEMENTS(items));
   }
   return xClipboard != NULL &&
          CopyPasteXWriterOffer(xClipboard, items, G_N_ELEMENTS(items));
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::RemoveHGBlock --
 *
 *    Lift the block on the staging directory, if one is in place. A block
 *    left behind by a paste that was never published stalls anything that
 *    looks at the directory, and makes the next AddBlock on it fail.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::RemoveHGBlock(void)
{
   if (!mBlockAdded) {
      return;
   }
   mBlockAdded = false;
   if (mBlockCtrl != NULL && DnD_BlockIsReady(mBlockCtrl)) {
      g_debug("%s: removing block for %s\n", __FUNCTION__,
              mHGStagingDir.c_str());
      mBlockCtrl->RemoveBlock(mBlockCtrl->fd, mHGStagingDir.c_str());
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::SetRemoteFileList --
 *
 *    The host sent a file list. Publish a text/uri-list pointing into the
 *    vmblock file system, so that the paste completes at once and the
 *    transfer waits until an application reads the files.
 *
 *    Returns true when a URI list was installed on the local clipboard.
 *
 *-----------------------------------------------------------------------------
 */

bool
CopyPasteUIWayland::SetRemoteFileList(const CPClipboard *clip)  // IN
{
   void *buf = NULL;
   size_t sz = 0;

   if (!CPClipboard_GetItem(clip, CPFORMAT_FILELIST, &buf, &sz) ||
       buf == NULL || sz == 0) {
      return false;
   }

   if (mBlockCtrl == NULL || !DnD_BlockIsReady(mBlockCtrl)) {
      /*
       * Without vmblock nothing can make a reader wait until the data
       * arrives. Copying every host file selection into the guest up front
       * would cost more than not supporting the paste, so decline.
       */
      g_warning("%s: vmblock is not available, skipping file paste. "
                "Check that run-vmblock\\x2dfuse.mount is active and that "
                "vmtoolsd -n vmusr was started with --blockFd.\n",
                __FUNCTION__);
      return false;
   }

   DnDFileList flist;
   if (!flist.FromCPClipboard(buf, sz)) {
      g_warning("%s: could not decode the host file list\n", __FUNCTION__);
      return false;
   }

   /* A run of NUL-delimited relative paths. */
   std::string relPaths = flist.GetRelPathsStr();
   if (relPaths.empty()) {
      g_warning("%s: host file list carries no paths\n", __FUNCTION__);
      return false;
   }

   char *stagingDir = DnD_CreateStagingDirectory();
   if (stagingDir == NULL) {
      g_warning("%s: could not create a staging directory\n", __FUNCTION__);
      return false;
   }
   mHGStagingDir = stagingDir;
   free(stagingDir);

   mBlockAdded = mBlockCtrl->AddBlock(mBlockCtrl->fd, mHGStagingDir.c_str());
   if (!mBlockAdded) {
      /*
       * A block from an abandoned transfer may still be on this directory;
       * the staging directory is reused whenever it is still empty. Drop it
       * and retry once before giving up.
       */
      g_debug("%s: AddBlock failed for %s, retrying after RemoveBlock\n",
              __FUNCTION__, mHGStagingDir.c_str());
      mBlockCtrl->RemoveBlock(mBlockCtrl->fd, mHGStagingDir.c_str());
      mBlockAdded = mBlockCtrl->AddBlock(mBlockCtrl->fd,
                                         mHGStagingDir.c_str());
   }

   if (!mBlockAdded) {
      g_warning("%s: failed to add a block for %s\n", __FUNCTION__,
                mHGStagingDir.c_str());
      return false;
   }
   g_debug("%s: added block for %s\n", __FUNCTION__, mHGStagingDir.c_str());

   /* The last path component names the staging directory in the block root. */
   std::string stagingName = mHGStagingDir;
   while (!stagingName.empty() && stagingName[stagingName.size() - 1] == '/') {
      stagingName.erase(stagingName.size() - 1);
   }
   size_t slash = stagingName.rfind('/');
   if (slash != std::string::npos) {
      stagingName = stagingName.substr(slash + 1);
   }

   /* The URIs point under the vmblock root. */
   std::vector<std::string> uris;
   size_t off = 0;
   while (off < relPaths.size()) {
      std::string name(relPaths.c_str() + off);

      if (name.empty()) {
         break;
      }
      off += name.size() + 1;

      /*
       * The host chooses these names. A ".." component or an absolute path
       * would point the URI outside the staging directory, where the block
       * does not apply.
       */
      if (!LocalIsSafeStagingName(name.c_str(), name.size() + 1)) {
         g_warning("%s: rejecting an unsafe file list entry\n",
                   __FUNCTION__);
         RemoveHGBlock();
         return false;
      }

      uris.push_back(CopyPasteWaylandPathToUri(
         std::string(mBlockCtrl->blockRoot) + "/" + stagingName + "/" + name));
   }

   if (uris.empty()) {
      g_warning("%s: built an empty URI list\n", __FUNCTION__);
      RemoveHGBlock();
      return false;
   }

   /* Arm the monitor before publishing, so that it sees the first paste. */
   mBlockGeneration++;
   pthread_mutex_lock(&mThreadParams.fileBlockMutex);
   mThreadParams.fileBlockCondExit = false;
   mThreadParams.fileBlockName = std::string(VMBLOCK_FUSE_NOTIFY_ROOT) + "/" +
                                 stagingName;
   mThreadParams.fileBlockGeneration = mBlockGeneration;
   pthread_cond_signal(&mThreadParams.fileBlockCond);
   pthread_mutex_unlock(&mThreadParams.fileBlockMutex);

   g_debug("%s: publishing %d file URI(s)\n", __FUNCTION__, (int) uris.size());

   std::string uriList;
   if (!CopyPasteWaylandOfferFileList(uris, XWriterClipboard(), uriList)) {
      g_warning("%s: could not install the URI list\n", __FUNCTION__);
      pthread_mutex_lock(&mThreadParams.fileBlockMutex);
      mThreadParams.fileBlockName.clear();
      pthread_mutex_unlock(&mThreadParams.fileBlockMutex);
      RemoveHGBlock();
      return false;
   }

   /*
    * Remember the list just published. With a stale cache the next host
    * request would take this uri-list for a new guest copy and size every
    * path in it. Those paths are under a block that only the main loop lifts,
    * and the sizing would stall the main loop.
    */
   CopyPasteWaylandParseUriList(uriList, mLastClipboardFiles);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::SetRemoteFileContents --
 *
 *    Handle a host clipboard that carries file contents instead of paths.
 *    LocalPrepareFileContents() writes the bytes into a staging directory,
 *    records it as the one the clipboard offers, and returns the paths it
 *    created, which are published as a uri-list.
 *
 *    No vmblock is involved. Unlike a file list, the data has already
 *    arrived, so no reader has to wait for it.
 *
 * Results:
 *    true if a selection was installed.
 *
 * Side effects:
 *    Writes files into the staging directory and takes the local selection.
 *
 *-----------------------------------------------------------------------------
 */

bool
CopyPasteUIWayland::SetRemoteFileContents(const CPClipboard *clip)  // IN
{
   std::vector<utf::string> paths;
   std::vector<std::string> uris;
   std::string uriList;

   if (!mCP->CheckCapability(DND_CP_CAP_FILE_CONTENT_CP)) {
      g_debug("%s: file contents copy/paste not in caps\n", __FUNCTION__);
      return false;
   }
   if (!LocalPrepareFileContents(clip, paths) || paths.empty()) {
      g_debug("%s: could not write the file contents out\n", __FUNCTION__);
      return false;
   }

   for (size_t i = 0; i < paths.size(); i++) {
      uris.push_back(CopyPasteWaylandPathToUri(paths[i].c_str()));
   }

   g_debug("%s: staged %d file(s) from contents\n", __FUNCTION__,
           (int) paths.size());

   if (!CopyPasteWaylandOfferFileList(uris, XWriterClipboard(), uriList)) {
      g_warning("%s: could not install the staged file list\n", __FUNCTION__);
      return false;
   }
   CopyPasteWaylandParseUriList(uriList, mLastClipboardFiles);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::GetLocalFilesDone --
 *
 *    The host finished or abandoned the requested transfer. Lift the block so
 *    that the stalled reader continues.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::GetLocalFilesDone(bool success)  // IN
{
   g_debug("%s: success %d\n", __FUNCTION__, success);

   RemoveHGBlock();

   if (success) {
      /*
       * Ask for the staging dir to be deleted on next reboot, as the X11
       * backends do. On POSIX, File_UnlinkDelayed() unlinks at once and
       * fails on a directory, so this removes nothing.
       */
      DnD_DeleteStagingFiles(mHGStagingDir.c_str(), TRUE);
   } else {
      mHGStagingDir.clear();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::SendLocalClip --
 *
 *    Send the guest selection read into mClipboard to the host.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Sends an RPC to the host.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::SendLocalClip(void)
{
   mCP->DestUISendClip(&mClipboard);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::SendClipNotChanged --
 *
 *    Send a not-changed clip to host.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteUIWayland::SendClipNotChanged(void)
{
   CPClipboard clip;

   CPClipboard_Init(&clip);
   CPClipboard_SetChanged(&clip, FALSE);
   mCP->DestUISendClip(&clip);
   CPClipboard_Destroy(&clip);
}
