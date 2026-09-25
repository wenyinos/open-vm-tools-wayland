/*********************************************************
 * Copyright (c) 2025 Broadcom. All Rights Reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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
 * @file dndUIX11GTK4.cpp --
 *
 * This class implements stubs for the methods that allow DnD between
 * host and guest on GTK4 lib.
 */

#define G_LOG_DOMAIN "dndcp"

#include "xutils/xutilsGTK4.hh"
#include "copyPasteDnDUtil.h"
#include "dndUIX11GTK4.h"
#include "guestDnDCPMgr.hh"
#include "tracer.hh"
#include "fakeMouseWayland/fakeMouseWayland.h"

extern "C" {
#include "vmware/guestrpc/tclodefs.h"

#include "copyPasteCompat.h"
#include "dndClipboard.h"
#include "hgfsUri.h"
#include "rpcout.h"
}

#include "dnd.h"
#include "dndMsg.h"
#include "hostinfo.h"
#include "file.h"
#include "vmblock.h"

/* IsXExtensionPointer may be not defined with old Xorg. */
#ifndef IsXExtensionPointer
#define IsXExtensionPointer 4
#endif

#include "copyPasteDnDWrapper.h"


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::DnDUIX11 --
 *
 *      Constructor.
 *
 *-----------------------------------------------------------------------------
 */

DnDUIX11::DnDUIX11(ToolsAppCtx *ctx)
    : mCtx(ctx),
      mDnD(NULL),
      mDetWnd(NULL),
      mClipboard(),
      mBlockCtrl(NULL),
      mHGGetFileStatus(DND_FILE_TRANSFER_NOT_STARTED),
      mBlockAdded(false),
      mGHDnDInProgress(false),
      mGHDnDDataReceived(false),
      mGHReadCancel(NULL),
      mGHReads(NULL),
      mInHGDrag(false),
      mHGButtonDown(false),
      mEffect(DROP_NONE),
      mMousePosX(0),
      mMousePosY(0),
      mNumPendingRequest(0),
      mDestDropTime(0),
      mTotalFileSize(0),
      mOriginX(0),
      mOriginY(0),
      mGdkSurface(NULL)
{
   TRACE_CALL();


   /*
    * XXX Hard coded use of default screen means this doesn't work in dual-
    * headed setups (e.g. DISPLAY=:0.1).  However, the number of people running
    * such setups in VMs is expected to be, like, hella small, so I'mma cut
    * corners for now.
    */
   Glib::RefPtr<Gdk::Display> display = Gdk::Display::get_default();
   GdkSurface *gdkSurface = gdk_x11_display_get_default_group(display->gobj());
   mGdkSurface = Glib::wrap(gdkSurface);
   UpdateWorkArea();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::~DnDUIX11 --
 *
 *      Destructor.
 *
 *-----------------------------------------------------------------------------
 */

DnDUIX11::~DnDUIX11()
{
   TRACE_CALL();

   if (mDetWnd) {
      delete mDetWnd;
      mDetWnd = NULL;
   }
   CPClipboard_Destroy(&mClipboard);
   /* Any files from last unfinished file transfer should be deleted. */
   if (DND_FILE_TRANSFER_IN_PROGRESS == mHGGetFileStatus
       && !mHGStagingDir.empty()) {
      uint64 totalSize = File_GetSizeEx(mHGStagingDir.c_str());
      if (mTotalFileSize != totalSize) {
         g_debug("%s: deleting %s, expecting %" FMT64 "u, finished %" FMT64 "u\n",
                 __FUNCTION__, mHGStagingDir.c_str(),
                 mTotalFileSize, totalSize);
         DnD_DeleteStagingFiles(mHGStagingDir.c_str(), FALSE);
      } else {
         g_debug("%s: file size match %s\n",
                 __FUNCTION__, mHGStagingDir.c_str());
      }
   }
   ResetUI();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::Init --
 *
 *      Initialize DnDUIX11 object.
 *
 * Results:
 *      Returns true on success and false on failure.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::Init()
{
   TRACE_CALL();
   bool ret = true;

   CPClipboard_Init(&mClipboard);

   GuestDnDCPMgr *p = GuestDnDCPMgr::GetInstance();
   ASSERT(p);
   mDnD = p->GetDnDMgr();
   ASSERT(mDnD);

   mDetWnd = new DragDetWnd(mCtx->uinputFD);
   if (!mDetWnd) {
      g_debug("%s: unable to allocate DragDetWnd object\n", __FUNCTION__);
      goto fail;
   }

   InitGtk();

#define CONNECT_SIGNAL(_obj, _sig, _cb) \
   _obj->_sig.connect(sigc::mem_fun(this, &DnDUIX11::_cb))
   /* Set common layer callbacks. */
   CONNECT_SIGNAL(mDnD, srcDragBeginChanged,   OnSrcDragBegin);
   CONNECT_SIGNAL(mDnD, srcCancelChanged,      OnSrcCancel);
   CONNECT_SIGNAL(mDnD, getFilesDoneChanged,   OnGetFilesDone);
   CONNECT_SIGNAL(mDnD, moveMouseChanged,      OnMoveMouse);
   CONNECT_SIGNAL(mDnD, srcDropChanged,        OnSrcDrop);
   CONNECT_SIGNAL(mDnD, destCancelChanged,     OnDestCancel);
   CONNECT_SIGNAL(mDnD, destMoveDetWndToMousePosChanged, OnDestMoveDetWndToMousePos);
   CONNECT_SIGNAL(mDnD, privDropChanged,       OnPrivateDrop);
   CONNECT_SIGNAL(mDnD, updateDetWndChanged,   OnUpdateDetWnd);
#undef CONNECT_SIGNAL

   mDetWnd->UpdateDetWnd(false, 0, 0);
   goto out;
fail:
   ret = false;
   if (mDnD) {
      delete mDnD;
      mDnD = NULL;
   }
   if (mDetWnd) {
      delete mDetWnd;
      mDetWnd = NULL;
   }
out:
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::InitGtk --
 *
 *      Register supported DND target types and signal handlers with GTK+.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::InitGtk()
{
   TRACE_CALL();

   mDetWnd->register_source_drag_begin_handler(OnGtkSrcDragBegin, (void *)this);
   mDetWnd->register_source_drag_end_handler(OnGtkSrcDragEnd, (void *)this);
   mDetWnd->register_source_drag_cancel_handler(OnGtkSrcDragCancel, (void *)this);
   mDetWnd->register_target_drag_accept_handler(OnGtkDragAccept, (void *)this);
   mDetWnd->register_target_drag_enter_handler(OnGtkDragEnter, (void *)this);
   mDetWnd->register_target_drag_drop_handler(OnGtkDragDrop, (void *)this);
   mDetWnd->register_target_drag_leave_handler(OnGtkDragLeave, (void *)this);
   mDetWnd->register_target_drag_motion_handler(OnGtkDragMotion, (void *)this);
   mDetWnd->register_target_value_handler(OnGtkDragDataReceived, (void *)this);
   mDetWnd->EnableDND();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::ResetUI --
 *
 *      Reset UI state variables.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May remove a vmblock blocking entry.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::ResetUI()
{
   TRACE_CALL();
   /*
    * The press faked to start a host to guest drag is released by the host
    * when it drops. If the drag never started there is nothing to drop, and
    * a button left down through uinput holds the pointer in a drag for the
    * whole session, which looks to the user like the desktop has frozen.
    */
   if (mHGButtonDown) {
      /* The destructor deletes the detection window before it resets. */
      if (mDetWnd != NULL) {
         g_debug("%s: releasing the faked button\n", __FUNCTION__);
         /*
          * Release where the host has the pointer. The X server's idea of
          * the pointer is stale while it is over a Wayland surface, and the
          * host can still be dragging, so leave the window where it is.
          */
         mDetWnd->SimulateXEvents(false, true, false, false, true,
                                  mMousePosX, mMousePosY);
      }
      mHGButtonDown = false;
   }
   mGHDnDDataReceived = false;
   mHGGetFileStatus = DND_FILE_TRANSFER_NOT_STARTED;
   mGHDnDInProgress = false;
   mEffect = DROP_NONE;
   mInHGDrag = false;
   CancelGHRead();
   RemoveBlock();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::AddBlock --
 *
 *      Insert a vmblock blocking entry.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Caller must pair with RemoveBlock() upon dnd completion/cancellation.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::AddBlock()
{
   TRACE_CALL();

   if (mBlockAdded) {
      g_debug("%s: block already added\n", __FUNCTION__);
      return;
   }

   g_debug("%s: DnDBlockIsReady %d fd %d\n", __FUNCTION__,
           DnD_BlockIsReady(mBlockCtrl), mBlockCtrl->fd);

   if (   DnD_BlockIsReady(mBlockCtrl)
       && mBlockCtrl->AddBlock(mBlockCtrl->fd, mHGStagingDir.c_str())) {
      mBlockAdded = true;
      g_debug("%s: add block for %s.\n", __FUNCTION__, mHGStagingDir.c_str());
   } else {
      mBlockAdded = false;
      g_debug("%s: unable to add block dir %s.\n", __FUNCTION__, mHGStagingDir.c_str());
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::RemoveBlock --
 *
 *      Remove a vmblock blocking entry.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::RemoveBlock()
{
   TRACE_CALL();

   if (mBlockAdded && (DND_FILE_TRANSFER_IN_PROGRESS != mHGGetFileStatus)) {
      g_debug("%s: removing block for %s\n", __FUNCTION__, mHGStagingDir.c_str());
      /* We need to make sure block subsystem has not been shut off. */
      if (DnD_BlockIsReady(mBlockCtrl)) {
         mBlockCtrl->RemoveBlock(mBlockCtrl->fd, mHGStagingDir.c_str());
      }
      mBlockAdded = false;
   } else {
      g_debug("%s: not removing block mBlockAdded %d mHGGetFileStatus %d\n",
              __FUNCTION__, mBlockAdded, mHGGetFileStatus);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalSetContentProvider --
 *
 *      Set content provider based on different desktop type.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      mProvider is chanaged.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalSetContentProvider(const char* desktop)   // IN
{
   std::vector<Glib::RefPtr<Gdk::ContentProvider>> providerVec;
   Glib::RefPtr<Glib::Bytes> fileList;
   void *buf;
   size_t sz;

   g_debug("%s: enter.\n", __FUNCTION__);

   if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_FILELIST) ||
       CPClipboard_ItemExists(&mClipboard, CPFORMAT_FILECONTENTS)) {
      /* The caller has already framed the list for this desktop. */
      fileList = Glib::Bytes::create(mHGDndUriList.c_str(), mHGDndUriList.bytes());
      providerVec.push_back(Gdk::ContentProvider::create(DRAG_TARGET_NAME_URI_LIST, fileList));
   } else if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_RTF)) {
      if (CPClipboard_GetItem(&mClipboard, CPFORMAT_RTF, &buf, &sz)) {
         g_debug("%s: RTF data, size %" FMTSZ "u.\n", __FUNCTION__, sz);
         Glib::RefPtr<Glib::Bytes> rtfData = Glib::Bytes::create(buf, sz);
         providerVec.push_back(Gdk::ContentProvider::create(TARGET_NAME_APPLICATION_RTF, rtfData));
         providerVec.push_back(Gdk::ContentProvider::create(TARGET_NAME_TEXT_RICHTEXT, rtfData));
         providerVec.push_back(Gdk::ContentProvider::create(TARGET_NAME_TEXT_RTF, rtfData));
      }
   } else if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_TEXT)) {
      if (CPClipboard_GetItem(&mClipboard, CPFORMAT_TEXT, &buf, &sz)) {
         g_debug("%s: Text data, size %" FMTSZ "u.\n", __FUNCTION__, sz);
         Glib::RefPtr<Glib::Bytes> textData = Glib::Bytes::create(buf, sz);
         providerVec.push_back(Gdk::ContentProvider::create(TARGET_NAME_TEXT_PLAIN, textData));
         providerVec.push_back(Gdk::ContentProvider::create(TARGET_NAME_TEXT_PLAIN_UTF8, textData));
      }
   }
   mProvider = Gdk::ContentProvider::create(providerVec);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::BuildFileURIList --
 *
 *      Build URI file list with proper pre/post str.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::BuildFileURIList(utf::string& uriList,         // IN/OUT
                           utf::string pre,              // IN
                           utf::string post,             // IN
                           utf::string stagingDirName)   // IN
{
   size_t pos  = 0;
   utf::string str;

   while ((str = GetNextPath(mHGFCPData, pos).c_str()).bytes() != 0) {
      g_debug("%s: Path: %s", __FUNCTION__, str.c_str());
      uriList += pre;
      if (mBlockAdded) {
         uriList += mBlockCtrl->blockRoot;
         uriList += DIRSEPS + stagingDirName + DIRSEPS + str + post;
      } else {
         uriList += DIRSEPS + mHGStagingDir + DIRSEPS + str + post;
      }
   }
   g_debug("%s: providing uriList [%s]\n", __FUNCTION__, uriList.c_str());
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::BuildFileContentURIList --
 *
 *      Used to populate the file content URI lists for GNOME, Nautilus, and KDE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::BuildFileContentURIList(utf::string& uriList,         // IN/OUT
                                        utf::string pre,              // IN
                                        utf::string post)             // IN
{
   for (const auto& path : mHGFileContentsList) {
      uriList += pre + path + post;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalGetFileRequest --
 *
 *      Build URI file list with proper pre/post str.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      mHGDndUriList may be changed.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalGetFileRequest(utf::string stagingDirName,
                              const char *desktop)
{
   mHGDndUriList = "";
   if (desktop && strstr(desktop, "GNOME")) {
      /* Setting Gnome URIs for each path in the guest's file list. */
      BuildFileURIList(mHGDndUriList, FCP_GNOME_LIST_PRE, FCP_GNOME_LIST_POST, stagingDirName);
      /* Nautilus on Gnome does not expect FCP_GNOME_LIST_POST after the last uri. See bug 143147. */
      size_t postLen = strlen(FCP_GNOME_LIST_POST);
      if (mHGDndUriList.length() >= postLen &&
          mHGDndUriList.compare(mHGDndUriList.length() - postLen,
                                postLen,
                                FCP_GNOME_LIST_POST) == 0) {
         /*
          * The list is empty when there are no files, and an unguarded
          * subtraction would wrap and make erase() throw out of the main loop.
          */
         mHGDndUriList.erase(mHGDndUriList.length() - postLen);
      }
      g_debug("%s: providing file list [%s]\n", __FUNCTION__, mHGDndUriList.c_str());
   } else if (desktop && strstr(desktop, "KDE")) {
      /* Setting KDE URIs for each path in the guest's file list. */
      BuildFileURIList(mHGDndUriList, DND_URI_LIST_PRE_KDE, DND_URI_LIST_POST, stagingDirName);
      g_debug("%s: providing file list [%s]\n", __FUNCTION__, mHGDndUriList.c_str());
   } else {
      /*
       * Both cases above depart from text/uri-list. GNOME separates entries
       * with a bare newline, and KDE drops the authority. Any other desktop
       * gets the form the type specifies.
       */
      BuildFileURIList(mHGDndUriList, DND_URI_LIST_PRE, DND_URI_LIST_POST, stagingDirName);
      g_debug("%s: %s, providing file list [%s]\n", __FUNCTION__,
              desktop != NULL ? desktop : "no XDG_CURRENT_DESKTOP",
              mHGDndUriList.c_str());
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DndUIX11::LocalGetFileContentsRequest --
 *
 *      Build the files content uri list when host dnd with file content.
 *      H->G only.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      mHGDndUriList may be changed.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalGetFileContentsRequest(const char *desktop)
{
   mHGDndUriList = "";
   if (desktop && strstr(desktop, "GNOME")) {
      /* Setting Gnome URIs for each path in the guest's file list. */
      BuildFileContentURIList(mHGDndUriList, FCP_GNOME_LIST_PRE, FCP_GNOME_LIST_POST);
      /* Nautilus on Gnome does not expect FCP_GNOME_LIST_POST after the last uri. See bug 143147. */
      size_t postLen = strlen(FCP_GNOME_LIST_POST);
      if (mHGDndUriList.length() >= postLen &&
          mHGDndUriList.compare(mHGDndUriList.length() - postLen,
                                postLen,
                                FCP_GNOME_LIST_POST) == 0) {
         /*
          * The list is empty when there are no files, and an unguarded
          * subtraction would wrap and make erase() throw out of the main loop.
          */
         mHGDndUriList.erase(mHGDndUriList.length() - postLen);
      }
      g_debug("%s: providing file list [%s]\n", __FUNCTION__, mHGDndUriList.c_str());
   } else if (desktop && strstr(desktop, "KDE")) {
      /* Setting KDE URIs for each path in the guest's file list. */
      BuildFileContentURIList(mHGDndUriList, DND_URI_LIST_PRE_KDE, DND_URI_LIST_POST);
      g_debug("%s: providing file list [%s]\n", __FUNCTION__, mHGDndUriList.c_str());
   } else {
      BuildFileContentURIList(mHGDndUriList, DND_URI_LIST_PRE, DND_URI_LIST_POST);
      g_debug("%s: %s, providing file list [%s]\n", __FUNCTION__,
              desktop != NULL ? desktop : "no XDG_CURRENT_DESKTOP",
              mHGDndUriList.c_str());
   }
}


/* Source functions for HG DnD. */


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalFileListIsSafe --
 *
 *      Whether every relative path in a host file list names something below
 *      the staging directory and can be published as one uri-list entry.
 *
 * Results:
 *      True if all of them can, false at the first that cannot.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::LocalFileListIsSafe(const DnDFileList &fList)   // IN
{
   std::vector<std::string> relPaths = fList.GetRelPaths();

   for (const std::string &rel : relPaths) {
      if (!LocalIsSafeStagingName(rel.c_str(), rel.size() + 1)) {
         g_debug("%s: refusing relative path [%s]\n", __FUNCTION__,
                 rel.c_str());
         return false;
      }
   }

   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalRefuseDrag --
 *
 *      Abandon a host to guest drag that has just begun.
 *
 *      Returning from OnSrcDragBegin() without SourceDragStartDone() leaves
 *      the state machine in DRAGBEGIN_PENDING, and the host's next drag then
 *      only resets it. Resetting here runs OnSrcCancel() over the state the
 *      drag begin set up, and leaves the state machine ready.
 *
 * Side effects:
 *      Clears the clipboard copy and the file list, and resets mDnD.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalRefuseDrag(void)
{
   mHGFCPData.clear();
   mHGDndUriList = "";
   CPClipboard_Clear(&mClipboard);
   mDnD->GetRpc()->SrcCancel(mDnD->GetSessionId());
   /*
    * This deletes the GuestDnDSrc whose signal called this function. The
    * GuestDnDSrc touches none of its members after the signal returns, so
    * the delete is safe.
    */
   mDnD->ResetDnD();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnSrcDragBegin --
 *
 *      Called when host successfully detected a pending HG drag.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls mDetWnd->drag_begin().
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnSrcDragBegin(const CPClipboard *clip,       // IN
                         const std::string stagingDir)  // IN
{
   int mouseX = mOriginX + DRAG_DET_WINDOW_WIDTH / 2;
   int mouseY = mOriginY + DRAG_DET_WINDOW_WIDTH / 2;
   const char *desktop = getenv("XDG_CURRENT_DESKTOP");
   bool hasContent = false;
   mHGDragStarted = false;

   /* The host's coordinates are for the screen as it is now. */
   mDetWnd->RefreshScreenSize();

   /* Move detect window to the mouse in position */
   OnUpdateDetWnd(true, mOriginX, mOriginY);

   TRACE_CALL();

   CPClipboard_Clear(&mClipboard);
   CPClipboard_Copy(&mClipboard, clip);

   /*
    * Construct the target and action list, as well as a fake motion notify
    * event that's consistent with one that would typically start a drag.
    */
   utf::string stagingDirName;
   DnDFileList fList;
   void *buf;
   size_t sz;

   mHGFCPData.clear();

   if (CPClipboard_GetItem(&mClipboard, CPFORMAT_FILELIST, &buf, &sz)) {
      mHGStagingDir = utf::string(stagingDir.c_str());

      /* Provide path within vmblock file system instead of actual path. */
      stagingDirName = GetLastDirName(mHGStagingDir);
      if (stagingDirName.length() == 0) {
         g_debug("%s: Cannot get staging directory name, stagingDir: %s\n",
                 __FUNCTION__, mHGStagingDir.c_str());
         LocalRefuseDrag();
         return;
      }

      if (!fList.FromCPClipboard(buf, sz)) {
         g_debug("%s: Can't get data from clipboard\n", __FUNCTION__);
         LocalRefuseDrag();
         return;
      }

      /*
       * Every name below goes into the uri-list with only percent escaping,
       * and resolves under the vmblock mount. A name that leads out of the
       * staging directory, or ends a uri-list entry early, publishes a path
       * where nothing was staged.
       */
      if (!LocalFileListIsSafe(fList)) {
         g_warning("%s: refusing a file list with an unusable name\n",
                   __FUNCTION__);
         LocalRefuseDrag();
         return;
      }
      mTotalFileSize = fList.GetFileSize();
      // Provide path within vmblock file system instead of actual path.
      mHGFCPData = fList.GetRelPathsStr();

      if (!mBlockAdded) {
         AddBlock();
      } else {
         g_debug("%s: not calling AddBlock\n", __FUNCTION__);
      }
      if (mHGGetFileStatus != DND_FILE_TRANSFER_IN_PROGRESS) {
         mHGGetFileStatus = DND_FILE_TRANSFER_IN_PROGRESS;
      }

      LocalGetFileRequest(stagingDirName, desktop);
      hasContent = true;
   }

   if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_FILECONTENTS)) {
      g_debug("%s: Get file content from host clipboard", __FUNCTION__);
      LocalGetFileContentsRequest(desktop);
      hasContent = true;
   }

   if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_TEXT) ||
       CPClipboard_ItemExists(&mClipboard, CPFORMAT_RTF)) {
      g_debug("%s: Get text/rtfText content from host clipboard", __FUNCTION__);
      hasContent = true;
   }

   /* finally set the guest clipboard */
   if (hasContent) {
      LocalSetContentProvider(desktop);
   }

   SourceDragStartDone();
   /* Initialize host hide feedback to DROP_NONE. */
   mEffect = DROP_NONE;
   SourceUpdateFeedback(mEffect);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnMoveMouse --
 *
 *      simulate mouse move for h->g dnd.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnMoveMouse(int32 x,  // IN: Pointer x-coord
                      int32 y)  // IN: Pointer y-coord
{
   // Position the pointer, and record its position.
   mDetWnd->SimulateXEvents(false, false, false, false, true, x, y);
   mMousePosX = x;
   mMousePosY = y;

   if (mInHGDrag) {
      if (!mHGDragStarted) {
         /*
          * We want to simulate the mouse press and hold to trigger drag_begin
          * event for GtkDragSource, do it here rather than SrcDragBegin as
          * we want to get the mouse position.
          *
          * Before the DnD, we should make sure that the mouse is released
          * otherwise it may be another DnD, not ours. Send a release, then
          * a press here to cover this case.
          */
         mDetWnd->SimulateXEvents(true, true, false, true, true, x, y);
         // simulate drag mouse press
         mDetWnd->SimulateXEvents(false, true, true, false, true, x, y);
         mHGButtonDown = true;
         // Tell Gtk that a drag should be started from this widget.
         mDetWnd->StartDrag(mProvider, x, y);
         mHGDragStarted = true;
      }
      if (mEffect != DROP_MOVE) {
         mEffect = DROP_MOVE;
         g_debug("%s: Updating feedback\n", __FUNCTION__);
         SourceUpdateFeedback(mEffect);
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnSrcCancel --
 *
 *      Handler for when host cancels HG drag.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Detection window and fake mouse events.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnSrcCancel()
{
   TRACE_CALL();
   /*
    * Force the window to show, position the mouse over it, and release.
    * Seems like moving the window to 0, 0 eliminates frequently observed
    * flybacks when we cancel as user moves mouse in and out of destination
    * window in a H->G DnD.
    */
   OnUpdateDetWnd(true, mOriginX, mOriginY);
   mDetWnd->SimulateXEvents(true, true, false, true, true,
                            mOriginX + DRAG_DET_WINDOW_WIDTH / 2,
                            mOriginY + DRAG_DET_WINDOW_WIDTH / 2);
   OnUpdateDetWnd(false, 0, 0);
   mDetWnd->SimulateXEvents(false, false, false, false, true,
                           mMousePosX, mMousePosY);
   /* The real release has been sent; ResetUI() need not send another. */
   mHGButtonDown = false;
   mInHGDrag = false;
   mHGGetFileStatus = DND_FILE_TRANSFER_NOT_STARTED;
   mEffect = DROP_NONE;
   RemoveBlock();
}


/* Source functions for GH DnD. */


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnPrivateDrop --
 *
 *      Handler for private drop event.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Releases mouse button at current position.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnPrivateDrop(int32 x,        // UNUSED
                        int32 y)        // UNUSED
{
   TRACE_CALL();

   if (mGHDnDInProgress) {

      /*
       * Release the mouse button.
       */
      mDetWnd->SimulateXEvents(false, true, false, false, false, 0, 0);
   }
   ResetUI();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnDestCancel --
 *
 *      Handler for GH drag cancellation.
 *
 *      Note: This event fires as part of the complete guest-to-host sequence,
 *      not just error or user cancellation.
 *
 * Results:
 *      Uses detection window and fake mouse events to intercept drop.
 *
 * Side effects:
 *      Reinitializes UI state.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnDestCancel()
{
   TRACE_CALL();

   if (mGHDnDInProgress) {
      Glib::RefPtr<Gdk::Display> display = Gdk::Display::get_default();
      GdkSurface *gdkSurface = gdk_x11_display_get_default_group(display->gobj());
      UpdateWorkArea();

     /*
       * Show the window, move it to the mouse position, and release the
       * mouse button.
       */
      mDetWnd->SimulateXEvents(true, true, false, true, false,
                               mOriginX, mOriginY);
   }
   mDestDropTime = GetTimeInMillis();
   ResetUI();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnSrcDrop --
 *
 *      Callback when host signals drop.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Release the mouse button in the detection window.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnSrcDrop()
{
   TRACE_CALL();
   OnUpdateDetWnd(true, mOriginX, mOriginY);

   /*
    * Move the mouse to the saved coordinates, and release the mouse button.
    */
   mDetWnd->SimulateXEvents(false, true, false, false, true,
                            mMousePosX, mMousePosY);
   OnUpdateDetWnd(false, 0, 0);
   /* The real release has been sent; ResetUI() need not send another. */
   mHGButtonDown = false;
   /* We've successful or failure for HG drag, set the flag to false anyway */
   mInHGDrag = false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGetFilesDone --
 *
 *      Callback when HG file transfer completes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Releases vmblock blocking entry.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnGetFilesDone(bool success)  // IN: true if transfer succeeded
{
   g_debug("%s: %s\n", __FUNCTION__, success ? "success" : "failed");

   /*
    * If H->G drag is not done yet, only remove block. Otherwise destination
    * may miss the data because we are already reset.
    */

   mHGGetFileStatus = DND_FILE_TRANSFER_FINISHED;

   if (!mInHGDrag) {
      ResetUI();
   } else {
      RemoveBlock();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnUpdateDetWnd --
 *
 *      Callback to show/hide drag detection window.
 *
 * Results:
 *      Shows/hides and moves detection window.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnUpdateDetWnd(bool show,     // IN: show (true) or hide (false)
                         int32 x,       // IN: detection window's destination x-coord
                         int32 y)       // IN: detection window's destination y-coord
{
   g_debug("%s: enter 0x%lx show %d x %d y %d\n",
           __FUNCTION__,
           mDetWnd->Get_Current_Drop_ctx(), show, x, y);

   /* If the window is being shown, move it to the right place. */
   if (show) {
      UpdateWorkArea();
      x = MAX(x - DRAG_DET_WINDOW_WIDTH / 2, mOriginX);
      y = MAX(y - DRAG_DET_WINDOW_WIDTH / 2, mOriginY);
      mDetWnd->UpdateDetWnd(show, x, y);
   } else {
      g_debug("%s: hide\n", __FUNCTION__);
      mDetWnd->UpdateDetWnd(show, x, y);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnDestMoveDetWndToMousePos --
 *
 *      Callback to move detection window to current moue position.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Detection window is moved, shown.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnDestMoveDetWndToMousePos()
{
   mDetWnd->RefreshScreenSize();
   mDetWnd->SimulateXEvents(true, false, true, true, false, 0, 0);
}


/*
 ****************************************************************************
 * BEGIN GTK+ Callbacks (dndcp as drag source: host-to-guest)
 */


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkSrcDragBegin --
 *
 *      GTK4 h->g drag begin handler
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Signals to drag source that drag is started.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnGtkSrcDragBegin(const Glib::RefPtr< Gdk::Drag > &drag, void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   g_debug("%s: DND drag begin with ctx %ld\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drag_ctx());
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkSrcDragEnd --
 *
 *      GTK4 h->g drag end handler
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Signals to drag source that drag is started.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnGtkSrcDragEnd(const Glib::RefPtr< Gdk::Drag > &drag,
                          bool delete_data,
                          void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   g_debug("%s: DND drag done with ctx %ld\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drag_ctx());
   if (this_instance->mHGGetFileStatus != DND_FILE_TRANSFER_IN_PROGRESS) {
      g_debug("%s: reset UI\n", __FUNCTION__);
      this_instance->ResetUI();
   }
   this_instance->mInHGDrag = false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkSrcDragCancel --
 *
 *      GTK+ DragSource "drag_leave" signal handler.
 *
 *      Logs event.  Acknowledges, finishes outdated sequence if drag context
 *      is not the same as we're currently interested in (i.e. != mDragCtx).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May cancel dnd associated with this drag context.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::OnGtkSrcDragCancel(const Glib::RefPtr< Gdk::Drag > &drag,
                             Gdk::DragCancelReason reason, void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   g_debug("%s: DND cancel for drag %ld with reason %d\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drag_ctx(), (int)reason);
   this_instance->ResetUI();
   return true;
}


/*
 * END GTK+ Callbacks (dndcp as drag source: host-to-guest)
 ****************************************************************************
 */


/*
 ****************************************************************************
 * BEGIN GTK+ Callbacks (dndcp as drag destination: guest-to-host)
 */


/*
 *-----------------------------------------------------------------------------
 *
 * Reading what is dragged from the guest.
 *
 *    GTK would read the value itself, and for a file list it prefers
 *    application/vnd.portal.filetransfer, which KDE and GTK4 applications
 *    offer next to text/uri-list. gdk_drop_read_value_async() with
 *    GDK_TYPE_FILE_LIST picks the portal type, fetches its token over
 *    XdndSelection, then fails to redeem the token with "Unable to open
 *    /proc/<pid>/root".
 *
 *    The document portal authenticates a caller by opening /proc/<pid>/root,
 *    which fails here. vmusr is exec'd by a setuid wrapper, and that clears
 *    the kernel's dumpable flag. /proc/<pid> then stays owned by root even
 *    after the process drops back to the user.
 *
 *    So a file list is read as text/uri-list. The source offers it, it needs
 *    no portal, and it comes over the same XdndSelection transfer. A drag
 *    with no text/uri-list is read as a string, which GDK converts from
 *    whatever text type the source offers.
 *
 *    The uri-list stream has to be drained asynchronously.
 *    gdk_drop_read_finish() warns against blocking reads on the returned
 *    stream in the GTK thread, and a synchronous drain here deadlocks the
 *    main loop.
 *
 *-----------------------------------------------------------------------------
 */

#define DND_URI_LIST_CHUNK   (16 * 1024)
#define DND_URI_LIST_MAX     (1 << 20)

/*
 * DnDUIX11::CancelGHRead() clears ui when the drag ends or the object is
 * destroyed, so a callback that is already queued finds NULL rather than a
 * destroyed object. The read holds its own reference to the cancellable
 * rather than reading it back from the UI, so that starting a new read cannot
 * swap in a different one.
 */
typedef struct DnDGHRead {
   DnDUIX11 *ui;
   GCancellable *cancel;
   /* The type handed to DnDUIX11::OnGHDropRead(). */
   const char *type;
   /* For a uri-list only. */
   GInputStream *stream;
   GString *acc;
} DnDGHRead;


/*
 *-----------------------------------------------------------------------------
 *
 * DnDGHReadFree --
 *
 *    Free a read, and take it off the UI's list of reads in progress.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static void
DnDGHReadFree(DnDGHRead *rd)  // IN
{
   if (rd == NULL) {
      return;
   }
   if (rd->ui != NULL) {
      rd->ui->ForgetGHRead(rd);
      rd->ui = NULL;
   }
   if (rd->stream != NULL) {
      /*
       * Closing can make a round trip to the drag source, and this stream
       * must not see a blocking call on the GTK thread. Dropping the
       * reference is enough, because GIO closes the stream when it is
       * finalized.
       */
      g_object_unref(rd->stream);
   }
   if (rd->cancel != NULL) {
      g_object_unref(rd->cancel);
   }
   if (rd->acc != NULL) {
      g_string_free(rd->acc, TRUE);
   }
   g_free(rd);
}


static void DnDUriListReadChunk(GObject *source, GAsyncResult *res, gpointer data);


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUriListPump --
 *
 *    Ask for the next chunk of the uri-list.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    DnDUriListReadChunk() runs when the chunk arrives.
 *
 *-----------------------------------------------------------------------------
 */

static void
DnDUriListPump(DnDGHRead *rd)  // IN
{
   g_input_stream_read_bytes_async(rd->stream, DND_URI_LIST_CHUNK,
                                   G_PRIORITY_DEFAULT, rd->cancel,
                                   DnDUriListReadChunk, rd);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUriListReadChunk --
 *
 *    Add a chunk to the uri-list, and at the end of the stream hand the list
 *    to the UI.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Frees the read once it is over.
 *
 *-----------------------------------------------------------------------------
 */

static void
DnDUriListReadChunk(GObject *source,       // IN: the GInputStream
                    GAsyncResult *res,     // IN
                    gpointer data)         // IN: DnDGHRead *
{
   DnDGHRead *rd = (DnDGHRead *) data;
   GError *error = NULL;
   GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source),
                                                    res, &error);

   if (bytes == NULL) {
      g_debug("%s: read failed: %s\n", __FUNCTION__,
              error != NULL ? error->message : "unknown");
      g_clear_error(&error);
      DnDGHReadFree(rd);
      return;
   }

   gsize len = g_bytes_get_size(bytes);

   if (len > 0) {
      gsize n;
      const char *p = (const char *) g_bytes_get_data(bytes, &n);

      g_string_append_len(rd->acc, p, n);
      g_bytes_unref(bytes);

      if (rd->acc->len > DND_URI_LIST_MAX) {
         g_warning("%s: uri list is implausibly large, giving up\n",
                   __FUNCTION__);
         DnDGHReadFree(rd);
         return;
      }
      DnDUriListPump(rd);
      return;
   }

   /* Zero bytes means end of stream. */
   g_bytes_unref(bytes);
   g_debug("%s: got %" G_GSIZE_FORMAT " bytes of text/uri-list\n",
           __FUNCTION__, (gsize) rd->acc->len);
   if (rd->ui != NULL) {
      rd->ui->OnGHDropRead(rd->acc->str, rd->type);
   }
   DnDGHReadFree(rd);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUriListGotStream --
 *
 *    The source has started sending the uri-list. Read it.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Frees the read if there is no stream.
 *
 *-----------------------------------------------------------------------------
 */

static void
DnDUriListGotStream(GObject *source,      // IN: the GdkDrop
                    GAsyncResult *res,    // IN
                    gpointer data)        // IN: DnDGHRead *
{
   DnDGHRead *rd = (DnDGHRead *) data;
   GError *error = NULL;
   const char *mime = NULL;
   GInputStream *stream = gdk_drop_read_finish(GDK_DROP(source), res,
                                               &mime, &error);

   if (stream == NULL) {
      g_debug("%s: no stream: %s\n", __FUNCTION__,
              error != NULL ? error->message : "unknown");
      g_clear_error(&error);
      DnDGHReadFree(rd);
      return;
   }

   g_debug("%s: reading drop as %s\n", __FUNCTION__,
           mime != NULL ? mime : DRAG_TARGET_NAME_URI_LIST);
   rd->stream = stream;
   DnDUriListPump(rd);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDTextGotValue --
 *
 *    The dragged text has arrived. Hand it to the UI.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Frees the read.
 *
 *-----------------------------------------------------------------------------
 */

static void
DnDTextGotValue(GObject *source,      // IN: the GdkDrop
                GAsyncResult *res,    // IN
                gpointer data)        // IN: DnDGHRead *
{
   DnDGHRead *rd = (DnDGHRead *) data;
   GError *error = NULL;
   const GValue *value = gdk_drop_read_value_finish(GDK_DROP(source), res,
                                                    &error);

   if (value == NULL || !G_VALUE_HOLDS_STRING(value)) {
      g_debug("%s: no text: %s\n", __FUNCTION__,
              error != NULL ? error->message : "unknown");
      g_clear_error(&error);
      DnDGHReadFree(rd);
      return;
   }

   g_debug("%s: got the text as %s\n", __FUNCTION__, rd->type);
   if (rd->ui != NULL) {
      rd->ui->OnGHDropRead(g_value_get_string(value), rd->type);
   }
   DnDGHReadFree(rd);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::StartGHRead --
 *
 *    Begin fetching what the guest is dragging, as a text/uri-list when the
 *    source offers one, and otherwise as text.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    OnGHDropRead() runs when the data has arrived.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::StartGHRead(const Glib::RefPtr< Gdk::Drop > &drop)  // IN
{
   static const char *mimes[] = { DRAG_TARGET_NAME_URI_LIST, NULL };
   GdkContentFormats *formats = gdk_drop_get_formats(drop->gobj());
   DnDGHRead *rd;

   /* Cancel any previous read, which is stale now. */
   CancelGHRead();
   mGHReadCancel = g_cancellable_new();

   rd = g_new0(DnDGHRead, 1);
   rd->ui = this;
   rd->cancel = G_CANCELLABLE(g_object_ref(mGHReadCancel));
   mGHReads = g_slist_prepend(mGHReads, rd);

   if (gdk_content_formats_contain_mime_type(formats,
                                             DRAG_TARGET_NAME_URI_LIST)) {
      rd->type = DRAG_TARGET_NAME_URI_LIST;
      rd->acc = g_string_new(NULL);
      gdk_drop_read_async(drop->gobj(), (const char **) mimes,
                          G_PRIORITY_DEFAULT, rd->cancel,
                          DnDUriListGotStream, rd);
      return;
   }

   /*
    * GDK hands RTF over only as the text GTK converts it to. The type says
    * which of SetCPClipboardFromGtk()'s paths takes it, and matches what
    * DragDetWnd::Get_Drop_Value() reports for the same drop.
    */
   rd->type = gdk_content_formats_contain_mime_type(formats,
                                                    TARGET_NAME_TEXT_RTF)
              ? TARGET_NAME_TEXT_RTF : TARGET_NAME_STRING;
   gdk_drop_read_value_async(drop->gobj(), G_TYPE_STRING, G_PRIORITY_DEFAULT,
                             rd->cancel, DnDTextGotValue, rd);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::CancelGHRead --
 *
 *      Abandon any outstanding guest to host data read.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::CancelGHRead(void)
{
   GSList *l;

   /*
    * Detach first. A callback whose result was already queued when the cancel
    * arrived still runs, and must not reach this object once the drag is over
    * or the object is being destroyed. Each read frees itself in its own
    * callback.
    */
   for (l = mGHReads; l != NULL; l = l->next) {
      ((DnDGHRead *) l->data)->ui = NULL;
   }
   g_slist_free(mGHReads);
   mGHReads = NULL;

   if (mGHReadCancel != NULL) {
      g_cancellable_cancel(mGHReadCancel);
      g_object_unref(mGHReadCancel);
      mGHReadCancel = NULL;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::ForgetGHRead --
 *
 *    Drop a finished read from the live list.
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
DnDUIX11::ForgetGHRead(DnDGHRead *read)  // IN
{
   mGHReads = g_slist_remove(mGHReads, read);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGHDropRead --
 *
 *      What the guest is dragging has arrived. Turn it into the
 *      cross-platform clipboard and tell the host, which is what
 *      OnGtkDragDataReceived() did when GTK preloaded the value.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May start a guest to host drag with the host, or reset the UI.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnGHDropRead(const char *data,   // IN
                       const char *type)   // IN: a TARGET_NAME_* type
{
   std::string value(data != NULL ? data : "");
   std::string valueType(type);

   if (!mGHDnDInProgress) {
      g_debug("%s: drag already finished\n", __FUNCTION__);
      return;
   }
   if (value.empty()) {
      g_debug("%s: nothing to send\n", __FUNCTION__);
      ResetUI();
      return;
   }
   if (!SetCPClipboardFromGtk(value, valueType)) {
      g_debug("%s: failed to set CP clipboard\n", __FUNCTION__);
      ResetUI();
      return;
   }
   if (CPClipboard_IsEmpty(&mClipboard)) {
      g_debug("%s: clipboard came out empty\n", __FUNCTION__);
      ResetUI();
      return;
   }
   if (!mGHDnDDataReceived) {
      g_debug("%s: drag entering, telling the host\n", __FUNCTION__);
      mGHDnDDataReceived = true;
      TargetDragEnter();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkDragAccept --
 *
 *      GTK4 "signal_accept" signal handler for DropTarget.
 *
 * Results:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::OnGtkDragAccept(const Glib::RefPtr< Gdk::Drop > &drop, void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   if (!this_instance->mDnD->IsDnDAllowed()) {
      g_warning("%s: No dnd allowed!\n", __FUNCTION__);
      return false;
   }

   if (!this_instance->mDetWnd->Is_Drop_Supported(drop)) {
      g_warning("%s: No Unsupported DND type\n", __FUNCTION__);
      return false;
   }

   if (this_instance->mInHGDrag) {
      g_warning("%s: In H->G dnd process rather than G->H, exit\n", __FUNCTION__);
      return false;
   }

   if (this_instance->mGHDnDInProgress) {
      /*
       * Errors in the GTK layer can lead to dropped information without
       * notification. Therefore, a cleanup process is necessary.
       */
      g_warning("%s: Previous drag failed\n", __FUNCTION__);
      this_instance->ResetUI();
   }

   g_debug("%s: new drag, need to get data for host\n", __FUNCTION__);
   /*
    * This is a new drag operation. We need to start a drag thru the
    * RPC, and to the host. Before we can tell the host, we have to
    * retrieve the drop data.
    */
   this_instance->mGHDnDInProgress = true;
   /* only begin drag enter after we get the data */
   /* Need to grab all of the data. */
   CPClipboard_Clear(&this_instance->mClipboard);
   this_instance->mNumPendingRequest = 0;

   /*
    * Fetch the data here rather than through GTK's preload, which picks the
    * portal type for a file list and cannot redeem it. See "Reading what is
    * dragged from the guest" above.
    */
   this_instance->StartGHRead(drop);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkDragEnter --
 *
 *       GTK4 "signal_enter" signal handler for DropTarget.
 *
 * Results:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Gdk::DragAction
DnDUIX11::OnGtkDragEnter(double x, // IN: drag motion x-coord
                         double y, // IN: drag motion y-coord
                         void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   Gdk::DragAction action;

   if (this_instance->mDetWnd->Is_Current_Drop_Supported()) {
      action = Gdk::DragAction::COPY;
   } else {
      g_warning("%s: No dnd allowed @ (%f, %f)!\n", __FUNCTION__, x, y);
   }

   return action;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkDragDrop --
 *
 *      GTK+ "drag_drop" signal handler.
 *
 * Results:
 *      Returns true so long as drag target and data are (at one point)
 *      provided (i.e. not a spurious event).
 *
 * Side effects:
 *      Signals to drag source that drop is finished.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::OnGtkDragDrop(const Glib::ValueBase& value,
                        double x,
                        double y,
                        void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   g_debug("%s: Ctx %ld, drop at x %f y %f\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drop_ctx(), x, y);

   this_instance->ResetUI();

   if (CPClipboard_IsEmpty(&this_instance->mClipboard)) {
      g_debug("%s: No valid data on mClipboard.\n", __FUNCTION__);
      return false;
   }

   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkDragLeave --
 *
 *      GTK+ "drag_leave" signal handler.
 *
 *      Logs event.  Acknowledges, finishes outdated sequence if drag context
 *      is not the same as we're currently interested in (i.e. != mDragCtx).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May cancel dnd associated with this drag context.
 *
 *-----------------------------------------------------------------------------
 */

 void
 DnDUIX11::OnGtkDragLeave(void *ctx)
 {
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;

   g_debug("%s: DND leave for drop %ld\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drop_ctx());

   if (this_instance->mGHDnDInProgress) {
      g_debug("%s: reset UI\n", __FUNCTION__);
      this_instance->ResetUI();
   }
 }


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkDragMotion --
 *
 *      GTK "drag_motion" signal handler.
 *
 *      We should respond by setting drag status. Note that there is no drag
 *      enter signal. We need to figure out if a new drag is happening on
 *      our own. Also, we don't respond with a "allowed" drag status right
 *      away, we start a new drag operation with the host (which tries to
 *      notify the host of the new operation). Once the host has responded),
 *      we respond with a proper drag status.
 *
 *      Note: You may see this callback during DnD when detection window
 *      is acting as a source. In that case it will be ignored. In a future
 *      refactoring, we will try and avoid this.
 *
 * Results:
 *      Returns true unless we don't recognize the types offered.
 *
 * Side effects:
 *      Via RequestData issues a Gtk::Widget::drag_get_data.
 *
 *-----------------------------------------------------------------------------
 */

Gdk::DragAction
DnDUIX11::OnGtkDragMotion(double x, // IN: drag motion x-coord
                          double y, // IN: drag motion y-coord
                          void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   Gdk::DragAction action = Gdk::DragAction::COPY;
   /*
    * If this is a Host to Guest drag, we are done here, so return.
    */
   unsigned long curTime = GetTimeInMillis();
   g_debug("%s: enter dnd ctx %ld\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drop_ctx());
   if (curTime - this_instance->mDestDropTime <= 1000) {
      g_debug("%s: ignored %ld %ld %ld\n", __FUNCTION__,
              curTime, this_instance->mDestDropTime,
              curTime - this_instance->mDestDropTime);
      goto out;
   }

   g_debug("%s: not ignored %ld %ld %ld\n", __FUNCTION__,
           curTime, this_instance->mDestDropTime,
           curTime - this_instance->mDestDropTime);

   if (this_instance->mInHGDrag ||
       (this_instance->mHGGetFileStatus != DND_FILE_TRANSFER_NOT_STARTED)) {
      g_debug("%s: ignored not in hg drag or not getting hg data\n",
              __FUNCTION__);
      goto out;
   }
   if (!this_instance->mGHDnDInProgress) {
      g_debug("%s: new drag, need to get data for host\n", __FUNCTION__);
      /*
       * This is a new drag operation. We need to start a drag thru the
       * backdoor, and to the host. Before we can tell the host, we have to
       * retrieve the drop data.
       */
      this_instance->mGHDnDInProgress = true;
      /* only begin drag enter after we get the data */
      /* Need to grab all of the data. */
      CPClipboard_Clear(&this_instance->mClipboard);
      this_instance->mNumPendingRequest = 0;
   } else {
      g_debug("%s: Multiple drag motions before gh data has been received.\n",
              __FUNCTION__);
   }
out:
   return action;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGtkDragDataReceived --
 *
 *      GTK+ "drag_data_received" signal handler.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May signal to host beginning of guest-to-host DND.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnGtkDragDataReceived(void *ctx)
{
   DnDUIX11 *this_instance = (DnDUIX11 *)ctx;
   std::string value;
   std::string value_type;

   g_debug("%s: enter dnd ctx %ld\n", __FUNCTION__,
           this_instance->mDetWnd->Get_Current_Drop_ctx());
   /* The GH DnD may already finish before we got response. */
   if (!this_instance->mGHDnDInProgress) {
      g_debug("%s: not valid\n", __FUNCTION__);
      return;
   }

   if (this_instance->mDetWnd->Is_DropValue_Ready()) {
      if (this_instance->mDetWnd->Get_Drop_Value(value, value_type)) {
         if (this_instance->SetCPClipboardFromGtk(value, value_type)) {
            this_instance->mNumPendingRequest--;
            if (this_instance->mNumPendingRequest > 0) {
               return;
            }
            if (CPClipboard_IsEmpty(&this_instance->mClipboard)) {
               g_debug("%s: Failed getting item.\n", __FUNCTION__);
               this_instance->ResetUI();
               return;
            }
            if (!this_instance->mGHDnDDataReceived) {
               g_debug("%s: Drag entering.\n", __FUNCTION__);
               this_instance->mGHDnDDataReceived = true;
               this_instance->TargetDragEnter();
            }
         } else {
            g_debug("%s: Failed to set CP clipboard.\n", __FUNCTION__);
            this_instance->ResetUI();
         }
      } else {
         g_debug("%s: DND data is not ready, Could be a drag leave.\n",
                 __FUNCTION__);
      }
   }
}


/*
 * END GTK+ Callbacks (dndcp as drag destination: guest-to-host)
 ****************************************************************************
 */


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SetCPClipboardFromGtk_File --
 *
 *      Extract file list from GTK4.
 *      And construct cross-platform clipboard.
 *
 * Results:
 *      Returns true if conversion succeeded, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

bool DnDUIX11::SetCPClipboardFromGtk_File(utf::string &source)
{
   DnDFileList fileList;
   DynBuf buf;
   size_t index = 0;
   char *newPath;
   size_t newPathLen;
   uint64 totalSize = 0;
   int64 size;

   g_debug("%s: Got file list: [%s]\n", __FUNCTION__, source.c_str());

   if (source.length() == 0) {
      g_debug("%s: empty file list!\n", __FUNCTION__);
      return false;
   }

   /*
    * In gnome, before file list there may be a extra line indicating it
    * is a copy or cut.
    */
   if (source.length() >= 5 && source.compare(0, 5, "copy\n") == 0) {
      source = source.erase(0, 5);
   }

   if (source.length() >= 4 && source.compare(0, 4, "cut\n") == 0) {
      source = source.erase(0, 4);
   }

   while (source.length() > 0 &&
          (source[0] == '\n' || source[0] == '\r' || source[0] == ' ')) {
      source = source.erase(0, 1);
   }

   while ((newPath = DnD_UriListGetNextFile(source.c_str(),
                                            &index,
                                            &newPathLen)) != NULL) {
      char *newRelPath;

      /*TODO: refactor compiler option segments and merge them together*/
#if defined(__linux__)
      if (DnD_UriIsNonFileSchemes(newPath)) {
         /* Try to get local file path for non file uri. */
         GFile *file = g_file_new_for_uri(newPath);
         free(newPath);
         if (!file) {
            g_debug("%s: g_file_new_for_uri failed\n", __FUNCTION__);
            return false;
         }
         newPath = g_file_get_path(file);
         g_object_unref(file);
         if (!newPath) {
            g_debug("%s: g_file_get_path failed\n", __FUNCTION__);
            return false;
         }
      }
#endif
      /*
       * Parse relative path. The drag source controls these bytes. An entry
       * with no path separator, such as the bare "file:foo" form, or with
       * nothing after the last separator has no relative part. Skip it
       * instead of pointing past the end of the string.
       */
      newRelPath = Str_Strrchr(newPath, DIRSEPC);
      if (newRelPath == NULL || newRelPath[1] == '\0') {
         g_debug("%s: skipping uri without a relative path '%s'\n",
                 __FUNCTION__, newPath);
         free(newPath);
         continue;
      }
      newRelPath++; // Point to char after '/'

      /* Keep track of how big the dnd files are. */
      if ((size = File_GetSizeEx(newPath)) >= 0) {
         totalSize += size;
      } else {
         g_debug("%s: unable to get file size for %s\n", __FUNCTION__, newPath);
      }
      g_debug("%s: Adding newPath '%s' newRelPath '%s'\n", __FUNCTION__,
              newPath, newRelPath);
      fileList.AddFile(newPath, newRelPath);
#if defined(__linux__)
      char *newUri = HgfsUri_ConvertFromPathToHgfsUri(newPath, false);
      fileList.AddFileUri(newUri);
      free(newUri);
#endif
      free(newPath);
   }

   DynBuf_Init(&buf);
   fileList.SetFileSize(totalSize);
   if (fileList.ToCPClipboard(&buf, false)) {
      CPClipboard_SetItem(&mClipboard, CPFORMAT_FILELIST, DynBuf_Get(&buf),
                          DynBuf_GetSize(&buf));
   }
   DynBuf_Destroy(&buf);
#if defined(__linux__)
   if (fileList.ToUriClipboard(&buf)) {
      CPClipboard_SetItem(&mClipboard, CPFORMAT_FILELIST_URI, DynBuf_Get(&buf),
                          DynBuf_GetSize(&buf));
   }
   DynBuf_Destroy(&buf);
#endif
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SetCPClipboardFromGtk_PlainText --
 *
 *      Extract text from GTK4. And construct cross-platform clipboard.
 *
 * Results:
 *      Returns true if conversion succeeded, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

bool DnDUIX11::SetCPClipboardFromGtk_PlainText(utf::string &source)
{
   if (source.size() > 0 &&
       source.size() < DNDMSG_MAX_ARGSZ &&
       CPClipboard_SetItem(&mClipboard, CPFORMAT_TEXT, source.c_str(), source.size() + 1)) {
      g_debug("%s: Got text, size %" FMTSZ "u\n", __FUNCTION__, source.size());
   } else {
      g_warning("%s: Failed to get text\n", __FUNCTION__);
      return false;
   }
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SetCPClipboardFromGtk_RTF --
 *
 *      Extract RTF from GTK4. And construct cross-platform clipboard.
 *
 * Results:
 *      Returns true if conversion succeeded, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

bool DnDUIX11::SetCPClipboardFromGtk_RTF(utf::string &source)
{
   if (source.size() > 0 && source.size() < DNDMSG_MAX_ARGSZ) {
      /*
       * PR3578465, we could not get raw rtf format info from dnd clipboard
       * set to plain text as workaround. And removing one trailing "_" char.
       */
      if (source[source.size() - 1] == '_') {
         source.erase(source.size() - 1, 1);
      }
      if (CPClipboard_SetItem(&mClipboard, CPFORMAT_TEXT, source.c_str(),
                              source.size() + 1)) {
         g_debug("%s: Got RTF and parse as plain text, size %" FMTSZ "u\n",
                 __FUNCTION__, source.size());
         return true;
      } else {
         g_warning("%s: Failed to set target clipboard\n", __FUNCTION__);
      }
   } else {
      g_warning("%s: wrong RTF size from drag source\n", __FUNCTION__);
   }

   return false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SetCPClipboardFromGtk --
 *
 *      Construct cross-platform clipboard from GTK+ selection_data.
 *
 * Results:
 *      Returns true if conversion succeeded, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::SetCPClipboardFromGtk(std::string &data, std::string &drop_type) // IN
{
   bool ret = false;

   utf::string source = utf::string(data);
   utf::string type = utf::string(drop_type);

   /* Try to get file list. */
   if (mDnD->CheckCapability(DND_CP_CAP_FILE_DND) && type == DRAG_TARGET_NAME_URI_LIST) {
      ret = SetCPClipboardFromGtk_File(source);
   }
   /* Try to get plain text. */
   if (mDnD->CheckCapability(DND_CP_CAP_PLAIN_TEXT_DND) && TargetIsPlainText(type)) {
      ret = SetCPClipboardFromGtk_PlainText(source);
   }
   /* Try to get RTF string. */
   if (mDnD->CheckCapability(DND_CP_CAP_RTF_DND) && TargetIsRichText(type)) {
      ret = SetCPClipboardFromGtk_RTF(source);
   }

   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::GetLastDirName --
 *
 *      Try to get last directory name from a full path name.
 *
 *      What this really means is to get the basename of the parent's directory
 *      name, intended to isolate an individual DND operation's staging directory
 *      name.
 *
 *         E.g. /tmp/VMwareDnD/abcd137 → abcd137
 *
 * Results:
 *      Returns session directory name on success, empty string otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

utf::string
DnDUIX11::GetLastDirName(const utf::string &str)
{
   char *baseName;
   utf::string stripSlash;
   char *path = File_StripSlashes(str.c_str());
   if (path) {
      stripSlash = utf::CopyAndFree(path);
   }

   File_GetPathName(stripSlash.c_str(), NULL, &baseName);
   if (baseName) {
      return utf::CopyAndFree(baseName);
   } else {
      return utf::string();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::GetNextPath --
 *
 *      Convoluted somethingerother.
 *
 *      XXX Something here involves URI parsing and encoding.  Get to the bottom
 *      of this and use shared URI code.
 *
 *      Original description:
 *         Provide a substring containing the next path from the provided
 *         NUL-delimited string starting at the provided index.
 *
 * Results:
 *      Returns "a string with the next path or empty string if there are no
 *      more paths".
 *
 * Side effects:
 *      Updates index.
 *
 *-----------------------------------------------------------------------------
 */

utf::utf8string
DnDUIX11::GetNextPath(utf::utf8string& str,     // IN: NUL-delimited path list
                      size_t& index)            // IN/OUT: index into string
{
   utf::utf8string ret;
   size_t start;

   if (index >= str.length()) {
      return "";
   }

   for (start = index; str[index] != '\0' && index < str.length(); index++) {
      /*
       * Escape reserved characters according to RFC 1630.  We'd use
       * Escape_Do() if this wasn't a utf::string, but let's use the same table
       * replacement approach.
       */
      static char const Dec2Hex[] = {
         '0', '1', '2', '3', '4', '5', '6', '7',
         '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
      };

      unsigned char ubyte = str[index];

      if (ubyte == '#' ||   /* Fragment identifier delimiter */
          ubyte == '?' ||   /* Query string delimiter */
          ubyte == '*' ||   /* "Special significance within specific schemes" */
          ubyte == '!' ||   /* "Special significance within specific schemes" */
          ubyte == '%' ||   /* Escape character */
          ubyte == '\r' ||  /* Ends an entry in a uri-list */
          ubyte == '\n' ||  /* Ends an entry in a uri-list */
          ubyte >= 0x80) {  /* UTF-8 encoding bytes */
         str.replace(index, 1, "%");
         str.insert(index + 1, 1, Dec2Hex[ubyte >> 4]);
         str.insert(index + 2, 1, Dec2Hex[ubyte & 0xF]);
         index += 2;
      }
   }

   ret = str.substr(start, index - start);
   g_debug("%s: nextpath: %s", __FUNCTION__, ret.c_str());
   index++;
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::TargetDragEnter --
 *
 *      With the source's drag selection data on the clipboard, signal to
 *      host to begin guest-to-host drag-and-drop.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::TargetDragEnter()
{
   TRACE_CALL();

   /* Check if there is valid data with current detection window. */
   if (!CPClipboard_IsEmpty(&mClipboard)) {
      g_debug("%s: got valid data from detWnd.\n", __FUNCTION__);
      mDnD->DestUIDragEnter(&mClipboard);
   }

   /*
    * Show the window, and position it under the current mouse position.
    * This is particularly important for KDE 3.5 guests.
    */
   mDetWnd->SimulateXEvents(true, false, true, true, false, 0, 0);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SourceDragStartDone --
 *
 *      Tell host that we're done with host-to-guest drag-and-drop
 *      initialization.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::SourceDragStartDone()
{
   TRACE_CALL();
   mInHGDrag = true;
   mDnD->SrcUIDragBeginDone();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SetBlockControl --
 *
 *      Set block control member.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::SetBlockControl(DnDBlockControl *blockCtrl)   // IN
{
   mBlockCtrl = blockCtrl;
}



/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::SourceUpdateFeedback --
 *
 *      Forward feedback from our drop source to the host.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::SourceUpdateFeedback(DND_DROPEFFECT effect) // IN: feedback to send to the UI-independent DnD layer.
{
   TRACE_CALL();
   mDnD->SrcUIUpdateFeedback(effect);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::GetTimeInMillis --
 *
 *      Get Unix time in milliseconds.
 *
 *-----------------------------------------------------------------------------
 */

unsigned long
DnDUIX11::GetTimeInMillis()
{
   VmTimeType atime;

   Hostinfo_GetTimeOfDay(&atime);
   return((unsigned long)(atime / 1000));
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::VmXDnDVersionChanged --
 *
 *      Update version information in mDnD.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::VmxDnDVersionChanged(RpcChannel *chan,        // IN
                               uint32 version)          // IN
{
   ASSERT(mDnD);
   mDnD->VmxDnDVersionChanged(version);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::UpdateWorkArea --
 *
 *      Updates mOrigin in response to changes to _NET_workArea.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::UpdateWorkArea()
{
   TRACE_CALL();

   std::vector<unsigned long> values;
   if (xutils::GetCardinalList(mGdkSurface, "_NET_WORKAREA", values)
       && values.size() > 0
       && values.size() % 4 == 0) {
      /*
       * wm-spec: _NET_WORKAREA, x, y, width, height CARDINAL[][4]/32
       *
       * For the purposes of drag-and-drop, we're okay with using the screen-
       * agnostic _NET_WORKAREA atom, as the guest VM really deals with only
       * one logical monitor.
       */
      unsigned long desktop = 0;
      xutils::GetCardinal(mGdkSurface, "_NET_CURRENT_DESKTOP", desktop);

      mOriginX = values[0 + 4 * desktop];
      mOriginY = values[1 + 4 * desktop];
   } else {
      mOriginX = 0;
      mOriginY = 0;
   }

   g_debug("%s: new origin at (%d, %d)\n", __FUNCTION__, mOriginX, mOriginY);
}
