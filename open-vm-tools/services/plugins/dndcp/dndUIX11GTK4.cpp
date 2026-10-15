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
#include "hgfs.h"
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
#include "waylandDrag.h"

#include <fcntl.h>
#include <glib/gstdio.h>

/*
 * How long to wait for the compositor to deliver a faked press. The wait
 * ends as soon as the press arrives. The full time passes only when the
 * compositor sent the press somewhere else, and the drag then falls back to
 * XDND.
 */
#define WAYLAND_DRAG_PRESS_TIMEOUT_MS 250

/*
 * How long the target has to catch up with the pointer before the button is
 * released. A file manager that will take the drop accepts within a few tens
 * of milliseconds. OnSrcDrop() waits this long up to three times, with a
 * nudge and then WaylandDrag_Refocus() in between, and then releases the
 * button whatever the target said. A target that still refuses loses the
 * drop.
 */
#define WAYLAND_DRAG_ACCEPT_TIMEOUT_MS 500

/*
 * How an XDND drag that Mutter bridges is started. After the press the
 * pointer moves across the detection window in small steps, one per tick,
 * with the main loop running in between. GTK does not start a drag from
 * motion that all arrives at once. The pointer then stays on the window
 * until GTK has started the drag, or until the timeout.
 */
#define XDND_DRAG_BEGIN_TICK_MS 20
#define XDND_DRAG_BEGIN_STEPS 6
#define XDND_DRAG_BEGIN_TIMEOUT_MS 500

/*
 * How many times, and how far apart, the pointer is placed on the detection
 * window before the press, until X reports it there. When every try fails,
 * the press is tried again on the host's next motion, for up to
 * XDND_PRESS_ROUNDS motions, and after that the drag is given up.
 */
#define XDND_PRESS_TRIES 10
#define XDND_PRESS_RETRY_MS 20
#define XDND_PRESS_ROUNDS 3

/*
 * How long the pointer stays on the detection window after GTK has started
 * the drag. GTK takes XdndSelection as it starts the drag, and Mutter checks
 * where the pointer is when it sees that. Leaving at once loses the race
 * whenever the pointer's implicit grab is not holding it on the window. On
 * GNOME 50, after a click in another window, 150 ms still lost about one
 * drag in 40, and 300 ms lost none in 80.
 */
#define XDND_LEAVE_DELAY_MS 300

/* How long a drop that came before the pointer left waits for it to leave. */
#define XDND_DROP_WAIT_LEAVE_MS 1500

/*
 * How long a bridged XDND drop waits for the target to accept before the
 * button comes up anyway, and how many ticks of XDND_DRAG_BEGIN_TICK_MS pass
 * between nudges.
 */
#define XDND_DROP_ACCEPT_TIMEOUT_MS 1500
#define XDND_DROP_NUDGE_TICKS 4

/*
 * How long after the pointer leaves the detection window an accept can be
 * trusted. The detection window is a drop target too, for guest to host
 * drags, so it answers the drag while the drag starts over it, and GTK
 * keeps that answer until the next target sends its own. Mutter answers
 * for the window under the pointer within a few milliseconds of a motion.
 */
#define XDND_DROP_SETTLE_MS 100
/* How long after a guest to host drop OnGHDropCheck() looks at the drag. */
#define GH_DROP_CHECK_MS 300

/*
 * How many times, and how far apart, the pointer is moved onto the native
 * drop target after LocalShowNativeDropTarget() maps it. The compositor
 * offers the drag to the surface under the pointer only on a motion.
 */
#define GH_NATIVE_NUDGES 4
#define GH_NATIVE_NUDGE_MS 50


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
      mHGXdndWaitBegin(false),
      mHGXdndBeginTimer(0),
      mHGXdndBeginDeadline(0),
      mHGXdndPressX(0),
      mHGXdndPressY(0),
      mHGXdndStep(0),
      mHGXdndPressRounds(0),
      mHGXdndDrag(NULL),
      mHGXdndReleaseTimer(0),
      mHGXdndLeaveAt(0),
      mHGXdndLeftAt(0),
      mHGXdndDropAt(0),
      mHGXdndReleaseTicks(0),
      mGHDropCheckTimer(0),
      mMutterBridgesXdnd(false),
      mWaylandDragActive(false),
      mWaylandDragGen(0),
      mGHNative(false),
      mGHNativeExitX(0),
      mGHNativeExitY(0),
      mGHNativeFallback(false),
      mGHNativeLinger(false),
      mGHNativeHostTold(false),
      mGHNativeResend(false),
      mGHNativeNudgeTimer(0),
      mGHNativeNudgesLeft(0),
      mGHNativeX(0),
      mGHNativeY(0),
      mHGDeferBlock(false),
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
    *
    * There is no display when DISPLAY is unset. Init() then fails, and drag
    * and drop stays off.
    */
   Glib::RefPtr<Gdk::Display> display = Gdk::Display::get_default();
   if (display && GDK_IS_X11_DISPLAY(display->gobj())) {
      GdkSurface *gdkSurface =
         gdk_x11_display_get_default_group(display->gobj());
      mGdkSurface = Glib::wrap(gdkSurface);
   }
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

   if (mGHDropCheckTimer != 0) {
      g_source_remove(mGHDropCheckTimer);
      mGHDropCheckTimer = 0;
   }
   LocalHideNativeDropTarget(false);
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
   /*
    * Also for a drag that is armed but has not started. Any done() from
    * WaylandDrag_Exit() then finds a stale ticket.
    */
   LocalCancelWaylandDrag();
   WaylandDrag_Exit();
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

/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalTiming --
 *
 *    Read one drag timing from the [dnd] section of tools.conf.
 *
 *    The value is a number of milliseconds. Anything that is not a positive
 *    number is ignored and the default is used, so a typo cannot turn a wait
 *    into a busy loop.
 *
 * Results:
 *    The configured value, or defaultMs.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

int
DnDUIX11::LocalTiming(const char *key,     // IN: key in the [dnd] section
                      int defaultMs)       // IN: value to use without one
{
   GError *err = NULL;
   int value = defaultMs;

   if (mCtx != NULL && mCtx->config != NULL) {
      value = g_key_file_get_integer(mCtx->config, "dnd", key, &err);
      if (err != NULL) {
         /* Not set, which is the normal case; keep the default. */
         g_clear_error(&err);
         value = defaultMs;
      } else if (value <= 0) {
         g_warning("%s: [dnd] %s is not a positive number of milliseconds, "
                   "using %d\n", __FUNCTION__, key, defaultMs);
         value = defaultMs;
      } else {
         g_debug("%s: [dnd] %s = %d ms\n", __FUNCTION__, key, value);
      }
   }

   return value;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::Init --
 *
 * Results:
 *    TRUE on success.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::Init()
{
   TRACE_CALL();
   bool ret = true;

   /*
    * Read the drag timings before anything can use them. Each falls back to
    * the measured default when tools.conf does not set it.
    */
   mXdndLeaveDelayMs =
      LocalTiming("xdndLeaveDelayMs", XDND_LEAVE_DELAY_MS);
   mXdndDragBeginTimeoutMs =
      LocalTiming("xdndDragBeginTimeoutMs", XDND_DRAG_BEGIN_TIMEOUT_MS);
   mWaylandDragAcceptTimeoutMs =
      LocalTiming("waylandDragAcceptTimeoutMs", WAYLAND_DRAG_ACCEPT_TIMEOUT_MS);
   mXdndDropAcceptTimeoutMs =
      LocalTiming("xdndDropAcceptTimeoutMs", XDND_DROP_ACCEPT_TIMEOUT_MS);

   CPClipboard_Init(&mClipboard);

   /* The detection window is an X11 window. */
   if (!mGdkSurface) {
      g_warning("%s: no X display, drag and drop is off\n", __FUNCTION__);
      return false;
   }

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

   /*
    * Optional. Where the compositor does not support a native drag source,
    * host to guest drags use XDND. OnSrcDragBegin() tries again if this
    * fails.
    */
   WaylandDrag_Init();
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
    * The button is about to be released, and a native drag would take that
    * as the drop. The caller of the reset has decided the drag is over, so
    * cancel the drag rather than drop it on whatever is under the pointer.
    */
   if (mWaylandDragActive) {
      LocalCancelWaylandDrag();
   }
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
   LocalXdndForget();
   LocalXdndRestoreDetWnd();
   mGHDnDDataReceived = false;
   mHGGetFileStatus = DND_FILE_TRANSFER_NOT_STARTED;
   mGHDnDInProgress = false;
   mEffect = DROP_NONE;
   mInHGDrag = false;
   CancelGHRead();
   ClearFilePlaceholders();
   RemoveBlock();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalWaylandDragPreferred --
 *
 *      Whether the drag about to start will try the native Wayland source
 *      before XDND.
 *
 *      The source is chosen when the first motion arrives, and the native one
 *      can still fail then and fall back to XDND. The staging decisions have
 *      to be made before the offer is built, so they are made on these
 *      conditions.
 *
 * Results:
 *      True if the native source will be tried.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::LocalWaylandDragPreferred(void)
{
#ifdef USE_UINPUT
   return WaylandDrag_IsAvailable() && FakeMouse_IsInit();
#else
   return false;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalXdndChoosePath --
 *
 *      Decide, at the start of a drag, whether Mutter bridges the XDND drag
 *      between the detection window and Wayland clients, and log the
 *      decision. That is the case in a Wayland session with no native drag
 *      source when the X window manager is Mutter. Every other session keeps
 *      the upstream XDND behaviour.
 *
 *      Mutter starts the bridge when the X client takes XdndSelection, and
 *      only if the pointer is then over an Xwayland window with a button
 *      held. So the drag has to start while the pointer is still on the
 *      detection window, and the pointer must stay unlocked for the rest of
 *      the drag. See LocalXdndLeaveDetWnd() and DragDetWnd::SimulateXEvents().
 *      Other compositors without a layer shell may bridge XDND too, but
 *      these steps and their timings were measured on Mutter only.
 *
 *      The window manager is read again on every drag because it can be
 *      replaced while the plugin runs, and the read costs a few X round
 *      trips.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets mMutterBridgesXdnd, which LocalMutterBridgesXdnd() returns.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalXdndChoosePath(void)
{
   const char *type = getenv("XDG_SESSION_TYPE");
   std::string wmName;

   mMutterBridgesXdnd = false;
   if (type == NULL || strcmp(type, "wayland") != 0) {
      g_debug("%s: Mutter does not bridge XDND (not a Wayland session)\n",
              __FUNCTION__);
   } else if (WaylandDrag_IsAvailable()) {
      g_debug("%s: Mutter does not bridge XDND (native drag source)\n",
              __FUNCTION__);
   } else if (mDetWnd == NULL || !mDetWnd->WmIsMutter(wmName)) {
      g_debug("%s: Mutter does not bridge XDND (window manager \"%s\")\n",
              __FUNCTION__, wmName.c_str());
   } else {
      mMutterBridgesXdnd = true;
      g_debug("%s: Mutter bridges XDND (window manager \"%s\")\n",
              __FUNCTION__, wmName.c_str());
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalMutterBridgesXdnd --
 *
 *      Whether the current drag takes the steps Mutter needs to bridge it to
 *      Wayland clients. See LocalXdndChoosePath().
 *
 * Results:
 *      True if the drag needs that treatment.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::LocalMutterBridgesXdnd(void)
{
   return mMutterBridgesXdnd;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalXdndLeaveDetWnd --
 *
 *      Move the pointer from the detection window to where the host has it,
 *      once GTK has started the drag or has had the time to.
 *
 *      GTK handles the press and the motion that start the drag only when
 *      the main loop runs, which is after OnMoveMouse() returns. Moving the
 *      pointer away inside OnMoveMouse(), as the other paths do, means the
 *      drag starts with the pointer over a Wayland window. Mutter then does
 *      not bridge it, whenever the pointer's implicit grab no longer holds
 *      it on the detection window, which is the case after any click on a
 *      Wayland window in the session.
 *
 * Side effects:
 *      Moves the pointer.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalXdndLeaveDetWnd(void)
{
   if (!mHGXdndWaitBegin) {
      return;
   }
   mHGXdndWaitBegin = false;
   mHGXdndLeftAt = g_get_monotonic_time();
   if (mHGXdndBeginTimer != 0) {
      g_source_remove(mHGXdndBeginTimer);
      mHGXdndBeginTimer = 0;
   }
   g_debug("%s: moving to the host's pointer at (%d, %d)\n", __FUNCTION__,
           mMousePosX, mMousePosY);
   mDetWnd->SimulateXEvents(false, false, false, false, true,
                            mMousePosX, mMousePosY);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalXdndForget --
 *
 *      Stop the timers of a bridged XDND drag, clear its wait state, and
 *      release the reference to its GTK drag.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalXdndForget(void)
{
   mHGXdndWaitBegin = false;
   mHGXdndLeftAt = 0;
   if (mHGXdndBeginTimer != 0) {
      g_source_remove(mHGXdndBeginTimer);
      mHGXdndBeginTimer = 0;
   }
   if (mHGXdndReleaseTimer != 0) {
      g_source_remove(mHGXdndReleaseTimer);
      mHGXdndReleaseTimer = 0;
   }
   if (mHGXdndDrag != NULL) {
      g_object_unref(mHGXdndDrag);
      mHGXdndDrag = NULL;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalXdndIsStaleDrag --
 *
 *      Whether `drag` is a GTK drag from an earlier host drag. A GTK drag can
 *      end or be cancelled well after the host has moved on, for example
 *      when a drop the target never accepted is only cancelled once the next
 *      drag begins. Its end says nothing about the host drag in progress,
 *      and resetting on it would release that drag's press.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::LocalXdndIsStaleDrag(GdkDrag *drag)   // IN
{
   if (!mInHGDrag || drag == mHGXdndDrag) {
      return false;
   }
   g_debug("%s: ignoring the end of a drag from an earlier host drag\n",
           __FUNCTION__);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalXdndRestoreDetWnd --
 *
 *      Turn the X pointer warp back on, which a guest to host drag needs, and
 *      restore normal stacking. A bridged XDND drag changes both. A drag can
 *      end without ResetUI(), on a host cancel or at the drop, so those paths
 *      call this too.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalXdndRestoreDetWnd(void)
{
   if (mDetWnd != NULL) {
      mDetWnd->SetWarpXPointer(true);
      mDetWnd->SetAbove(false);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnXdndBeginTimeout --
 *
 *      Wake the main loop while the pointer waits on the detection window,
 *      and stop holding it if GTK has not started the drag in time. See
 *      XDND_DRAG_BEGIN_TICK_MS.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DnDUIX11::OnXdndBeginTimeout(gpointer ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   if (ui->mHGXdndStep < XDND_DRAG_BEGIN_STEPS) {
      ui->mHGXdndStep++;
      ui->mDetWnd->SimulateXEvents(false, false, false, false, true,
                                   ui->mHGXdndPressX + 3 * ui->mHGXdndStep,
                                   ui->mHGXdndPressY);
      return G_SOURCE_CONTINUE;
   }
   gint64 now = g_get_monotonic_time();

   if (ui->mHGXdndLeaveAt != 0 ? now < ui->mHGXdndLeaveAt
                               : now < ui->mHGXdndBeginDeadline) {
      return G_SOURCE_CONTINUE;
   }
   ui->mHGXdndBeginTimer = 0;
   if (ui->mHGXdndLeaveAt == 0) {
      g_debug("%s: GTK did not start the drag in %ums\n", __FUNCTION__,
              (unsigned) ui->mXdndDragBeginTimeoutMs);
   }
   ui->LocalXdndLeaveDetWnd();

   return G_SOURCE_REMOVE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::StageFilePlaceholders --
 *
 *      Create an empty entry in the staging directory for every file the host
 *      is dragging.
 *
 *      A file manager stats the URIs it is offered while the pointer is still
 *      moving, to decide whether it can take the drop at all, and refuses
 *      what it cannot stat. Nothing is in the staging directory until the
 *      host has dropped, so without this the offer never becomes acceptable.
 *      The entries are only placeholders. AddBlockForDrop() adds the block at
 *      drop time, and the block keeps readers out until the host has filled
 *      the entries in.
 *
 * Results:
 *      True if the whole list was staged, in which case the caller owns the
 *      entries until the transfer finishes or the drag is cancelled.
 *
 * Side effects:
 *      Creates files and directories under mHGStagingDir.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::StageFilePlaceholders(const DnDFileList &fList)   // IN
{
   std::vector<std::string> relPaths = fList.GetRelPaths();
   std::vector<CPFileAttributes> attrs = fList.GetFileAttributes();
   utf::string root = mHGStagingDir;
   size_t i;

   if (   mBlockCtrl == NULL
       || !DnD_BlockIsReady(mBlockCtrl)
       || root.empty()
       || relPaths.empty()) {
      return false;
   }
   while (root.length() > 1 && root[root.length() - 1] == DIRSEPC) {
      root.erase(root.length() - 1);
   }

   for (i = 0; i < relPaths.size(); i++) {
      const std::string &rel = relPaths[i];
      bool isDir =    i < attrs.size()
                   && attrs[i].fileType == HGFS_FILE_TYPE_DIRECTORY;
      std::string full;
      char *parent;
      bool ok;

      /*
       * The list comes from the host. Each name becomes a path under the
       * staging directory and then an entry in a uri-list. The file contents
       * path uses the same check.
       */
      if (!LocalIsSafeStagingName(rel.c_str(), rel.size() + 1)) {
         g_debug("%s: refusing relative path [%s]\n", __FUNCTION__,
                 rel.c_str());
         goto error;
      }

      full = std::string(root.c_str()) + DIRSEPS + rel;
      parent = isDir ? g_strdup(full.c_str())
                     : g_path_get_dirname(full.c_str());
      ok = g_mkdir_with_parents(parent, 0755) == 0;
      g_free(parent);
      if (!ok) {
         g_debug("%s: cannot create directories for %s: %s\n", __FUNCTION__,
                 full.c_str(), g_strerror(errno));
         goto error;
      }
      if (!isDir) {
         int fd = g_open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

         if (fd < 0) {
            g_debug("%s: cannot create %s: %s\n", __FUNCTION__, full.c_str(),
                    g_strerror(errno));
            goto error;
         }
         close(fd);
      }
   }

   g_debug("%s: staged %" FMTSZ "u placeholders under %s\n", __FUNCTION__,
           (size_t)relPaths.size(), root.c_str());
   return true;

error:
   DnD_DeleteStagingFiles(mHGStagingDir.c_str(), FALSE);
   return false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::AddBlockForDrop --
 *
 *      Add the block now, just before the target reads.
 *
 *      The staged entries are placeholders until the host fills them in, so
 *      the block has to hold the target's read until OnGetFilesDone() reports
 *      that the bytes are there. It must not go in earlier. While the target
 *      is still deciding whether to take the drop, it has to be able to stat
 *      what it was offered.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::AddBlockForDrop(void)
{
   if (!mHGDeferBlock) {
      return;
   }
   AddBlock();
   /* From here on the host fills in the staged entries. */
   mHGDeferBlock = false;

   /*
    * With no block the target reads the placeholders as they are, and
    * copies a set of empty files as if they were the drop. Without them it
    * finds nothing, and fails the drop where the user can see it.
    */
   if (!mBlockAdded && !mHGStagingDir.empty()) {
      g_warning("%s: no block for %s; withdrawing the placeholders\n",
                __FUNCTION__, mHGStagingDir.c_str());
      DnD_DeleteStagingFiles(mHGStagingDir.c_str(), FALSE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::ClearFilePlaceholders --
 *
 *      Undo StageFilePlaceholders() where the host never sent the files.
 *
 *      Left behind they would look to a target like empty files, and they
 *      would also stop the staging directory from being reused.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::ClearFilePlaceholders(void)
{
   /*
    * The drop and OnGetFilesDone() clear the flag, so a set flag here means
    * the host sent nothing.
    */
   if (!mHGDeferBlock) {
      return;
   }
   mHGDeferBlock = false;
   if (mHGStagingDir.empty()) {
      return;
   }
   g_debug("%s: dropping the placeholders in %s\n", __FUNCTION__,
           mHGStagingDir.c_str());
   DnD_DeleteStagingFiles(mHGStagingDir.c_str(), FALSE);
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
      if (mBlockedPath == mHGStagingDir) {
         g_debug("%s: block already added\n", __FUNCTION__);
         return;
      }
      /*
       * A block still in place on another directory belongs to a drag that
       * is over, and mBlockAdded refers to that block. Treating it as this
       * drag's block would leave this directory unblocked and aim the later
       * removal at the wrong path.
       */
      g_debug("%s: releasing the block left on %s\n", __FUNCTION__,
              mBlockedPath.c_str());
      if (DnD_BlockIsReady(mBlockCtrl)) {
         mBlockCtrl->RemoveBlock(mBlockCtrl->fd, mBlockedPath.c_str());
      }
      mBlockAdded = false;
      mBlockedPath = "";
   }

   g_debug("%s: DnDBlockIsReady %d fd %d\n", __FUNCTION__,
           DnD_BlockIsReady(mBlockCtrl), mBlockCtrl->fd);

   if (   DnD_BlockIsReady(mBlockCtrl)
       && mBlockCtrl->AddBlock(mBlockCtrl->fd, mHGStagingDir.c_str())) {
      mBlockAdded = true;
      mBlockedPath = mHGStagingDir;
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
      /* mBlockedPath is where the block went in. mHGStagingDir may differ. */
      g_debug("%s: removing block for %s\n", __FUNCTION__, mBlockedPath.c_str());
      /* We need to make sure block subsystem has not been shut off. */
      if (DnD_BlockIsReady(mBlockCtrl)) {
         mBlockCtrl->RemoveBlock(mBlockCtrl->fd, mBlockedPath.c_str());
      }
      mBlockAdded = false;
      mBlockedPath = "";
   } else {
      g_debug("%s: not removing block mBlockAdded %d mHGGetFileStatus %d\n",
              __FUNCTION__, mBlockAdded, mHGGetFileStatus);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::AddDragItem --
 *
 *      Record one format of what the host is dragging.
 *
 * Side effects:
 *      mDragItems grows.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::AddDragItem(const char *mimeType,  // IN
                      const void *data,      // IN
                      size_t size)           // IN
{
   DragItem item;

   item.mimeType = mimeType;
   item.data.assign(static_cast<const char *>(data), size);
   mDragItems.push_back(item);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalSetContentProvider --
 *
 *      Collect what the host is dragging, once, for both drag sources.
 *
 *      The Wayland source offers the same MIME types and bytes as the Gtk
 *      one, and the source is chosen only when the drag starts. The formats
 *      are therefore collected here, and the provider is built from them.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      mDragItems and mProvider are replaced.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalSetContentProvider()
{
   std::vector<Glib::RefPtr<Gdk::ContentProvider>> providerVec;
   void *buf;
   size_t sz;

   g_debug("%s: enter.\n", __FUNCTION__);

   mDragItems.clear();

   if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_FILELIST) ||
       CPClipboard_ItemExists(&mClipboard, CPFORMAT_FILECONTENTS)) {
      /*
       * The caller has already framed the list for this desktop. Do not
       * offer an empty list. A target that asks for the type and gets
       * nothing sees a successful transfer of zero bytes, which GTK turns
       * into a NULL vector.
       */
      if (mHGDndUriList.bytes() > 0) {
         AddDragItem(DRAG_TARGET_NAME_URI_LIST, mHGDndUriList.c_str(),
                     mHGDndUriList.bytes());
      } else {
         g_debug("%s: the host's file list came out empty\n", __FUNCTION__);
      }
   } else if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_RTF)) {
      if (CPClipboard_GetItem(&mClipboard, CPFORMAT_RTF, &buf, &sz)) {
         g_debug("%s: RTF data, size %" FMTSZ "u.\n", __FUNCTION__, sz);
         AddDragItem(TARGET_NAME_APPLICATION_RTF, buf, sz);
         AddDragItem(TARGET_NAME_TEXT_RICHTEXT, buf, sz);
         AddDragItem(TARGET_NAME_TEXT_RTF, buf, sz);
      }
   } else if (CPClipboard_ItemExists(&mClipboard, CPFORMAT_TEXT)) {
      if (CPClipboard_GetItem(&mClipboard, CPFORMAT_TEXT, &buf, &sz)) {
         g_debug("%s: Text data, size %" FMTSZ "u.\n", __FUNCTION__, sz);
         AddDragItem(TARGET_NAME_TEXT_PLAIN, buf, sz);
         AddDragItem(TARGET_NAME_TEXT_PLAIN_UTF8, buf, sz);
      }
   }

   for (const DragItem &item : mDragItems) {
      providerVec.push_back(
         Gdk::ContentProvider::create(item.mimeType,
            Glib::Bytes::create(item.data.data(), item.data.size())));
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
      if (mBlockAdded || mHGDeferBlock) {
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

   /*
    * A native drag that the host has finished with can still be waiting for
    * the target or the compositor. While it runs, this drag's first motion
    * would fall back to XDND, and its end would arrive in the middle of this
    * drag.
    */
   LocalCancelWaylandDrag();

   /*
    * The host starts a drag into the guest only once any drag out of it is
    * over. The native drop target shares its overlays with the native drag
    * source, and would take this drag's press.
    */
   LocalHideNativeDropTarget(false);

   /*
    * A bridged XDND drop still waiting for its target is let go now, since
    * the host has moved on.
    */
   if (mHGXdndReleaseTimer != 0) {
      g_source_remove(mHGXdndReleaseTimer);
      mHGXdndReleaseTimer = 0;
      LocalXdndRelease();
   }
   LocalXdndForget();
   mHGXdndPressRounds = 0;

   /*
    * The compositor may have been slow to answer at login, or its
    * connection may have dropped since. WaylandDrag_Init() returns at once
    * outside Wayland and where the compositor lacks what a native drag needs.
    */
   if (!WaylandDrag_IsAvailable()) {
      WaylandDrag_Init();
   }
   LocalXdndChoosePath();

   /* The host's coordinates are for the screen as it is now. */
   mDetWnd->RefreshScreenSize();

   /*
    * An X pointer warp stops Mutter bridging the drag to Wayland clients;
    * see DragDetWnd::SimulateXEvents(). A bridged drag therefore moves the
    * pointer through uinput only. LocalXdndRestoreDetWnd() turns the warp
    * back on when the drag ends.
    */
   mDetWnd->SetWarpXPointer(!LocalMutterBridgesXdnd());

   /* Move detect window to the mouse in position */
   ShowDetWnd(true, mOriginX, mOriginY, DET_WND_DRAG_SOURCE);
   /*
    * Mutter keeps a window mapped without focus below the focused one, so the
    * press would land on whatever the user last clicked.
    * LocalXdndRestoreDetWnd() undoes this.
    */
   if (LocalMutterBridgesXdnd()) {
      mDetWnd->SetAbove(true);
   }

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
   /*
    * LocalSetContentProvider() replaces these only when the host sent a
    * format that is offered here. For a drag with none, both drag sources
    * would otherwise offer the previous drag's data.
    */
   mDragItems.clear();
   mProvider.reset();

   if (CPClipboard_GetItem(&mClipboard, CPFORMAT_FILELIST, &buf, &sz)) {
      mHGStagingDir = utf::string(stagingDir.c_str());
#ifdef DND_IS_XDG
      /* Until OnGetFilesDone(), the prune must leave the directory alone. */
      DnD_SetDragStagingDir(mHGStagingDir.c_str());
#endif

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
       * For both drag sources every name below goes into the uri-list with
       * only percent escaping, and resolves under the vmblock mount. A name
       * that leads out of the staging directory, or ends a uri-list entry
       * early, publishes a path where nothing was staged.
       * StageFilePlaceholders() checks again, and when it fails the drag
       * continues with an ordinary block.
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

      /*
       * A vmblock entry makes every lookup below the staging directory wait,
       * path resolution included, and a file manager such as Thunar stats
       * what it is offered while the pointer is still over it. A block added
       * here would stall the target, and the target would refuse the drop.
       * The files are therefore staged empty, and the block is added at the
       * drop, the first moment the bytes have to be right. This holds for
       * the native drag and for XDND alike.
       */
      {
         void *attrBuf;
         size_t attrSz;

         if (CPClipboard_GetItem(&mClipboard, CPFORMAT_FILEATTRIBUTES,
                                 &attrBuf, &attrSz)) {
            fList.AttributesFromCPClipboard(attrBuf, attrSz);
         }
         mHGDeferBlock = StageFilePlaceholders(fList);
      }
      if (!mHGDeferBlock) {
         AddBlock();
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
      LocalSetContentProvider();
   }

   SourceDragStartDone();
   /* Initialize host hide feedback to DROP_NONE. */
   mEffect = DROP_NONE;
   SourceUpdateFeedback(mEffect);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnWaylandDragDone --
 *
 *      The native drag ended, whatever the reason.
 *
 *      A Wayland drag emits none of the GtkDragSource signals, so when the
 *      drag ends without a host drop, only this function releases the faked
 *      press. Without it the uinput button stays down for the rest of the
 *      session and the desktop looks frozen.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnWaylandDragDone(void *ctx)   // IN: a WaylandDragTicket
{
   WaylandDragTicket *ticket = (WaylandDragTicket *)ctx;
   DnDUIX11 *this_instance = ticket->ui;
   bool current = ticket->gen == this_instance->mWaylandDragGen;

   TRACE_CALL();
   delete ticket;
   /*
    * The host returns to READY without waiting for the compositor, on a
    * cancel, on GET_FILES_DONE and on a drop of anything but files. The next
    * drag can be running by the time this drag's end arrives, and a reset
    * then would release the new drag's button in the middle of it.
    */
   if (!current) {
      g_debug("%s: the end of a drag already given up\n", __FUNCTION__);
      return;
   }
   this_instance->mWaylandDragActive = false;
   if (this_instance->mHGGetFileStatus != DND_FILE_TRANSFER_IN_PROGRESS) {
      this_instance->ResetUI();
   } else if (this_instance->mHGButtonDown) {
      /*
       * A file drop keeps its staging state until the transfer is done. The
       * drag itself is over, and the host drop that would release the
       * button may never come.
       */
      g_debug("%s: releasing the faked button\n", __FUNCTION__);
#ifdef USE_UINPUT
      FakeMouse_Click(false);
#endif
      this_instance->mHGButtonDown = false;
   }
   this_instance->mInHGDrag = false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalStartWaylandDrag --
 *
 *      Start the host to guest drag as a native Wayland drag.
 *
 * Results:
 *      True if the compositor took the drag. The button then stays down
 *      until OnSrcDrop() or OnWaylandDragDone() releases it. False leaves no
 *      native drag behind and the button up, and the caller falls back to
 *      XDND.
 *
 * Side effects:
 *      Fakes a pointer press.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::LocalStartWaylandDrag(int32 x,   // IN: pointer x-coord
                                int32 y)   // IN: pointer y-coord
{
#ifdef USE_UINPUT
   std::vector<WaylandDragItem> items;

   if (!WaylandDrag_IsAvailable() || !FakeMouse_IsInit() ||
       mDragItems.empty()) {
      return false;
   }

   for (const DragItem &item : mDragItems) {
      WaylandDragItem wItem;

      wItem.mimeType = item.mimeType.c_str();
      wItem.data = item.data.data();
      wItem.size = item.data.size();
      items.push_back(wItem);
   }

   if (!WaylandDrag_Arm(&items[0], items.size())) {
      return false;
   }

   /*
    * The detection window has no part in a native drag, and under Xwayland
    * the compositor treats it like any other window. Left mapped at the
    * corner of the work area, it can be offered the drop itself, and the
    * drop is lost.
    */
   ShowDetWnd(false, 0, 0, DET_WND_DRAG_SOURCE);

   /*
    * Use uinput directly rather than SimulateXEvents(), which also shows,
    * moves and raises the X11 detection window and warps the X pointer. That
    * all serves XDND. Raising an Xwayland window under the cursor would also
    * take the press away from the overlay.
    *
    * FakeMouse_Button() skips the pause that FakeMouse_Click() takes after
    * each button event for GTK's drag detection.
    * WaylandDrag_StartOnPress() below waits for the press on the Wayland
    * connection instead. The host's RPC messages wait behind any sleep here.
    * If the user lets go of a quick drag during the sleep, the host cancels
    * the drag before the guest drag has started.
    */
   FakeMouse_Button(false);
   FakeMouse_Move(x, y);
   FakeMouse_Move(x + 1, y + 1);
   FakeMouse_Button(true);

   WaylandDragTicket *ticket = new WaylandDragTicket;

   ticket->ui = this;
   ticket->gen = ++mWaylandDragGen;
   if (!WaylandDrag_StartOnPress(WAYLAND_DRAG_PRESS_TIMEOUT_MS,
                                 OnWaylandDragDone, ticket)) {
      /* done() is never called for a drag that did not start. */
      delete ticket;
      WaylandDrag_Disarm();
      /*
       * The press went to some other surface. Let go of it, and map the
       * detection window again for the XDND drag the caller falls back to.
       * The window manager adopts the window some time after the map, and a
       * press before that lands on whatever is under the pointer.
       */
      FakeMouse_Button(false);
      ShowDetWnd(true, mOriginX, mOriginY, DET_WND_PRESS_TARGET);
      return false;
   }

   mWaylandDragActive = true;
   g_debug("%s: dragging %" FMTSZ "u formats natively\n", __FUNCTION__,
           (size_t)items.size());

   return true;
#else
   return false;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalCancelWaylandDrag --
 *
 *      End the native drag, if there is one, and forget it.
 *
 * Side effects:
 *      Any later done(), including the one this causes, is stale and
 *      ignored, so the caller does its own cleanup.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalCancelWaylandDrag(void)
{
   mWaylandDragGen++;
   mWaylandDragActive = false;
   WaylandDrag_Cancel();
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
   mMousePosX = x;
   mMousePosY = y;
#ifdef USE_UINPUT
   /*
    * The host moves the pointer during a guest to host drag only once the
    * drag has come back into the guest. Let the drag through the native
    * drop target to the windows under it. OnDestMoveDetWndToMousePos()
    * gives the pointer back to the drop target if the drag leaves again.
    */
   if (mGHNative && !mInHGDrag) {
      WaylandDrag_DropTargetSetInput(FALSE);
   }
#endif
   if (mHGXdndWaitBegin) {
      /* LocalXdndLeaveDetWnd() moves the pointer here once GTK is ready. */
      return;
   }
   // Position the pointer.
   mDetWnd->SimulateXEvents(false, false, false, false, true, x, y);
#ifdef USE_UINPUT
   if (mWaylandDragActive) {
      /* Serve the target between host motions. See WaylandDrag_Pump(). */
      WaylandDrag_Pump();
   }
#endif

   if (mInHGDrag) {
      /*
       * Send feedback to the host before starting the drag. The host
       * chooses between a drop and a cancel from the feedback it has when
       * the user lets go. Starting the drag maps the overlay and waits for
       * the press, and the user can let go of a quick drag in that time.
       * With no feedback yet, the host cancels. With feedback, the host's
       * drop waits in the RPC queue until the drag below has started.
       */
      if (mEffect != DROP_MOVE) {
         mEffect = DROP_MOVE;
         g_debug("%s: Updating feedback\n", __FUNCTION__);
         SourceUpdateFeedback(mEffect);
      }
      if (!mHGDragStarted) {
         /*
          * Both drag sources hold the button down from here on, and it has
          * to be released however the drag ends.
          */
         mHGButtonDown = true;

         /*
          * A native Wayland drag reaches Wayland and X11 targets alike. An
          * XDND drag from the Xwayland detection window reaches Wayland
          * targets only where the compositor bridges it. wlroots does not, so
          * under sway every drop onto a Wayland application is lost. Use the
          * native source where there is one, and XDND everywhere else.
          */
         if (!LocalStartWaylandDrag(x, y)) {
            /*
             * We want to simulate the mouse press and hold to trigger
             * drag_begin event for GtkDragSource, do it here rather than
             * SrcDragBegin as we want to get the mouse position.
             *
             * Before the DnD, we should make sure that the mouse is released
             * otherwise it may be another DnD, not ours. Send a release, then
             * a press here to cover this case.
             */
            int detX = 0, detY = 0, detW = 0, detH = 0;
            int pressX = x, pressY = y;
            bool bridged = LocalMutterBridgesXdnd();

            if (bridged) {
               /*
                * Where the window was asked to be, in case its geometry is
                * not known yet. The window is not moved for the press here,
                * so the geometry below is where the compositor put it.
                */
               pressX = mOriginX + DRAG_DET_WINDOW_WIDTH;
               pressY = mOriginY + DRAG_DET_WINDOW_WIDTH;
            }

            /*
             * GTK starts the drag from a press followed by motion on the
             * detection window itself, so the press has to land on it. A
             * compositor may put the window somewhere other than the requested
             * position, and a tiling one does, so press where the window is.
             */
            mDetWnd->GetGeometry(detX, detY, detW, detH);
            if (detW > 0 && detH > 0) {
               pressX = detX + detW / 2;
               pressY = detY + detH / 2;
               if (pressX != x || pressY != y) {
                  g_debug("%s: pressing on the detection window at (%d, %d), "
                          "not the pointer at (%d, %d)\n", __FUNCTION__,
                          pressX, pressY, x, y);
               }
            }
            if (bridged) {
               /*
                * Press only where X has the pointer over the window. The
                * window manager can still be placing the window when it is
                * first shown, so alternate between where the window reports
                * it is and the middle of where it was asked to be. A press
                * anywhere else lands on the window underneath and starts
                * moving it.
                */
               bool over = false;

               for (int i = 0; i < XDND_PRESS_TRIES && !over; i++) {
                  int tx = mOriginX + DRAG_DET_WINDOW_WIDTH;
                  int ty = mOriginY + DRAG_DET_WINDOW_WIDTH;

                  mDetWnd->GetGeometry(detX, detY, detW, detH);
                  if (i % 2 == 0 && detW > 0 && detH > 0) {
                     tx = detX + detW / 2;
                     ty = detY + detH / 2;
                  }
                  mDetWnd->SimulateXEvents(false, false, false, false, true,
                                           tx, ty);
                  /* SimulateXEvents() ends a pixel down and right. */
                  over = mDetWnd->PointerIsOver(tx + 1, ty + 1);
                  if (over) {
                     pressX = tx;
                     pressY = ty;
                  } else {
                     g_usleep(XDND_PRESS_RETRY_MS * 1000);
                  }
               }
               if (!over) {
                  /*
                   * Try again on the host's next motion, a few times, and
                   * otherwise let the drag fail without pressing at all.
                   * Nothing has been pressed yet.
                   */
                  mHGButtonDown = false;
                  mHGXdndPressRounds++;
                  g_debug("%s: the pointer never reached the detection "
                          "window (round %d)\n", __FUNCTION__,
                          mHGXdndPressRounds);
                  if (mHGXdndPressRounds >= XDND_PRESS_ROUNDS) {
                     mHGDragStarted = true;
                  }
                  return;
               }
            }
            mDetWnd->SimulateXEvents(true, true, false, !bridged, true,
                                     pressX, pressY);
            // simulate drag mouse press
            mDetWnd->SimulateXEvents(false, true, true, false, true, pressX, pressY);
            // Tell Gtk that a drag should be started from this widget.
            mDetWnd->StartDrag(mProvider, pressX, pressY);
            if (bridged) {
               /*
                * The motion that makes GTK start the drag comes from
                * OnXdndBeginTimeout(), one small step per tick, and the
                * pointer stays on the window until GTK has started it.
                */
               mHGXdndPressX = pressX;
               mHGXdndPressY = pressY;
               mHGXdndStep = 0;
               mHGXdndLeaveAt = 0;
               mHGXdndWaitBegin = true;
               mHGXdndBeginDeadline = g_get_monotonic_time() +
                                      (gint64) mXdndDragBeginTimeoutMs * 1000;
               mHGXdndBeginTimer = g_timeout_add(XDND_DRAG_BEGIN_TICK_MS,
                                                 OnXdndBeginTimeout, this);
               mHGDragStarted = true;
               return;
            }

            /*
             * Put the pointer back where the host has it. The press had to
             * land on the detection window, but the drag follows the pointer
             * from here. Waiting for the next motion from the host leaves the
             * XDND handshake less time than it needs before the host drops.
             */
            if (pressX != x || pressY != y) {
               mDetWnd->SimulateXEvents(false, false, false, false, true, x, y);
            }
         }
         mHGDragStarted = true;
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
    * A native drag would take the release below as its drop, onto whatever
    * is under the pointer.
    */
   LocalCancelWaylandDrag();
   LocalXdndForget();
   /*
    * Force the window to show, position the mouse over it, and release.
    * Seems like moving the window to 0, 0 eliminates frequently observed
    * flybacks when we cancel as user moves mouse in and out of destination
    * window in a H->G DnD.
    */
   ShowDetWnd(true, mOriginX, mOriginY, DET_WND_DRAG_SOURCE);
   mDetWnd->SimulateXEvents(true, true, false, true, true,
                            mOriginX + DRAG_DET_WINDOW_WIDTH / 2,
                            mOriginY + DRAG_DET_WINDOW_WIDTH / 2);
   ShowDetWnd(false, 0, 0, DET_WND_DRAG_SOURCE);
   mDetWnd->SimulateXEvents(false, false, false, false, true,
                           mMousePosX, mMousePosY);
   LocalXdndRestoreDetWnd();
   /* The real release has been sent; ResetUI() need not send another. */
   mHGButtonDown = false;
   mInHGDrag = false;
   mHGGetFileStatus = DND_FILE_TRANSFER_NOT_STARTED;
   mEffect = DROP_NONE;
   ClearFilePlaceholders();
   RemoveBlock();
}


/* Source functions for GH DnD. */


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalShowNativeDropTarget --
 *
 *      Show the native Wayland drop target in place of the detection window
 *      for a guest to host drag, and move the pointer onto it where the host
 *      says the pointer left the guest.
 *
 *      The detection window receives a Wayland drag only if the compositor
 *      carries the drag into Xwayland. niri, through xwayland-satellite,
 *      does not, and it scrolls its view to the window as well. The native
 *      drop target needs no such help, and exists where the native drag
 *      source does, which is where the compositor implements wlr-layer-shell.
 *      Mutter does not, and there the detection window stays.
 *
 * Results:
 *      True if the native drop target is shown. False leaves the pointer
 *      alone, and the caller shows the detection window.
 *
 * Side effects:
 *      Moves the pointer.
 *
 *-----------------------------------------------------------------------------
 */

bool
DnDUIX11::LocalShowNativeDropTarget(int32 x,   // IN: pointer x-coord
                                    int32 y)   // IN: pointer y-coord
{
#ifdef USE_UINPUT
   /*
    * A lost compositor connection takes the drop target with it, and the
    * native branches elsewhere must not run for a detection window drag.
    */
   if (mGHNative && !WaylandDrag_DropTargetShown()) {
      mGHNative = false;
      mGHNativeLinger = false;
      mGHNativeHostTold = false;
   }
   /* The connection may have dropped since Init(). See OnSrcDragBegin(). */
   if (!WaylandDrag_IsAvailable()) {
      WaylandDrag_Init();
   }
   if (!LocalWaylandDragPreferred() || mWaylandDragActive) {
      return false;
   }
   if (!WaylandDrag_ShowDropTarget(OnNativeDropData, OnNativeDrop,
                                   OnNativeDropLeave, this)) {
      g_debug("%s: no native drop target; using the detection window\n",
              __FUNCTION__);
      return false;
   }
   mGHNative = true;
   mGHNativeLinger = false;
   mGHNativeHostTold = false;
   /*
    * The drop target may still hold the drag from an earlier exit, and the
    * compositor sends no new enter for it. Hand its contents over again,
    * from the main loop, once the common layer has taken the new session.
    */
   mGHNativeResend = WaylandDrag_DropTargetHasData();
   /* The detection window takes no part, so neither does Mutter's bridge. */
   mMutterBridgesXdnd = false;
   g_debug("%s: native drop target for a guest to host drag at (%d, %d)\n",
           __FUNCTION__, x, y);
   mGHNativeExitX = x;
   mGHNativeExitY = y;
   /*
    * Move the pointer from the main loop. The host also asks for the
    * detection window, at (0, 0), when a host to guest drag begins, and
    * OnSrcDragBegin() hides the drop target again before control returns
    * to the main loop, which cancels this. Moving now would take the
    * pointer to the top-left corner, where niri opens its overview.
    */
   if (mGHNativeNudgeTimer != 0) {
      g_source_remove(mGHNativeNudgeTimer);
   }
   mGHNativeNudgeTimer = g_idle_add(OnNativeDropShown, this);
   return true;
#else
   return false;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalHideNativeDropTarget --
 *
 *      Hide the native drop target, if it is shown. A drop check still
 *      pending runs first, because it needs to know whether the drag holds
 *      the drop target.
 *
 *      With keepHeldDrag, a drop target that still holds a guest drag the
 *      host was told about, and that has not had the drop, stays until the
 *      drag drops on it or leaves it. Any other drag is the host's no
 *      concern, and goes on to the windows under the drop target. This is for a drag that outlived the drop on the host and that
 *      Escape did not cancel. wlroots, for one, sends no key to anyone
 *      during a drag. The host keeps its mouse's button down until the
 *      pointer next moves over the guest, and then lets go. Were the drop
 *      target gone by then, the drag would drop on whatever window is under
 *      the pointer. The drop target covers every output, so the drag drops
 *      on it instead, and OnNativeDrop() hides it.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalHideNativeDropTarget(bool keepHeldDrag)   // IN
{
   if (!mGHNative) {
      return;
   }
   if (mGHNativeNudgeTimer != 0) {
      g_source_remove(mGHNativeNudgeTimer);
      mGHNativeNudgeTimer = 0;
   }
   if (mGHDropCheckTimer != 0) {
      g_source_remove(mGHDropCheckTimer);
      mGHDropCheckTimer = 0;
      OnGHDropCheck(this);
   }
   if (keepHeldDrag && mGHNativeHostTold && WaylandDrag_DropTargetHasData()) {
      if (!mGHNativeLinger) {
         g_debug("%s: the guest drag still holds the native drop target; "
                 "keeping it until the drag ends\n", __FUNCTION__);
      }
      mGHNativeLinger = true;
      return;
   }
   WaylandDrag_HideDropTarget();
   mGHNative = false;
   mGHNativeLinger = false;
   mGHNativeHostTold = false;
   mGHNativeResend = false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalNudgeNativeDropTarget --
 *
 *      Move the pointer to (x, y) through uinput, and again a few times from
 *      the main loop until the drag has entered the native drop target.
 *
 *      The compositor offers the drag to the surface under the pointer only
 *      on a motion. The pointer may already be at (x, y), and the kernel
 *      drops an absolute move to where the device already is, so each move
 *      goes a pixel away and back.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalNudgeNativeDropTarget(int32 x,   // IN: pointer x-coord
                                     int32 y)   // IN: pointer y-coord
{
#ifdef USE_UINPUT
   /* The host's coordinates are for the screen as it is now. */
   mDetWnd->RefreshScreenSize();
   mGHNativeX = x;
   mGHNativeY = y;
   FakeMouse_Move(x > 0 ? x - 1 : x + 1, y);
   FakeMouse_Move(x, y);
   if (mGHNativeNudgeTimer != 0) {
      g_source_remove(mGHNativeNudgeTimer);
   }
   mGHNativeNudgesLeft = GH_NATIVE_NUDGES;
   mGHNativeNudgeTimer = g_timeout_add(GH_NATIVE_NUDGE_MS, OnNativeDropNudge,
                                       this);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnNativeDropShown --
 *
 *      The first move after LocalShowNativeDropTarget() maps the drop
 *      target. LocalHideNativeDropTarget() removes it when the drop target
 *      goes before this runs.
 *
 * Results:
 *      G_SOURCE_REMOVE.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DnDUIX11::OnNativeDropShown(gpointer ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   ui->mGHNativeNudgeTimer = 0;
   if (ui->mGHNative) {
      g_debug("%s: moving the pointer onto the native drop target\n",
              __FUNCTION__);
      ui->LocalNudgeNativeDropTarget(ui->mGHNativeExitX, ui->mGHNativeExitY);
   }
   return G_SOURCE_REMOVE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnNativeDropNudge --
 *
 *      One of the moves LocalNudgeNativeDropTarget() schedules. The first
 *      one also hands over again the contents of a drag the drop target
 *      already held when it was shown. See LocalShowNativeDropTarget().
 *
 * Results:
 *      G_SOURCE_CONTINUE while moves are left and the drag has not entered
 *      the native drop target, else G_SOURCE_REMOVE.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DnDUIX11::OnNativeDropNudge(gpointer ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   if (ui->mGHNative && ui->mGHNativeResend) {
      ui->mGHNativeResend = false;
      WaylandDrag_DropTargetResend();
   }
   if (!ui->mGHNative || WaylandDrag_DropTargetHasDrag()) {
      ui->mGHNativeNudgeTimer = 0;
      return G_SOURCE_REMOVE;
   }
   if (ui->mGHNativeNudgesLeft <= 0) {
      ui->mGHNativeNudgeTimer = 0;
      /*
       * Neither a drag nor the pointer reached the drop target, so the
       * compositor does not route input to it. Fall back to the detection
       * window, whose drag the compositor bridges from Wayland to X. The
       * pointer without a drag counts as reached. The host asks whenever the
       * pointer leaves the guest, and that must not map the X window.
       */
      if (!WaylandDrag_DropTargetReached()) {
         g_debug("%s: nothing reached the native drop target; using the "
                 "detection window\n", __FUNCTION__);
         ui->LocalHideNativeDropTarget(false);
         ui->mGHNativeFallback = true;
         ui->ShowDetWnd(true, ui->mGHNativeExitX, ui->mGHNativeExitY,
                        DET_WND_DROP_TARGET);
      }
      return G_SOURCE_REMOVE;
   }
   ui->mGHNativeNudgesLeft--;
#ifdef USE_UINPUT
   FakeMouse_Move(ui->mGHNativeX > 0 ? ui->mGHNativeX - 1 : ui->mGHNativeX + 1,
                  ui->mGHNativeY);
   FakeMouse_Move(ui->mGHNativeX, ui->mGHNativeY);
#endif
   return G_SOURCE_CONTINUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnNativeDropData --
 *
 *      The native drop target has read what the guest is dragging. Hand it
 *      to OnGHDropRead(), as the GTK drop target does.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnNativeDropData(const char *mimeType,   // IN
                           const char *data,       // IN: NUL-terminated
                           size_t size,            // IN
                           void *ctx)              // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);
   /* Every other type the drop target reads is text. */
   const char *type = strcmp(mimeType, DRAG_TARGET_NAME_URI_LIST) == 0
                      ? DRAG_TARGET_NAME_URI_LIST : TARGET_NAME_STRING;

   g_debug("%s: the native drop target read %" FMTSZ "u bytes of %s\n",
           __FUNCTION__, size, mimeType);
   ui->mGHNativeHostTold = false;
   if (!ui->mDnD->IsDnDAllowed()) {
      g_warning("%s: No dnd allowed!\n", __FUNCTION__);
   } else if (ui->mInHGDrag) {
      g_warning("%s: In H->G dnd process rather than G->H, exit\n",
                __FUNCTION__);
   } else {
      ui->LocalBeginGHDrag();
      ui->OnGHDropRead(data, type);
      ui->mGHNativeHostTold = ui->mGHDnDDataReceived;
   }

   /*
    * The host has not heard of this drag, so the drop target must not keep
    * it. Let it through to the windows under the drop target, as it would
    * go past the detection window.
    */
   if (!ui->mGHNativeHostTold) {
      g_debug("%s: the host was not told of the drag; letting it through\n",
              __FUNCTION__);
      WaylandDrag_DropTargetSetInput(FALSE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnNativeDrop, DnDUIX11::OnNativeDropLeave --
 *
 *      The guest's drag dropped on the native drop target, or left it. A
 *      drag that leaves ends the guest to host drag, as OnGtkDragLeave()
 *      does for the detection window. Either one ends a drop target kept
 *      for a drag that outlived the host's drop.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::OnNativeDrop(void *ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   g_debug("%s: the guest drag dropped on the native drop target\n",
           __FUNCTION__);
   /* See LocalHideNativeDropTarget(). */
   if (ui->mGHNativeLinger) {
      ui->LocalHideNativeDropTarget(false);
   }
}


void
DnDUIX11::OnNativeDropLeave(void *ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   g_debug("%s: the guest drag left the native drop target\n", __FUNCTION__);
   ui->mGHNativeHostTold = false;
   if (ui->mGHDnDInProgress) {
      ui->ResetUI();
   }
   /* See LocalHideNativeDropTarget(). */
   if (ui->mGHNativeLinger) {
      ui->LocalHideNativeDropTarget(false);
   }
}


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

   if (mGHNative) {
#ifdef USE_UINPUT
      /*
       * Release where the pointer is. SimulateXEvents() would first move the
       * pointer to where the X server last saw it, which is stale while the
       * pointer is over a Wayland surface.
       */
      FakeMouse_Button(false);
#endif
   } else if (mGHDnDInProgress) {

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

   if (mGHNative) {
#ifdef USE_UINPUT
      if (mGHDnDInProgress || WaylandDrag_DropTargetHasDrag()) {
         /*
          * The pointer is on the native drop target, where
          * LocalShowNativeDropTarget() put it, so release there. The
          * compositor then drops the drag on the drop target, which
          * finishes the drop. OnGHDropCheck() cancels a drag that is still
          * running when it looks.
          */
         g_debug("%s: releasing the button over the native drop target\n",
                 __FUNCTION__);
         FakeMouse_Button(false);
         if (mGHDropCheckTimer != 0) {
            g_source_remove(mGHDropCheckTimer);
         }
         mGHDropCheckTimer = g_timeout_add(GH_DROP_CHECK_MS, OnGHDropCheck,
                                           this);
      }
#endif
   } else if (mGHDnDInProgress) {
      Glib::RefPtr<Gdk::Display> display = Gdk::Display::get_default();
      GdkSurface *gdkSurface = gdk_x11_display_get_default_group(display->gobj());
      UpdateWorkArea();

     /*
       * Show the window, move it to the mouse position, and release the
       * mouse button.
       */
      mDetWnd->SimulateXEvents(true, true, false, true, false,
                               mOriginX, mOriginY);
#ifdef USE_UINPUT
      /*
       * See OnGHDropCheck(). Only GNOME has been seen to keep the guest's
       * drag alive after the drop, so the check runs only under Mutter.
       */
      if (FakeMouse_IsInit() && LocalMutterBridgesXdnd()) {
         if (mGHDropCheckTimer != 0) {
            g_source_remove(mGHDropCheckTimer);
         }
         mGHDropCheckTimer = g_timeout_add(GH_DROP_CHECK_MS, OnGHDropCheck,
                                           this);
      }
#endif
   }
   mDestDropTime = GetTimeInMillis();
   ResetUI();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnGHDropCheck --
 *
 *      Cancel the guest's drag if the release in OnDestCancel() did not end
 *      it.
 *
 *      The drag started with the mouse the host drives, and the uinput
 *      device never pressed. The kernel drops a release from a device that
 *      never pressed, so the release faked through uinput reaches no
 *      compositor. On GNOME the host also keeps its mouse's button down after
 *      the user drops on the host, until the pointer next moves over the
 *      guest. Left alone, the drag drops wherever the pointer is when the
 *      host lets go. Escape cancels it. While the host holds the button,
 *      Mutter still keeps the pointer on the window the drag started in, so
 *      a host to guest drag cannot start until then either way.
 *
 *      A drag that has ended has either dropped on the detection window or
 *      left it. After a drop GTK keeps the drop while it reads the value, so
 *      the drop is checked for as well. A drag that still offers a drop to
 *      the window, and has not dropped, is still running, and Escape then
 *      goes to the drag rather than to an application.
 *
 *      The native drop target knows whether wl_data_device.drop arrived, so
 *      with it the check needs no guesswork. A drag that still holds the
 *      drop target and has not dropped is still running.
 *
 * Results:
 *      G_SOURCE_REMOVE.
 *
 * Side effects:
 *      May press Escape.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DnDUIX11::OnGHDropCheck(gpointer ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   ui->mGHDropCheckTimer = 0;
#ifdef USE_UINPUT
   if (ui->mGHNative) {
      if (!ui->mInHGDrag && !ui->mHGButtonDown &&
          WaylandDrag_DropTargetHasDrag() && !WaylandDrag_DropTargetDropped()) {
         g_debug("%s: no drop reached the native drop target; cancelling "
                 "the guest drag\n", __FUNCTION__);
         FakeMouse_Escape();
      } else {
         g_debug("%s: the guest drag %s\n", __FUNCTION__,
                 WaylandDrag_DropTargetDropped()
                    ? "dropped on the native drop target"
                    : "no longer holds the native drop target");
      }
      return G_SOURCE_REMOVE;
   }
   /* A host to guest drag may have started since, over its own window. */
   if (ui->mDetWnd != NULL && !ui->mInHGDrag && !ui->mHGButtonDown &&
       ui->mDetWnd->Get_Current_Drop_ctx() != 0 &&
       !ui->mDetWnd->XdndDropped()) {
      g_debug("%s: the guest drag outlived the drop; cancelling it\n",
              __FUNCTION__);
      FakeMouse_Escape();
   }
#endif
   return G_SOURCE_REMOVE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::NudgeWaylandDrag --
 *
 *      Move the pointer one pixel and back. The compositor and the target
 *      under the drag check the drag again only on pointer motion.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::NudgeWaylandDrag(void *ctx)   // IN: the DnDUIX11
{
#ifdef USE_UINPUT
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);

   FakeMouse_Move(ui->mMousePosX > 0 ? ui->mMousePosX - 1 : ui->mMousePosX + 1,
                  ui->mMousePosY);
   FakeMouse_Move(ui->mMousePosX, ui->mMousePosY);
#endif
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
#ifdef USE_UINPUT
   if (mWaylandDragActive) {
      /*
       * Showing the detection window maps and raises an Xwayland window, and
       * SimulateXEvents() also warps the X pointer. Either one loses a native
       * drag. Even after the target has accepted, the compositor then
       * cancels the drag instead of dropping. Leave the detection
       * window and the X pointer alone, and use uinput and the Wayland
       * connection only.
       */
      FakeMouse_Move(mMousePosX, mMousePosY);

      /*
       * The compositor decides the drop on the target's last answer. The
       * host moves the pointer faster than a file manager answers, so when
       * the host lets go, the last answer is often a refusal for an earlier
       * motion. Give the target time to catch up before releasing the
       * button.
       *
       * AddBlockForDrop() adds the block only after the wait. To decide, the
       * target stats the files it was offered, and with a block in place that
       * stat does not return. When the placeholders could not be staged,
       * OnSrcDragBegin() added the block already, and this wait runs out.
       */
      if (!WaylandDrag_WaitForAccept(mWaylandDragAcceptTimeoutMs)) {
         /*
          * A file manager fetches the offer's contents and decides only on a
          * motion that comes after they arrive. When the pointer has already
          * stopped, no such motion comes, so move the pointer one pixel and
          * back.
          */
         g_debug("%s: nudging the pointer for an answer\n", __FUNCTION__);
         NudgeWaylandDrag(this);
         if (!WaylandDrag_WaitForAccept(mWaylandDragAcceptTimeoutMs)) {
            /*
             * A target that still refuses may be judging this drag on the
             * contents of an earlier one, as WaylandDrag_Refocus()
             * describes. Make it see the drag leave and come back.
             */
            g_debug("%s: taking the drag off the target and back\n",
                    __FUNCTION__);
            WaylandDrag_Refocus(mWaylandDragAcceptTimeoutMs,
                                NudgeWaylandDrag, this);
         }
      }
      AddBlockForDrop();
      FakeMouse_Click(false);
      mHGButtonDown = false;
      mInHGDrag = false;
      return;
   }
#endif
   /*
    * A drag that GTK never started, or has already ended, has no target to
    * wait for. Waiting would only pull the pointer back to the drop point
    * on every nudge.
    */
   if (   LocalMutterBridgesXdnd()
       && mInHGDrag
       && mHGButtonDown
       && (mHGXdndDrag != NULL || mHGXdndWaitBegin)) {
      /*
       * The drop is decided on the target's last XdndStatus, which GTK only
       * sees when the main loop runs, and the host lets go as soon as the
       * user does, often before the target has answered. Release from the
       * main loop once the target has accepted. See OnXdndReleaseTick().
       */
      mHGXdndDropAt = g_get_monotonic_time();
      mHGXdndReleaseTicks = 0;
      if (mHGXdndReleaseTimer == 0) {
         mHGXdndReleaseTimer = g_timeout_add(XDND_DRAG_BEGIN_TICK_MS,
                                             OnXdndReleaseTick, this);
      }
      return;
   }
   LocalXdndRelease();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::LocalXdndRelease --
 *
 *      Release the button that has held the XDND drag.
 *
 * Side effects:
 *      Adds the deferred vmblock block, if any, shows and hides the detection
 *      window, and fakes the release.
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalXdndRelease(void)
{
   AddBlockForDrop();
   ShowDetWnd(true, mOriginX, mOriginY, DET_WND_DRAG_SOURCE);

   /*
    * Move the mouse to the saved coordinates, and release the mouse button.
    */
   mDetWnd->SimulateXEvents(false, true, false, false, true,
                            mMousePosX, mMousePosY);
   /* The real release has been sent; ResetUI() need not send another. */
   mHGButtonDown = false;
   ShowDetWnd(false, 0, 0, DET_WND_DRAG_SOURCE);
   /* We've successful or failure for HG drag, set the flag to false anyway */
   mInHGDrag = false;
   /* After the release. A warp with the button still down stops the bridge. */
   LocalXdndRestoreDetWnd();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnXdndReleaseTick --
 *
 *      Release a bridged XDND drop once the target has accepted it, or when
 *      the time is up. While it has not, move the pointer a pixel and back
 *      now and then, since a target only answers motion.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DnDUIX11::OnXdndReleaseTick(gpointer ctx)   // IN: the DnDUIX11
{
   DnDUIX11 *ui = static_cast<DnDUIX11 *>(ctx);
   gint64 now = g_get_monotonic_time();
   bool accepted;

   /* The pointer has to leave the detection window before anything else. */
   if (ui->mHGXdndWaitBegin) {
      if (now - ui->mHGXdndDropAt < (gint64) XDND_DROP_WAIT_LEAVE_MS * 1000) {
         return G_SOURCE_CONTINUE;
      }
      ui->LocalXdndLeaveDetWnd();
   }
   accepted = ui->mHGXdndDrag != NULL &&
              gdk_drag_get_selected_action(ui->mHGXdndDrag) != 0 &&
              now - ui->mHGXdndLeftAt >= (gint64) XDND_DROP_SETTLE_MS * 1000;
   ui->mHGXdndReleaseTicks++;
   if (!accepted &&
       now - MAX(ui->mHGXdndLeftAt, ui->mHGXdndDropAt) <
          (gint64) ui->mXdndDropAcceptTimeoutMs * 1000) {
      if (ui->mHGXdndReleaseTicks % XDND_DROP_NUDGE_TICKS == 0) {
         /* SimulateXEvents() moves to the point and a pixel beyond it. */
         ui->mDetWnd->SimulateXEvents(false, false, false, false, true,
                                      ui->mMousePosX, ui->mMousePosY);
      }
      return G_SOURCE_CONTINUE;
   }
   g_debug("%s: releasing, target %s\n", __FUNCTION__,
           accepted ? "accepted" : "never accepted");
   ui->mHGXdndReleaseTimer = 0;
   ui->LocalXdndRelease();

   return G_SOURCE_REMOVE;
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
#ifdef DND_IS_XDG
   /* The files are written. Open files and the age rule protect them now. */
   DnD_SetDragStagingDir(NULL);
#endif

   /*
    * The host decides when this arrives, and the RPC layer does not check
    * that a transfer was requested, so it can arrive before the drop at
    * which OnSrcDrop() adds the block. The RemoveBlock() below then does
    * nothing, and the drop would add a block that nothing removes. The
    * application that took the drop would then wait in open() forever.
    * Clear mHGDeferBlock so that the drop adds no block. The uri-list still
    * names the files through the vmblock mount, which passes lookups
    * straight through to the files the host has written when the staging
    * directory has no block.
    *
    * Only mHGDeferBlock says that this drag's block is still to come.
    * mBlockAdded can be left over from another drag, and checking it here
    * would let the drop add the block anyway.
    */
   if (mHGDeferBlock) {
      g_debug("%s: files done before the drop; not blocking\n", __FUNCTION__);
      mHGDeferBlock = false;
   }

   if (!mInHGDrag) {
      ResetUI();
   } else {
      RemoveBlock();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::ShowDetWnd --
 *
 *      Show or hide the drag detection window in the given role.
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
DnDUIX11::ShowDetWnd(bool show,      // IN: show (true) or hide (false)
                     int32 x,        // IN: detection window's destination x-coord
                     int32 y,        // IN: detection window's destination y-coord
                     DetWndRole role)// IN: what the window is being shown for
{
   g_debug("%s: enter 0x%lx show %d x %d y %d role %d\n",
           __FUNCTION__,
           mDetWnd->Get_Current_Drop_ctx(), show, x, y, role);

   /*
    * Where the compositor supports the native drag source, the native drop
    * target stands in for the detection window in a guest to host drag. See
    * LocalShowNativeDropTarget().
    */
   if (role == DET_WND_DROP_TARGET) {
      if (show && !mGHNativeFallback && LocalShowNativeDropTarget(x, y)) {
         return;
      }
      if (!show) {
         LocalHideNativeDropTarget(true);
         mGHNativeFallback = false;
      }
   }

   /* If the window is being shown, move it to the right place. */
   if (show) {
      UpdateWorkArea();
      /* A guest to host drag starts with the window shown as a drop target. */
      if (role == DET_WND_DROP_TARGET) {
         LocalXdndChoosePath();
      }
      x = MAX(x - DRAG_DET_WINDOW_WIDTH / 2, mOriginX);
      y = MAX(y - DRAG_DET_WINDOW_WIDTH / 2, mOriginY);
      /*
       * During a host to guest drag the detection window is the drag source,
       * and nothing needs to find it, so skip the wait for the window
       * manager. The caller passes the role because mInHGDrag is not set yet
       * when that drag begins.
       */
      mDetWnd->UpdateDetWnd(show, x, y, role != DET_WND_DRAG_SOURCE,
                            role == DET_WND_DROP_TARGET && LocalMutterBridgesXdnd());
   } else {
      g_debug("%s: hide\n", __FUNCTION__);
      mDetWnd->UpdateDetWnd(show, x, y);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DnDUIX11::OnUpdateDetWnd --
 *
 *      Callback to show/hide drag detection window. This is only wired to the
 *      common layer's updateDetWndChanged signal. The host drives it on the
 *      guest to host path, where the window is a drop target, and the common
 *      layer also hides the window when it returns to READY after a host to
 *      guest transfer.
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
   /*
    * A bridged host to guest drag can still be starting from the window, or
    * waiting to release on it, when the host's files arrive and the common
    * layer returns to READY. Hiding the window then takes the drag's origin
    * away and Mutter drops the drag. LocalXdndRelease() hides it afterwards.
    */
   if (!show && (mHGXdndWaitBegin || mHGXdndReleaseTimer != 0)) {
      g_debug("%s: keeping the window for the drag in progress\n",
              __FUNCTION__);
      return;
   }
   ShowDetWnd(show, x, y, DET_WND_DROP_TARGET);
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
   /*
    * The drag has left the guest again. Give the native drop target the
    * pointer back, and move the pointer so that the drag enters it.
    */
   if (mGHNative) {
      WaylandDrag_DropTargetSetInput(TRUE);
      LocalNudgeNativeDropTarget(mMousePosX, mMousePosY);
      return;
   }
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
   /*
    * GTK starts a drag from the press and motion only when the main loop
    * runs. When the host cancels before then, the GTK drag of the old press
    * can begin after OnSrcDragBegin() of the next host drag. That drag has
    * no drag of its own until OnMoveMouse() presses, so a begin before the
    * press belongs to the old one. Keeping it would make its end look
    * current to LocalXdndIsStaleDrag().
    */
   if (   !this_instance->mInHGDrag
       || !this_instance->mHGDragStarted
       || !this_instance->mHGButtonDown) {
      g_debug("%s: not a drag of the current host drag\n", __FUNCTION__);
      return;
   }
   if (this_instance->mHGXdndDrag != NULL) {
      g_object_unref(this_instance->mHGXdndDrag);
   }
   this_instance->mHGXdndDrag = GDK_DRAG(g_object_ref(drag->gobj()));
   if (this_instance->mHGXdndWaitBegin) {
      /* OnXdndBeginTimeout() moves the pointer off; see XDND_LEAVE_DELAY_MS. */
      this_instance->mHGXdndLeaveAt = g_get_monotonic_time() +
                                      (gint64) this_instance->mXdndLeaveDelayMs * 1000;
   }
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
   if (this_instance->LocalXdndIsStaleDrag(drag->gobj())) {
      return;
   }
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
   if (this_instance->LocalXdndIsStaleDrag(drag->gobj())) {
      return true;
   }
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
 * DnDUIX11::LocalBeginGHDrag --
 *
 *      Start a new guest to host drag, before its data is read. The GTK
 *      drop target and the native one both come through here, and both
 *      hand the data to OnGHDropRead().
 *
 *-----------------------------------------------------------------------------
 */

void
DnDUIX11::LocalBeginGHDrag(void)
{
   if (mGHDnDInProgress) {
      /*
       * Errors in the GTK layer can lead to dropped information without
       * notification. Therefore, a cleanup process is necessary.
       */
      g_warning("%s: Previous drag failed\n", __FUNCTION__);
      ResetUI();
   }

   g_debug("%s: new drag, need to get data for host\n", __FUNCTION__);
   /*
    * This is a new drag operation. We need to start a drag thru the
    * RPC, and to the host. Before we can tell the host, we have to
    * retrieve the drop data.
    */
   mGHDnDInProgress = true;
   /* only begin drag enter after we get the data */
   /* Need to grab all of the data. */
   CPClipboard_Clear(&mClipboard);
   mNumPendingRequest = 0;
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
   g_debug("%s: XDND accept reached the detection window\n", __FUNCTION__);
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

   this_instance->LocalBeginGHDrag();

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

   g_debug("%s: XDND enter at (%f, %f)\n", __FUNCTION__, x, y);

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
    *
    * Not when the native drop target has the drag. The detection window
    * would take the drag from it.
    */
   if (!mGHNative) {
      mDetWnd->SimulateXEvents(true, false, true, true, false, 0, 0);
   }
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
   if (   mGdkSurface
       && xutils::GetCardinalList(mGdkSurface, "_NET_WORKAREA", values)
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
