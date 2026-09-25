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
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <gtk/gtk.h>

#include "vmware.h"
#include "copyPasteUIWayland.h"
#include "waylandClipboard.h"
#include "guestDnDCPMgr.hh"
#include "dndMsg.h"

extern "C" {
#include "file.h"
#include "str.h"
#include "util.h"
#include "dndCPMsgV4.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
}


/*
 * One request from the host can ask for several types in turn. Each read is
 * bounded, but a sequence of them is not, and this runs on the main loop that
 * also carries the RPC channel. Give the whole request one budget and let each
 * read have what is left of it.
 */
#define CP_REQUEST_BUDGET_MS 2000


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
   mRetriedGeneration(G_MAXUINT64)
{
   GuestDnDCPMgr *p = GuestDnDCPMgr::GetInstance();
   ASSERT(p);
   mCP = p->GetCopyPasteMgr();
   ASSERT(mCP);
   CPClipboard_Init(&mClipboard);
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
   WaylandClipboard_Exit();
   CPClipboard_Destroy(&mClipboard);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteUIWayland::Init --
 *
 *    Choose how to own and read the local selection and connect to the
 *    common-layer copy/paste signals.
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
      if (!current.empty() && current == mLastHostClip &&
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
 *    Read the local selection and send it to the host. An image comes first,
 *    then rich text with its plain text rendering, then plain text.
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
    * host its own data. On the
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
    * An image first, as in copyPasteUIX11.cpp. An image selection often also
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
