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
 * waylandClipboard.c. A data control client needs no keyboard focus, unlike
 * the core wl_data_device protocol that GtkClipboard uses on Wayland. That is
 * what allows a background daemon to bridge the clipboard.
 */

#define G_LOG_DOMAIN "dndcp"

#include <glib.h>
#include <string.h>

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
    * ext-data-control-v1. This has to happen before the baseline read below.
    */
   if (WaylandClipboard_Init()) {
      g_debug("%s: using the native Wayland selection\n", __FUNCTION__);
   } else {
      g_warning("%s: the compositor does not offer ext-data-control-v1, "
                "copy and paste is disabled\n", __FUNCTION__);
   }

   /*
    * Establish a baseline before the first host request. Otherwise the guest's
    * pre-existing clipboard looks new and overwrites a newer host clipboard as
    * soon as the VM receives focus.
    */
   GetSelectionGeneration(&mReadGeneration);
   gsize len = 0;
   gchar *text = CopyPasteWaylandGetText(&len,
      g_get_monotonic_time() + CP_REQUEST_BUDGET_MS * 1000);
   mLastClipboardText.assign(text != NULL ? text : "", len);
   g_free(text);

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
          WaylandClipboard_IsOwner()) {
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

   bool ok = false;
   if (WaylandClipboard_IsAvailable()) {
      ok = WaylandClipboard_Offer(&items[0], items.size());
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
 * CopyPasteUIWayland::GetSelectionGeneration --
 *
 *    Get a number that changes whenever the local selection changes. The
 *    native backend counts selection events.
 *
 * Results:
 *    false when nothing counts the changes, which is where the compositor has
 *    no ext-data-control-v1. Every request then reads the selection.
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
    * host its own data.
    */
   if (WaylandClipboard_IsOwner()) {
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
