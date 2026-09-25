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
 * @file dndUIX11GTK4.h
 *
 *    Implement the methods that allow DnD between host and guest for
 *    protocols V3 or greater on GTK4 lib.
 *
 */

#ifndef __DND_UI_X11_GTK4_H__
#define __DND_UI_X11_GTK4_H__

#include "stringxx/string.hh"
#include "dnd.h"
#include "str.h"
#include "util.h"
#include "vmblock.h"
#include "dynbuf.h"
#include "dynxdr.h"
#include "posix.h"

extern "C" {
#include "debug.h"
#include "dndClipboard.h"
#include "vmware/tools/guestrpc.h"
#include "vmware/tools/plugin.h"
}

#include "guestDnD.hh"
#include "dndFileList.hh"
#include "dragDetWndX11GTK4.h"

struct DblLnkLst_Links;
struct DnDGHRead;

/**
 * The DnDUI class implements the UI portion of DnD V3 and greater
 * versions of the protocol.
 */
class DnDUIX11
   : public sigc::trackable
{
public:
   DnDUIX11(ToolsAppCtx *ctx);
   ~DnDUIX11();
   bool Init();
   void VmxDnDVersionChanged(RpcChannel *chan, uint32 version);
   void SetDnDAllowed(bool isDnDAllowed)
      {ASSERT(mDnD); mDnD->SetDnDAllowed(isDnDAllowed);}
   void SetBlockControl(DnDBlockControl *blockCtrl);

   /*
    * Called from the async guest to host read callbacks, which are not
    * members of the class.
    */
   void OnGHDropRead(const char *data, const char *type);
   void ForgetGHRead(DnDGHRead *read);

private:
   /*
    * Blocking FS Helper Functions.
    */
   void AddBlock();
   void RemoveBlock();
   bool LocalWaylandDragPreferred(void);
   void LocalXdndChoosePath(void);
   bool LocalMutterBridgesXdnd(void);
   void LocalXdndLeaveDetWnd(void);
   static gboolean OnXdndBeginTimeout(gpointer ctx);
   void LocalXdndRelease(void);
   void LocalXdndForget(void);
   void LocalXdndRestoreDetWnd(void);
   bool LocalXdndIsStaleDrag(GdkDrag *drag);
   static gboolean OnXdndReleaseTick(gpointer ctx);
   static gboolean OnGHDropCheck(gpointer ctx);
   bool StageFilePlaceholders(const DnDFileList &fList);
   void ClearFilePlaceholders(void);
   void AddBlockForDrop(void);
   void
   BuildFileURIList(utf::string& uriList,
                    utf::string pre,
                    utf::string post,
                    utf::string stagingDirName);
   void
   BuildFileContentURIList(utf::string& uriList,
                           utf::string pre,
                           utf::string post);
   bool LocalFileListIsSafe(const DnDFileList &fList);
   void LocalRefuseDrag(void);
   void LocalGetFileRequest(utf::string stagingDirName, const char *desktop);
   void LocalGetFileContentsRequest(const char *desktop);
   void AddDragItem(const char *mimeType, const void *data, size_t size);
   void LocalSetContentProvider();

   /*
    * Callbacks from Common DnD layer.
    */

   void ResetUI();
   void OnMoveMouse(int32 x, int32 y);

   /*
    * Source functions for HG DnD.
    */
   void OnSrcDragBegin(const CPClipboard *clip, std::string stagingDir);
   void OnSrcDrop();
   void OnSrcCancel();

   /*
    * Called when GH DnD is completed.
    */
   void OnPrivateDrop(int32 x, int32 y);
   void OnDestCancel();

   /*
    * Source functions for file transfer.
    */
   void OnGetFilesDone(bool success);

   /*
    * The role the detection window plays while it is shown. A drag has to
    * find a drop target, and a press has to find the window it is meant
    * for, but nothing has to find a drag source.
    */
   typedef enum {
      DET_WND_DROP_TARGET,
      DET_WND_PRESS_TARGET,
      DET_WND_DRAG_SOURCE
   } DetWndRole;

   /*
    * One format of what the host is dragging. Both drag sources offer the
    * same bytes under the same MIME types, so they are collected once.
    */
   typedef struct {
      std::string mimeType;
      std::string data;    /* Raw bytes, which may include NULs. */
   } DragItem;

   /*
    * Callbacks for showing/hiding detection window.
    */
   void ShowDetWnd(bool bShow, int32 x, int32 y, DetWndRole role);
   void OnUpdateDetWnd(bool bShow, int32 x, int32 y);
   void OnDestMoveDetWndToMousePos();
   void SourceDragStartDone();
   void SourceUpdateFeedback(DND_DROPEFFECT effect);

   /* Native Wayland drag source, where the compositor supports one. */
   bool LocalStartWaylandDrag(int32 x, int32 y);
   void LocalCancelWaylandDrag(void);
   static void OnWaylandDragDone(void *ctx);
   static void NudgeWaylandDrag(void *ctx);

   /*
    * The argument to the native drag's done(). It records which drag it was,
    * so that the end of an abandoned drag is not taken for the end of the
    * next one. OnWaylandDragDone() frees it.
    */
   typedef struct {
      DnDUIX11 *ui;
      unsigned int gen;
   } WaylandDragTicket;

   /*
    * Native Wayland drop target for guest to host drags, where the
    * compositor supports the native drag source.
    */
   bool LocalShowNativeDropTarget(int32 x, int32 y);
   void LocalHideNativeDropTarget(bool keepHeldDrag);
   void LocalNudgeNativeDropTarget(int32 x, int32 y);
   static gboolean OnNativeDropShown(gpointer ctx);
   static gboolean OnNativeDropNudge(gpointer ctx);
   static void OnNativeDropData(const char *mimeType, const char *data,
                                size_t size, void *ctx);
   static void OnNativeDrop(void *ctx);
   static void OnNativeDropLeave(void *ctx);

   /*
    * Gtk+ Callbacks: Drag Source
    */
   static void OnGtkSrcDragBegin(const Glib::RefPtr< Gdk::Drag > &drag, void *ctx);
   static void OnGtkSrcDragEnd(const Glib::RefPtr< Gdk::Drag > &drag, bool delete_data, void *ctx);
   static bool OnGtkSrcDragCancel(const Glib::RefPtr< Gdk::Drag > &drag, Gdk::DragCancelReason reason, void *ctx);

   /*
    * Gtk+ Callbacks: Drag Destination.
    */
   static bool OnGtkDragAccept(const Glib::RefPtr< Gdk::Drop > &drop, void *ctx);
   static Gdk::DragAction OnGtkDragEnter(double x, double y, void *ctx);
   static Gdk::DragAction OnGtkDragMotion(double x, double y, void *ctx);
   static bool OnGtkDragDrop(const Glib::ValueBase& value, double x, double y, void *ctx);
   static void OnGtkDragLeave(void *ctx);
   static void OnGtkDragDataReceived(void *ctx);
   /*
    * Target function for GH DnD. Makes call to common layer.
    */
   void TargetDragEnter();
   void LocalBeginGHDrag(void);

   /*
    * Misc methods.
    */
   void InitGtk();

   bool SetCPClipboardFromGtk_File(utf::string &source);
   bool SetCPClipboardFromGtk_PlainText(utf::string &source);
   bool SetCPClipboardFromGtk_RTF(utf::string &source);
   bool SetCPClipboardFromGtk(std::string &data, std::string &drop_type);
   void StartGHRead(const Glib::RefPtr< Gdk::Drop > &drop);
   void CancelGHRead(void);

   utf::string GetLastDirName(const utf::string &str);
   utf::utf8string GetNextPath(utf::utf8string &str, size_t& index);

   static unsigned long GetTimeInMillis();

   static inline bool TargetIsPlainText(const utf::string& target) {
      return    target == TARGET_NAME_STRING
             || target == TARGET_NAME_TEXT_PLAIN
             || target == TARGET_NAME_UTF8_STRING
             || target == TARGET_NAME_COMPOUND_TEXT;
   }

   static inline bool TargetIsRichText(const utf::string& target) {
      return    target == TARGET_NAME_APPLICATION_RTF
             || target == TARGET_NAME_TEXT_RICHTEXT
             || target == TARGET_NAME_TEXT_RTF;
   }

   void UpdateWorkArea();

   ToolsAppCtx *mCtx;
   GuestDnDMgr *mDnD;
   utf::string mHGStagingDir;
   std::vector<utf::string> mHGFileContentsList;
   DragDetWnd *mDetWnd;
   CPClipboard mClipboard;
   DnDBlockControl *mBlockCtrl;
   DND_FILE_TRANSFER_STATUS mHGGetFileStatus;
   bool mBlockAdded;
   /* The path the current block was added on, which RemoveBlock() removes. */
   utf::string mBlockedPath;

   /* State to determine if drag motion is a drag enter. */
   bool mGHDnDInProgress;
   /* Icon updates from the guest. */
   /* Only update mouse when we have clipboard contents from the host. */
   bool mGHDnDDataReceived;
   GCancellable *mGHReadCancel;
   GSList *mGHReads;
   bool mInHGDrag;
   // Flag to indicate if H->G drag has been triggered in guest by simulate mouse press
   bool mHGDragStarted;
   /* A faked press is outstanding; it must not outlive the drag. */
   bool mHGButtonDown;
   /*
    * The XDND drag was pressed on the detection window and the pointer stays
    * there until GTK has started the drag. See LocalXdndLeaveDetWnd().
    */
   bool mHGXdndWaitBegin;
   guint mHGXdndBeginTimer;
   gint64 mHGXdndBeginDeadline;
   int mHGXdndPressX;
   int mHGXdndPressY;
   int mHGXdndStep;
   /* Host motions on which the press could not be placed; see OnMoveMouse(). */
   int mHGXdndPressRounds;
   /* The GTK drag, for the target's answer at the drop. */
   GdkDrag *mHGXdndDrag;
   guint mHGXdndReleaseTimer;
   gint64 mHGXdndLeaveAt;
   gint64 mHGXdndLeftAt;
   gint64 mHGXdndDropAt;
   int mHGXdndReleaseTicks;
   /* See OnGHDropCheck(). */
   guint mGHDropCheckTimer;
   /* See LocalXdndChoosePath(). */
   bool mMutterBridgesXdnd;
   /* A native Wayland drag is running, so the XDND teardown must not run. */
   bool mWaylandDragActive;
   /* Which native drag is current; a done() for any other is ignored. */
   unsigned int mWaylandDragGen;
   /*
    * The native drop target stands in for the detection window in the
    * current guest to host drag. See LocalShowNativeDropTarget().
    */
   bool mGHNative;
   /*
    * The host has hidden the detection window, but the guest's drag still
    * holds the native drop target, which stays until the drag ends.
    */
   bool mGHNativeLinger;
   /*
    * The host was told about the drag the native drop target holds. Only
    * such a drag keeps the drop target; any other is let through.
    */
   bool mGHNativeHostTold;
   /* The drag the drop target already held is to be handed over again. */
   bool mGHNativeResend;
   guint mGHNativeNudgeTimer;
   int mGHNativeNudgesLeft;
   /* Where ShowDetWnd() was asked to show the drop target. */
   int32 mGHNativeExitX;
   int32 mGHNativeExitY;
   /* The native drop target was never reached; use the X window instead. */
   bool mGHNativeFallback;
   int32 mGHNativeX;
   int32 mGHNativeY;
   /* The staging directory holds placeholders, and the drop adds the block. */
   bool mHGDeferBlock;
   DND_DROPEFFECT mEffect;
   int32 mMousePosX;
   int32 mMousePosY;
   int mNumPendingRequest;
   unsigned long mDestDropTime;
   uint64 mTotalFileSize;

   /* GdkSurface used in dnd */
   Glib::RefPtr< Gdk::Surface > mGdkSurface;
   Glib::RefPtr<Gdk::ContentProvider> mProvider;
   /* The same formats the provider carries, for the Wayland drag source. */
   std::vector<DragItem> mDragItems;
   utf::string mHGDndUriList;
   utf::utf8string mHGFCPData;

   /*
    * Upper left corner of our work area, a safe place for us to place
    * our detection window without clashing with a windows parented to the
    * composite overlay window.
    */
   int mOriginX;
   int mOriginY;
};

#endif // __DND_UI_X11_H__
