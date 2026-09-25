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
 * @file dragDetWndX11GTK4.cpp
 *
 * Detection window code for Linux/X11, based on Gtkmm-4.0.
 */

#define G_LOG_DOMAIN "dndcp"

#include "tracer.hh"
#include "dragDetWndX11GTK4.h"
#include "dndUIX11GTK4.h"
#include "waylandDrag.h"
#include <list>

extern "C" {
#include <X11/extensions/XTest.h>       /* for XTest*() */
#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
}

/* See DragDetWnd::OnDropTargetNudge(). */
#define DROP_TARGET_NUDGES    3
#define DROP_TARGET_NUDGE_MS  100

/* How long UpdateDetWnd() waits for the window manager to adopt the window. */
#define DET_WND_MANAGE_TIMEOUT_MS 500


/*
 *-----------------------------------------------------------------------------
 *
 * DetWndUnderWayland --
 *
 *    Whether this session runs under a Wayland compositor, with Xwayland as
 *    the X server.
 *
 * Results:
 *    True for a Wayland session, false for an Xorg session.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static bool
DetWndUnderWayland(void)
{
   const char *type = getenv("XDG_SESSION_TYPE");
   const char *display = getenv("WAYLAND_DISPLAY");

   if (type != NULL && strstr(type, "wayland") != NULL) {
      return true;
   }
   /* The same rule as the backend choice in copyPasteDnDWrapper.cpp. */
   return display != NULL && *display != '\0' &&
          (type == NULL || strcmp(type, "x11") != 0);
}


/**
 *
 * Constructor.
 */

DragDetWnd::DragDetWnd(int useUInput_fd)
   : m_isVisible(false),
     mWnd(NULL),
     mSource(NULL),
     mTarget(NULL),
     mUseUInput(false),
     mUnderWayland(DetWndUnderWayland()),
     mWarpXPointer(true),
     mAbove(false),
     mNudgeTimer(0),
     mNudgeX(0),
     mNudgeY(0),
     mNudgesLeft(0),
     mPlaced(false),
     mPlacedX(0),
     mPlacedY(0),
     mScreenWidth(0),
     mScreenHeight(0),
     mXEventHandler(0),
     mXdndEnter(None),
     mXdndDrop(None),
     mXdndDropped(false)
{
   GdkDisplay *gdkDisplay = gdk_display_get_default();
   std::vector<GType> types;
   mWnd = new Gtk::Window();
   if (!mWnd) {
      g_error("%s: unable to allocate DragDetWnd object\n", __FUNCTION__);
      return;
   }

   /*
    * Under Wayland the detection window must never be activated. GTK4 cannot
    * tell the window manager that a toplevel takes no focus, so
    * ApplyDetWndMode() sets the hints for that on the X11 window below.
    *
    * KWin activates a focusable detection window on present(). The window
    * then gets a task switcher entry, takes focus from the application being
    * dragged from, and takes input until UNGRAB_TIMEOUT expires.
    *
    * Under Xorg the window takes focus when it is shown. Mutter and the
    * window managers derived from it stack a newly mapped window that they
    * do not activate below the active one, where neither the faked press nor
    * the XDND drag reaches it.
    */
   mWnd->set_decorated(false);
   mWnd->set_resizable(false);
   mWnd->set_modal(false);
   mWnd->set_can_focus(!mUnderWayland);
   mWnd->set_focusable(!mUnderWayland);
   mWnd->set_sensitive(false);
   // Makes this window transparent because we don't want user to see it.
   mWnd->set_opacity(0.01);
   mWnd->set_visible(true);
   /*
    * The X window exists only once GTK has shown the window, and the window
    * manager reads the hints when the window is mapped. Apply them during
    * this first map so that the next map picks them up.
    */
   ApplyDetWndMode();
   mWnd->set_visible(false);

   //dnd Source controller
   mSource = Gtk::DragSource::create();
   if (!mSource) {
      g_error("%s: unable to allocate source for DragDetWnd object\n",
              __FUNCTION__);
      goto fail;
   }
   mSource->set_actions(Gdk::DragAction::COPY | Gdk::DragAction::MOVE);

   //dnd target controller
   mTarget = Gtk::DropTarget::create(G_TYPE_INVALID, Gdk::DragAction::COPY | Gdk::DragAction::MOVE);
   if (!mTarget) {
      g_error("%s: unable to allocate target for DragDetWnd object\n",
              __FUNCTION__);
      goto fail;
   }

   // Put GDK_TYPE_FILE_LIST at first, and that will make GTK report file list when possible.
   types.push_back(GDK_TYPE_FILE_LIST);
   types.push_back(G_TYPE_FILE);
   types.push_back(G_TYPE_STRING);
   mTarget->set_gtypes(types);
   /*
    * No preload. GTK would read the value itself on enter, and from a KDE
    * source it picks application/vnd.portal.filetransfer, which fails here.
    * DnDUIX11::StartGHRead() reads text/uri-list or text instead. The gtypes
    * above still decide whether a drop is accepted.
    */
   mTarget->set_preload(false);

   /* See OnXEvent(). */
   if (gdkDisplay != NULL && GDK_IS_X11_DISPLAY(gdkDisplay)) {
      mXEventHandler = g_signal_connect(gdkDisplay, "xevent",
                                        G_CALLBACK(OnXEvent), this);
   }

/* Need to check USE_UINPUT, as FreeBSD does not support this */
#ifdef USE_UINPUT
   // Uinput simulation for wayland backend.
   if (useUInput_fd != -1) {
       g_debug("%s: Using uinput simulation", __FUNCTION__);
       Display *display = XOpenDisplay(NULL);

       /*
        * A compositor may start Xwayland on demand and stop it again when
        * the last client goes, so there is not always a server to ask. The
        * pointer then moves through XTest instead, which needs no screen
        * size from here.
        */
       if (display != NULL) {
           Screen * scrn = DefaultScreenOfDisplay(display);
           if (FakeMouse_Init(useUInput_fd, scrn->width, scrn->height)) {
               mUseUInput = true;
               mScreenWidth = scrn->width;
               mScreenHeight = scrn->height;
               g_debug("%s: mScreenWidth is %d, mScreenHeight is %d", __FUNCTION__, mScreenWidth, mScreenHeight);
           }
           // Disconnect the connection to X Server.
           XCloseDisplay(display);
       } else {
           g_warning("%s: no X display, uinput disabled\n", __FUNCTION__);
       }
   }
#endif
   return;
fail:
   if (mWnd) {
       delete mWnd;
       mWnd = NULL;
   }
}


/**
 *
 * Destructor.
 */

DragDetWnd::~DragDetWnd()
{
   GdkDisplay *gdkDisplay = gdk_display_get_default();

   if (mXEventHandler != 0 && gdkDisplay != NULL) {
      g_signal_handler_disconnect(gdkDisplay, mXEventHandler);
      mXEventHandler = 0;
   }
   CancelDropTargetNudges();
#ifdef USE_UINPUT
   if (mUseUInput) {
      FakeMouse_Destory();
   }
#endif
   if (mWnd) {
      delete mWnd;
      mWnd = NULL;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::RefreshScreenSize --
 *
 *    Update the uinput device after a change of screen size.
 *
 *    The device declares its absolute range when it is created, and
 *    FakeMouse_Move() clamps to the size it was created with. After a
 *    resolution change the range describes the old screen. The compositor
 *    scales the old range onto the new output, and the clamp cuts off
 *    coordinates beyond the old size. The GTK3 build checks the screen
 *    size in DnDUIX11::OnSrcDragBegin() for the same reason.
 *
 *    In a Wayland session with one output, the size comes from that
 *    output's mode. See WaylandDrag_GetOutputSize() for why the X screen
 *    does not do there. Otherwise this opens a fresh X connection, because
 *    Xlib updates the screen size of an open connection only for a client
 *    that processes RandR events itself.
 *
 * Side effects:
 *    Recreates the uinput device if the size has changed. If that fails the
 *    pointer goes back to XTest.
 *
 *-----------------------------------------------------------------------------
 */

void
DragDetWnd::RefreshScreenSize(void)
{
#ifdef USE_UINPUT
   int width;
   int height;
   const char *source = "Wayland output";

   if (!mUseUInput) {
      return;
   }
   if (!WaylandDrag_GetOutputSize(&width, &height)) {
      Display *display = XOpenDisplay(NULL);
      Screen *scrn;

      if (display == NULL) {
         return;
      }
      scrn = DefaultScreenOfDisplay(display);
      width = scrn->width;
      height = scrn->height;
      XCloseDisplay(display);
      source = "X screen";
   }
   if (width != mScreenWidth || height != mScreenHeight) {
      g_debug("%s: screen is now %dx%d from the %s, was %dx%d\n",
              __FUNCTION__, width, height, source, mScreenWidth,
              mScreenHeight);
      if (FakeMouse_Update(width, height)) {
         mScreenWidth = width;
         mScreenHeight = height;
      } else {
         g_warning("%s: could not recreate the uinput device\n", __FUNCTION__);
         mUseUInput = false;
      }
   }
#endif
}


/**
 * Get the actual widget.
 */

Gtk::Widget *
DragDetWnd::GetWnd()
{
   return static_cast<Gtk::Widget*>(mWnd);
}

/**
 * Register GTK callback which was triggered when a drag begin happens.
 */

bool
DragDetWnd::register_source_drag_begin_handler(source_drag_begin_handler handler, void *ctx)
{
   mSource->signal_drag_begin().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback which was triggered when a drag end happens.
 */

bool
DragDetWnd::register_source_drag_end_handler(source_drag_end_handler handler, void *ctx)
{
   mSource->signal_drag_end().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when a drag has failed
 */

bool
DragDetWnd::register_source_drag_cancel_handler(source_drag_cancel_handler handler, void *ctx)
{
   mSource->signal_drag_cancel().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when a drag is about to be initiated.
 */

bool
DragDetWnd::register_source_drag_prepare_handler(source_drag_prepare_handler handler, void *ctx)
{
   mSource->signal_prepare().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when a drop operation is about to begin.
 */

bool
DragDetWnd::register_target_drag_accept_handler(target_drag_accept_handler handler, void *ctx)
{
   mTarget->signal_accept().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when the pointer enters the widget.
 */

bool
DragDetWnd::register_target_drag_enter_handler(target_drag_enter_handler handler, void *ctx)
{
   mTarget->signal_enter().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when the user drops the data onto the widget.
 */

bool
DragDetWnd::register_target_drag_drop_handler(target_drag_drop_handler handler, void *ctx)
{
   mTarget->signal_drop().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when the pointer leaves the widget.
 */

bool
DragDetWnd::register_target_drag_leave_handler(target_drag_leave_handler handler, void *ctx)
{
   mTarget->signal_leave().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Register GTK callback when the drop value changed. when set droptarget controller preload,
 * the value may be ready befor drop action happen. When drop leave happens, the value will be
 * set to NULL.
 */

bool
DragDetWnd::register_target_value_handler(target_drag_value_handler handler, void *ctx)
{
    mTarget->property_value().signal_changed().connect(sigc::bind(sigc::ptr_fun(handler), ctx));
    return true;
}


/**
 * Register GTK callback while the pointer is moving over the drop target.
 */

bool
DragDetWnd::register_target_drag_motion_handler(target_drag_motion_handler handler, void *ctx)
{
   mTarget->signal_motion().connect(sigc::bind(handler, ctx), false);
   return true;
}


/**
 * Enable drag source and target.
 */

bool
DragDetWnd::EnableDND(void)
{
   g_debug("%s\n", __FUNCTION__);
   mWnd->add_controller(mSource);
   mWnd->add_controller(mTarget);
   return true;
}


/**
 * Get the current drag contax from source.
 */

unsigned long
DragDetWnd::Get_Current_Drag_ctx(void)
{
   Glib::RefPtr< Gdk::Drag > current_drag = mSource->get_drag();
   if (current_drag != NULL)
       return (unsigned long) current_drag->gobj();
   else
       return 0;
}


/**
 * Disable drag source and target.
 */

bool
DragDetWnd::DisableDND(void)
{
   g_debug("%s\n", __FUNCTION__);
   mWnd->remove_controller(mSource);
   mWnd->remove_controller(mTarget);
   return true;
}


/**
 * Query if drop value is ready.
 */

bool
DragDetWnd::Is_DropValue_Ready(void)
{
    Glib::ValueBase value= mTarget->get_value();
    return G_IS_VALUE(value.gobj());
}


/**
 * Query if current drop type is supported.
 */

bool
DragDetWnd::Is_Current_Drop_Supported(void)
{
   Glib::RefPtr< Gdk::Drop > current_drop = mTarget->get_drop();
   return this->Is_Drop_Supported(current_drop);
}


/**
 * Query if a drop type is supported.
 */

bool
DragDetWnd::Is_Drop_Supported(const Glib::RefPtr< Gdk::Drop > &drop)
{
   Glib::RefPtr<Gdk::ContentFormats> supported_format = drop->get_formats();
   g_debug("%s content type is %s", __FUNCTION__, supported_format->to_string().c_str());

   return (supported_format->contain_gtype(GDK_TYPE_FILE_LIST) ||
           supported_format->contain_gtype(G_TYPE_STRING));
}


/**
 * Query the current drop context from target.
 */

unsigned long
DragDetWnd::Get_Current_Drop_ctx(void)
{
   Glib::RefPtr< Gdk::Drop > current_drop = mTarget->get_drop();
   if (current_drop != NULL)
       return (unsigned long) mTarget->get_drop()->gobj();
   else
       return 0;
}


/**
 * Get drop value from detect window.
 */

bool
DragDetWnd::Get_Drop_Value(std::string &value, std::string &value_type)
{
    Glib::ValueBase cur_value= mTarget->get_value();
    bool ret = false;

    if (G_IS_VALUE(cur_value.gobj())) {
      if (G_VALUE_HOLDS(cur_value.gobj(), GDK_TYPE_FILE_LIST)) {
         GSList *file_list = gdk_file_list_get_files(static_cast<GdkFileList *>(g_value_get_boxed(cur_value.gobj())));
         g_debug("%s Get file list ...", __FUNCTION__);
         for (GSList *l = file_list; l != nullptr; l = l->next) {
            GFile *file = static_cast<GFile *>(l->data);
            gchar *uri = g_file_get_uri(file);
            if (uri) {
               g_debug("%s Get file uri: %s", __FUNCTION__, uri);
               value += uri;
               value += '\n';
            }
         }
         if (value.length()) {
            ret = true;
            value_type = DRAG_TARGET_NAME_URI_LIST;
         }
      } else if (G_VALUE_HOLDS(cur_value.gobj(), G_TYPE_FILE)) {
         GFile* gfile = G_FILE(g_value_get_object(cur_value.gobj()));
         g_debug("%s Get file ...", __FUNCTION__);
         if (gfile) {
            gchar *uri = g_file_get_uri(gfile);
             if (uri) {
               g_debug("%s Get file uri: %s", __FUNCTION__, uri);
               value += uri;
            }
        }
      } else if (G_VALUE_HOLDS(cur_value.gobj(), G_TYPE_STRING)) {
         const gchar* text= g_value_get_string(cur_value.gobj());
         Glib::RefPtr<Gdk::ContentFormats> supported_format = mTarget->get_drop()->get_formats();

         value += text;
         g_debug("%s Get str: %s", __FUNCTION__, text);
         if (supported_format->to_string().find(TARGET_NAME_TEXT_RTF) != Glib::ustring::npos) {
             value_type = TARGET_NAME_TEXT_RTF;
         } else {
             value_type = TARGET_NAME_STRING;
         }
         g_debug("%s Set type as: %s", __FUNCTION__, value_type.c_str());
         ret = true;

      } else {
         g_warning("%s Unsupported DND Value type %s",
	           __FUNCTION__, g_type_name(G_TYPE_FROM_INSTANCE(cur_value.gobj())));
      }
    } else
       g_debug("%s No value available %ld", __FUNCTION__, (unsigned long) cur_value.gobj());

   return ret;
}


/**
 * Flush the X connection.
 */

void
DragDetWnd::Flush()
{
   Glib::RefPtr<Gdk::Display> gdkdisplay = Gdk::Display::get_default();
   g_debug("%s\n", __FUNCTION__);
   if (gdkdisplay) {
      gdkdisplay->sync();
      gdkdisplay->flush();
   }
}


/**
 * Show the window.
 */

void
DragDetWnd::Show(void)
{
   Glib::RefPtr<Gdk::Display> display = Gdk::Display::get_default();
   g_debug("%s: enter\n", __FUNCTION__);
   if (!display) {
      g_error("%s: Cannot get default display \n", __FUNCTION__);
      return;
   }

   /*
    * Map the window at the size it is used at. UpdateDetWnd() resizes it to
    * this straight afterwards, but a compositor only has to honour the size
    * a window is mapped with. A compositor that ignores the later resize
    * leaves the detection window covering the whole output, on top of the
    * drop target, and the detection window then takes the drop meant for the
    * window underneath.
    */
   mWnd->set_default_size(DRAG_DET_WINDOW_WIDTH * 2, DRAG_DET_WINDOW_WIDTH * 2);
   /*
    * set_default_size() applies only the first time the window is mapped,
    * and this window is mapped once and then hidden and shown for each drag.
    * GTK applies a size request on every map, so a compositor that sizes the
    * window itself gets the same size each time.
    */
   mWnd->set_size_request(DRAG_DET_WINDOW_WIDTH * 2, DRAG_DET_WINDOW_WIDTH * 2);

   mWnd->set_sensitive(true);
   mWnd->set_visible(true);

   /*
    * Apply the hints again on every show. GTK may have recreated the X
    * window, and the hints must be in place before the drag arrives.
    */
   ApplyDetWndMode();

   /*
    * Under Wayland the window must not take focus. Detection works through
    * the pointer grab and XDND, not keyboard focus.
    */
   if (!mUnderWayland) {
      if (mWnd->grab_focus()) {
         g_debug("%s: grab_focus successfully\n", __FUNCTION__);
      } else {
         Warning("%s: grab_focus failed! May unable to detect drag actions\n",
                 __FUNCTION__);
      }
   }

   m_isVisible = true;

   if (mWnd->get_focus())
   {
      g_debug("%s: detwnd have focus on %s\n", __FUNCTION__, G_OBJECT_TYPE_NAME(mWnd->get_focus()->gobj()));
   }
   else
      g_debug("%s: detwnd doesn't have focus\n", __FUNCTION__);

   g_debug("%s: exiting \n", __FUNCTION__);
}


/**
 * Hide the window.
 */

void
DragDetWnd::Hide(void)
{
   g_debug("%s: enter\n", __FUNCTION__);
   g_debug("%s: detwnd current visible is  %d\n", __FUNCTION__, mWnd->is_visible());
   g_debug("%s: detwnd current focus is %d", __FUNCTION__, gtk_widget_has_focus(GTK_WIDGET(mWnd->gobj())));

   mWnd->set_sensitive(false);
   mWnd->set_visible(false);
   m_isVisible = false;
   g_debug("%s: exiting \n", __FUNCTION__);
}


/**
 * Lower the the GTK window by set lower its surface
 */

static bool lower_wnd(Gtk::Window *window)
{
   GtkWidget* widget = GTK_WIDGET(window->gobj());
   GtkNative* native = gtk_widget_get_native(widget);

   if (native) {
      GdkSurface* surface = gtk_native_get_surface(native);
      if (surface && GDK_IS_TOPLEVEL(surface)) {
         gdk_toplevel_lower(GDK_TOPLEVEL(surface));
         return true;
      }
   }

   return false;
}


/**
 * Get the monitor of the GTK window
 */

static Glib::RefPtr<Gdk::Monitor> get_monitor(Gtk::Window *window)
{
   GtkWidget* widget = GTK_WIDGET(window->gobj());
   GtkNative* native = gtk_widget_get_native(widget);

   if (native) {
      GdkSurface* surface = gtk_native_get_surface(native);
      g_object_ref(surface);
      Glib::RefPtr<Gdk::Surface> share_surface(Glib::wrap(surface));
      if (surface)
         return Gdk::Display::get_default()->get_monitor_at_surface(share_surface);
   }
   return NULL;
}


/**
 * Raise the window.
 */

void
DragDetWnd::Raise(void)
{
   Display *display;
   Window xwindow;

   /*
    * Under Xorg, activate the window as well, so that the window manager
    * stacks it on top.
    */
   if (!mUnderWayland) {
      mWnd->present();
      g_debug("%s: detwnd Presenting\n", __FUNCTION__);
      Flush();
      return;
   }

   /*
    * Under Wayland, present() asks the window manager to activate the
    * window, which must not happen here. Raise the X11 window directly
    * instead, since only the stacking order matters.
    */
   if (GetXWindow(&display, &xwindow)) {
      XWindowChanges changes;

      /*
       * For a managed window XRaiseWindow() alone is only a request, and the
       * window manager can restack the detection window under the drag source
       * during the drag. Set the stacking mode explicitly as well.
       */
      changes.stack_mode = Above;
      XConfigureWindow(display, xwindow, CWStackMode, &changes);
      XRaiseWindow(display, xwindow);
      XFlush(display);
      g_debug("%s: detwnd 0x%lx raised\n", __FUNCTION__,
              (unsigned long) xwindow);
   }
   Flush();
}


/*
 *-----------------------------------------------------------------------------
 *
 * DetWndIsManaged --
 *
 *    Check whether the window manager lists this window in _NET_CLIENT_LIST.
 *    A compositor looks for a drag target among managed windows only, so a
 *    drag can reach only a managed detection window.
 *
 * Results:
 *    True if the window manager has adopted the window.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
DetWndIsManaged(Display *display,  // IN
                Window xwindow)    // IN
{
   Atom clientList = XInternAtom(display, "_NET_CLIENT_LIST", False);
   Bool found = False;
   long offset = 0;

   /*
    * A busy desktop can have more clients than one request returns, and a
    * truncated read would report a managed window as unmanaged. Keep reading
    * while bytesAfter says there is more.
    */
   do {
      Atom actualType;
      int actualFormat;
      unsigned long n, bytesAfter;
      unsigned char *data = NULL;

      if (XGetWindowProperty(display, DefaultRootWindow(display), clientList,
                             offset, 256, False, XA_WINDOW, &actualType,
                             &actualFormat, &n, &bytesAfter,
                             &data) != Success) {
         return False;
      }
      if (data == NULL) {
         return False;
      }
      if (actualType != XA_WINDOW || actualFormat != 32) {
         XFree(data);
         return False;
      }

      Window *list = (Window *) data;
      for (unsigned long i = 0; i < n && !found; i++) {
         found = (list[i] == xwindow);
      }
      XFree(data);

      if (found || bytesAfter == 0) {
         break;
      }
      offset += (long) n;
   } while (TRUE);

   return found;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::WaitUntilManaged --
 *
 *    Wait for the window manager to adopt the detection window.
 *
 *    A window does not become a drag target the moment it maps. KWin needs it
 *    to be a managed client and to have committed a buffer, and both happen
 *    around 100ms after MapNotify. This polls _NET_CLIENT_LIST every 10ms,
 *    and then waits 40ms more for the buffer, whose commit cannot be seen
 *    from here.
 *
 *    KWin picks the target on pointer motion and does not look again until
 *    the pointer moves. In a guest to host drag the first motion is the move
 *    that UpdateDetWnd() makes through SimulateXEvents() after this returns,
 *    and on KWin a move before the window is ready loses the drag for its
 *    whole duration. OnDropTargetNudge() repeats the move from the main
 *    loop.
 *
 * Results:
 *    True if the window is managed, false if timeoutMs passed first.
 *
 * Side effects:
 *    Sleeps without running the main loop.
 *
 *-----------------------------------------------------------------------------
 */

bool
DragDetWnd::WaitUntilManaged(unsigned int timeoutMs)  // IN
{
   Display *display;
   Window xwindow;

   if (!GetXWindow(&display, &xwindow)) {
      return false;
   }

   const unsigned int stepMs = 10;
   const unsigned int settleMs = 40;
   gint64 deadline = g_get_monotonic_time() + timeoutMs * 1000;
   gint64 started = g_get_monotonic_time();

   /*
    * Do not call g_main_context_iteration() here. This runs from an RPC
    * handler, and iterating the main loop dispatches the RPC source again.
    * GuestDnDMgr::OnRpcQueryExiting() shows the detection window before it
    * records the session id, so a command delivered from inside this call
    * would run the whole sequence again, and the outer call would then write
    * the older session id last. Every later command for the real session is
    * rejected after that, and drag and drop stays broken until a reset.
    *
    * Nothing here needs the main loop. The window manager acts on the X
    * requests that Show() and SetGeometry() have already flushed, so
    * _NET_CLIENT_LIST is updated whether or not this process is running its
    * own loop.
    */
   while (g_get_monotonic_time() < deadline) {
      if (DetWndIsManaged(display, xwindow)) {
         g_usleep(settleMs * 1000);
         g_debug("%s: detwnd 0x%lx managed after %" G_GINT64_FORMAT "ms\n",
                 __FUNCTION__, (unsigned long) xwindow,
                 (g_get_monotonic_time() - started) / 1000);
         RequestSkipTaskbar();
         return true;
      }
      g_usleep(stepMs * 1000);
   }

   g_warning("%s: detwnd 0x%lx still not managed after %ums; the drag will "
             "probably be offered to another window\n", __FUNCTION__,
             (unsigned long) xwindow, timeoutMs);

   return false;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::PointerIsOver --
 *
 *    Check whether the X server has the pointer at (x, y), within a pixel,
 *    and over the detection window.
 *
 * Results:
 *    True if both hold.
 *
 * Side effects:
 *    Round trip to the X server.
 *
 *-----------------------------------------------------------------------------
 */

bool
DragDetWnd::PointerIsOver(int x,   // IN
                          int y)   // IN
{
   Display *display;
   Window xwindow;
   Window retRoot, retChild;
   int rootX, rootY, winX, winY;
   unsigned int mask;

   if (!GetXWindow(&display, &xwindow)) {
      return false;
   }

   /*
    * The X server learns where the pointer is only while it is over an X
    * window, and otherwise goes on reporting the last place it saw it. So
    * the position has to match as well as the window.
    */
   XSync(display, False);
   return XQueryPointer(display, DefaultRootWindow(display), &retRoot,
                        &retChild, &rootX, &rootY, &winX, &winY, &mask) &&
          retChild == xwindow &&
          ABS(rootX - x) <= 1 && ABS(rootY - y) <= 1;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::GetXWindow --
 *
 *    Look up the X display and the X window of the detection window.
 *
 * Results:
 *    True if the window has an X11 surface, and then *display and *xwindow
 *    are set.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

bool
DragDetWnd::GetXWindow(Display **display,  // OUT
                       Window *xwindow)    // OUT
{
   GtkNative *native = gtk_widget_get_native(GetWnd()->gobj());
   GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;

   if (surface == NULL || !GDK_IS_X11_SURFACE(surface)) {
      return false;
   }
   *display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
   *xwindow = gdk_x11_surface_get_xid(surface);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::WmIsMutter --
 *
 *    Whether the X window manager is Mutter, found the EWMH way. The root
 *    window's _NET_SUPPORTING_WM_CHECK names a window of the window manager,
 *    which carries the same property pointing to itself and the manager's
 *    name in _NET_WM_NAME. Mutter sets that name from its context, which
 *    GNOME Shell creates as "GNOME Shell" and the mutter binary as "Mutter".
 *
 *    This reads the X server each time, three round trips, because the
 *    window manager can be replaced while the plugin runs.
 *
 * Results:
 *    True if the name contains "GNOME Shell" or "Mutter", in any case.
 *    wmName is set to the name, or left empty if there is none.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

bool
DragDetWnd::WmIsMutter(std::string &wmName)   // OUT
{
   GdkDisplay *gdkDisplay = gtk_widget_get_display(GetWnd()->gobj());
   Display *display;
   Atom check;
   Atom actualType;
   int actualFormat;
   unsigned long nItems;
   unsigned long bytesAfter;
   unsigned char *data;
   Window wmWindow = None;
   Window self = None;
   bool isMutter = false;

   wmName.clear();
   if (gdkDisplay == NULL || !GDK_IS_X11_DISPLAY(gdkDisplay)) {
      return false;
   }
   display = gdk_x11_display_get_xdisplay(gdkDisplay);
   check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);

   data = NULL;
   if (XGetWindowProperty(display, DefaultRootWindow(display), check, 0, 1,
                          False, XA_WINDOW, &actualType, &actualFormat,
                          &nItems, &bytesAfter, &data) == Success &&
       actualType == XA_WINDOW && actualFormat == 32 && nItems == 1) {
      wmWindow = *(Window *)data;
   }
   if (data != NULL) {
      XFree(data);
   }
   if (wmWindow == None) {
      return false;
   }

   /*
    * A window manager that has exited leaves the root property behind, and
    * its window is then gone or reused, so check that it points to itself.
    */
   gdk_x11_display_error_trap_push(gdkDisplay);
   data = NULL;
   if (XGetWindowProperty(display, wmWindow, check, 0, 1, False, XA_WINDOW,
                          &actualType, &actualFormat, &nItems, &bytesAfter,
                          &data) == Success &&
       actualType == XA_WINDOW && actualFormat == 32 && nItems == 1) {
      self = *(Window *)data;
   }
   if (data != NULL) {
      XFree(data);
   }
   data = NULL;
   if (self == wmWindow &&
       XGetWindowProperty(display, wmWindow,
                          XInternAtom(display, "_NET_WM_NAME", False), 0, 64,
                          False, XInternAtom(display, "UTF8_STRING", False),
                          &actualType, &actualFormat, &nItems, &bytesAfter,
                          &data) == Success &&
       actualFormat == 8 && data != NULL) {
      wmName.assign((const char *)data, nItems);
   }
   if (data != NULL) {
      XFree(data);
   }
   gdk_x11_display_error_trap_pop_ignored(gdkDisplay);

   if (!wmName.empty()) {
      char *folded = g_utf8_casefold(wmName.c_str(), -1);

      isMutter = strstr(folded, "gnome shell") != NULL ||
                 strstr(folded, "mutter") != NULL;
      g_free(folded);
   }
   return isMutter;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::SendWmState --
 *
 *    Ask the window manager to add or remove up to two _NET_WM_STATE atoms
 *    on the mapped detection window.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Sends a client message to the root window.
 *
 *-----------------------------------------------------------------------------
 */

void
DragDetWnd::SendWmState(long action,   // IN: 1 to add, 0 to remove
                        Atom first,    // IN
                        Atom second,   // IN: or None
                        long source)   // IN: 1 application, 2 pager
{
   Display *display;
   Window xwindow;
   XEvent ev;

   if (!GetXWindow(&display, &xwindow)) {
      return;
   }

   memset(&ev, 0, sizeof ev);
   ev.xclient.type = ClientMessage;
   ev.xclient.window = xwindow;
   ev.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
   ev.xclient.format = 32;
   ev.xclient.data.l[0] = action;
   ev.xclient.data.l[1] = first;
   ev.xclient.data.l[2] = second;
   ev.xclient.data.l[3] = source;
   XSendEvent(display, DefaultRootWindow(display), False,
              SubstructureNotifyMask | SubstructureRedirectMask, &ev);
   XFlush(display);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::SetAbove --
 *
 *    Ask the window manager to keep the detection window above other
 *    windows, or to stop doing so.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Sends a _NET_WM_STATE client message to the root window.
 *
 *-----------------------------------------------------------------------------
 */

void
DragDetWnd::SetAbove(bool above)   // IN
{
   Display *display;
   Window xwindow;

   /*
    * The add is sent every time, because the window manager drops a
    * window's _NET_WM_STATE when the window is withdrawn, and an earlier add
    * does not survive the next hide. A remove is sent only after an add.
    */
   if (!GetXWindow(&display, &xwindow) || (!above && !mAbove)) {
      return;
   }

   SendWmState(above ? 1 : 0,
               XInternAtom(display, "_NET_WM_STATE_ABOVE", False), None, 2);
   mAbove = above;
   g_debug("%s: %s\n", __FUNCTION__, above ? "above other windows" : "normal stacking");
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::RequestSkipTaskbar --
 *
 *    Keep the detection window out of the taskbar and pager.
 *
 *    The window manager reads the _NET_WM_STATE property only before the
 *    window is mapped, and GTK rewrites that property when it maps the
 *    window. Once the window is mapped, the state has to be requested with a
 *    client message.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Sends a client message to the root window.
 *
 *-----------------------------------------------------------------------------
 */

void
DragDetWnd::RequestSkipTaskbar(void)
{
   Display *display;
   Window xwindow;

   if (!GetXWindow(&display, &xwindow)) {
      return;
   }

   SendWmState(1,
               XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False),
               XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False), 1);
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::ApplyDetWndMode --
 *
 *    Set the window manager hints of the detection window.
 *
 *    Under Wayland the window has to stay managed. A compositor bridging a
 *    native Wayland drag to an Xwayland window looks for the target among its
 *    managed clients only, so an override-redirect detection window never
 *    receives XdndEnter. On KWin the lookup is Workspace::findClient(),
 *    called from XwlDropHandler::updateDragTarget(), and pickDragTarget()
 *    skips anything that is not a client.
 *
 *    Under Wayland hints also keep the window from being activated. They are
 *    _NET_WM_WINDOW_TYPE_DND, _NET_WM_USER_TIME 0 and WM_HINTS input False.
 *    Under Xorg the window is activated when it is shown, so it gets none of
 *    these. Both get fixed size hints, the requested position, and skip
 *    taskbar and skip pager.
 *
 *    The window manager reads these when the window is mapped, and the X
 *    window exists only once GTK has shown it. This therefore runs while the
 *    window is shown, for the next map to see. It is idempotent and is called
 *    from both the constructor and Show().
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Changes window attributes and properties on the X server.
 *
 *-----------------------------------------------------------------------------
 */

void
DragDetWnd::ApplyDetWndMode(void)
{
   Display *display;
   Window xwindow;

   if (!GetXWindow(&display, &xwindow)) {
      g_debug("%s: no X11 surface; detection window may steal focus\n",
              __FUNCTION__);
      return;
   }

   GdkDisplay *gdkDisplay = gdk_display_get_default();
   XSetWindowAttributes attrs;

   gdk_x11_display_error_trap_push(gdkDisplay);

   attrs.override_redirect = False;
   XChangeWindowAttributes(display, xwindow, CWOverrideRedirect, &attrs);

   /*
    * Set fixed size hints, the ICCCM way to say that a window cannot usefully
    * be resized. A tiling compositor such as sway then floats the window
    * rather than fitting it into the layout. A tiled 124x124 detection
    * window is stretched over the whole output, covers the drop target and
    * takes the drop.
    */
   {
      XSizeHints *hints = XAllocSizeHints();

      if (hints != NULL) {
         hints->flags = PMinSize | PMaxSize;
         /*
          * Also the position SetGeometry() asked for. Without it the window
          * manager places a newly mapped window itself, and on the first
          * map it ends up somewhere other than where the move asked.
          */
         if (mPlaced) {
            hints->flags |= USPosition | PPosition;
            hints->x = mPlacedX;
            hints->y = mPlacedY;
         }
         hints->min_width = hints->max_width = DRAG_DET_WINDOW_WIDTH * 2;
         hints->min_height = hints->max_height = DRAG_DET_WINDOW_WIDTH * 2;
         XSetWMNormalHints(display, xwindow, hints);
         XFree(hints);
      }
   }

   if (mUnderWayland) {
      Atom wmType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
      Atom wmTypeDnd = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DND", False);
      XChangeProperty(display, xwindow, wmType, XA_ATOM, 32, PropModeReplace,
                      (unsigned char *) &wmTypeDnd, 1);

      Atom userTime = XInternAtom(display, "_NET_WM_USER_TIME", False);
      unsigned long zero = 0;
      XChangeProperty(display, xwindow, userTime, XA_CARDINAL, 32,
                      PropModeReplace, (unsigned char *) &zero, 1);
   }

   {
      Atom skipTaskbar =
         XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
      Atom skipPager = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
      Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
      Atom states[2];

      states[0] = skipTaskbar;
      states[1] = skipPager;
      XChangeProperty(display, xwindow, wmState, XA_ATOM, 32, PropModeReplace,
                      (unsigned char *) states, 2);

      /*
       * The window manager reads this property only while the window is
       * unmapped, and GTK rewrites it when it maps the window.
       * RequestSkipTaskbar() requests the state again with a client message
       * once the window is managed.
       */
   }

   if (mUnderWayland) {
      /*
       * Under ICCCM 4.1.7, input False tells the window manager not to give
       * this window the keyboard focus. Where WM_PROTOCOLS has no
       * WM_TAKE_FOCUS, it also makes KWin's wantsInput() false, so the
       * requestFocus() that KWin's drag filter issues for the window under
       * the cursor cannot activate it.
       */
      XWMHints *hints = XGetWMHints(display, xwindow);
      XWMHints fresh;

      if (hints == NULL) {
         memset(&fresh, 0, sizeof fresh);
         hints = &fresh;
      }
      hints->flags |= InputHint;
      hints->input = False;
      XSetWMHints(display, xwindow, hints);
      if (hints != &fresh) {
         XFree(hints);
      }
   }

   XFlush(display);

   gdk_x11_display_error_trap_pop_ignored(gdkDisplay);
}


/**
 * Lower the window.
 */

void
DragDetWnd::Lower(void)
{
   lower_wnd(mWnd);
   Flush();
}


/**
 *
 * Get the width of the screen associated with this window.
 *
 * @return width of screen, in pixels.
 */

int
DragDetWnd::GetScreenWidth(void)
{
   Gdk::Rectangle geometry;
   Glib::RefPtr<Gdk::Monitor> monitor = get_monitor(mWnd);
   monitor->get_geometry(geometry);
   return geometry.get_width();
}


/**
 *
 * Get the height of the screen associated with this window.
 *
 * @return height of screen, in pixels.
 */

int
DragDetWnd::GetScreenHeight(void)
{
   Gdk::Rectangle geometry;
   Glib::RefPtr<Gdk::Monitor> monitor = get_monitor(mWnd);
   monitor->get_geometry(geometry);
   return geometry.get_height();
}


/**
 *
 * Set the geometry of the window.
 *
 * @param[in] x desired x-coordinate of the window.
 * @param[in] y desired y-coordinate of the window.
 * @param[in] width desired width of the window.
 * @param[in] height desired height of the window.
 */

void
DragDetWnd::SetGeometry(const int x,
                        const int y,
                        const int width,
                        const int height)
{
   GtkWidget* widget = GetWnd()->gobj();
   GtkNative* native = gtk_widget_get_native(widget);
   Gtk::Window* gdkwin = dynamic_cast<Gtk::Window*>(GetWnd());
   GdkSurface* surface = gtk_native_get_surface(native);
   g_debug("%s: set Geo at (%d, %d, %d, %d)\n", __FUNCTION__, x, y, width, height);

   if (gdkwin && surface) {
      gdkwin->set_default_size(width, height);
      if (GDK_IS_X11_SURFACE(surface)) {
         Display* display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
         Window xwindow = gdk_x11_surface_get_xid(surface);
         XSizeHints *hints = XAllocSizeHints();

         mPlaced = true;
         mPlacedX = x;
         mPlacedY = y;
         /*
          * The next map honours this position. ApplyDetWndMode() explains
          * why.
          */
         if (hints != NULL) {
            long supplied = 0;

            if (!XGetWMNormalHints(display, xwindow, hints, &supplied)) {
               hints->flags = 0;
            }
            hints->flags |= USPosition | PPosition;
            hints->x = x;
            hints->y = y;
            XSetWMNormalHints(display, xwindow, hints);
            XFree(hints);
         }
         XMoveWindow(display, xwindow, x, y);
         XFlush(display);
      }
      Flush();
   }
}


/**
 *
 * Get the current geometry of the window.
 *
 * @param[out] x current x-coordinate of the window.
 * @param[out] y current y-coordinate of the window.
 * @param[out] width current width of the window.
 * @param[out] height current height of the window.
 *
 * @note The current geometry may be inaccurate if retrieved too quickly
 * after a change made by SetGeometry(). This is due to the realities of
 * X and window managers. Some of this is mitigated by the use of flush()
 * and sync() calls in SetGeometry(), but these are no guarantee.
 */

void
DragDetWnd::GetGeometry(int &x, int &y, int &width, int &height)
{
   GtkWidget* widget = GetWnd()->gobj();
   GtkNative* native = gtk_widget_get_native(widget);
   GdkSurface* surface = gtk_native_get_surface(native);

   width = gdk_surface_get_width(surface);
   height = gdk_surface_get_height(surface);

   if (GDK_IS_X11_SURFACE(surface)) {
      Display* display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
      Window xwindow = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surface));

      Window root, child;
      int dx, dy;
      unsigned int w, h, bw, depth;

      if (XGetGeometry(display, xwindow, &root, &dx, &dy, &w, &h, &bw, &depth)) {
         XTranslateCoordinates(display, xwindow, root, 0, 0, &x, &y, &child);
      }
   }
}


/**
 *
 *  Show or hide detect window and move it to the positon.
 *
 */

void
DragDetWnd::UpdateDetWnd(bool show, int32 x, int32 y, bool asDropTarget,
                         bool above)
{
  g_debug("%s: enter 0x%lx show %d x %d y %d\n",
         __FUNCTION__,
         (unsigned long) Get_Current_Drop_ctx(), show, x, y);

   /* A show in any role, or a hide, ends the moves of an earlier show. */
   CancelDropTargetNudges();

   /* If the window is being shown, move it to the right place. */
   if (show) {
      /* Before the map as well, so that the map itself uses the position. */
      SetGeometry(x, y, DRAG_DET_WINDOW_WIDTH * 2, DRAG_DET_WINDOW_WIDTH * 2);
      Show();
      Raise();
      SetGeometry(x, y, DRAG_DET_WINDOW_WIDTH * 2, DRAG_DET_WINDOW_WIDTH * 2);

      /*
       * Under Wayland, wait for the window manager to adopt the window only
       * now, with the window at its final size and position. The mouse move
       * below is the only motion the compositor will see, and it must come
       * after that. Under Xorg the drag source looks up the window under the
       * pointer in the X server on each motion, and the window needs nothing
       * from the window manager but the map.
       *
       * Wait only when the window has to receive a drag. The wait takes
       * 100ms or more, up to DET_WND_MANAGE_TIMEOUT_MS, and a drag source
       * does not have to be found. During a host to guest drop the window is
       * the drag source, and the delay there loses the drop target, so the
       * drag ends in GDK_DRAG_CANCEL_NO_TARGET.
       */
      int moveX = x + 2;
      int moveY = y + 2;

      if (asDropTarget && mUnderWayland) {
         int gx, gy, gw, gh;

         WaitUntilManaged(DET_WND_MANAGE_TIMEOUT_MS);
         /*
          * Mutter maps a window that does not take focus below the focused
          * window. A drag leaving the guest usually starts in the focused
          * window, so that window would get the drag instead. The caller
          * asks for this only under Mutter. See OnDropTargetNudge() for the
          * rest.
          */
         if (above) {
            SetAbove(true);
         }
         /*
          * A tiling compositor such as sway floats the window where it
          * chooses, in the middle of the output, not at the edge the pointer
          * left by. Move the pointer to where the window is.
          */
         GetGeometry(gx, gy, gw, gh);
         if (ABS(gx - x) > 2 || ABS(gy - y) > 2) {
            if (gw <= 1 || gh <= 1) {
               gw = gh = DRAG_DET_WINDOW_WIDTH * 2;
            }
            moveX = gx + gw / 2;
            moveY = gy + gh / 2;
            g_debug("%s: the window is at (%d, %d), not where asked\n",
                    __FUNCTION__, gx, gy);
         }
      }
      g_debug("%s: show at (%d, %d, %d, %d)\n", __FUNCTION__, x, y, DRAG_DET_WINDOW_WIDTH * 2, DRAG_DET_WINDOW_WIDTH * 2);
      /*
       * Wiggle the mouse here. Especially for G->H DnD, this improves
       * reliability of making the drag escape the guest window immensly.
       * Stolen from the legacy V2 DnD code.
       *
       * Not during a host to guest drop. There the pointer is already on the
       * target the host chose, and this window is the drag source, so moving
       * the pointer onto it takes the drag off that target moments before the
       * button is released. Under a compositor the move is real input, and
       * the target does not get re-entered before the release arrives.
       */
      if (asDropTarget) {
         SimulateMouseMove(moveX, moveY);
      }
      SetIsVisible(true);
      if (asDropTarget && mUnderWayland) {
         mNudgeX = moveX;
         mNudgeY = moveY;
         mNudgesLeft = DROP_TARGET_NUDGES;
         mNudgeTimer = g_timeout_add(DROP_TARGET_NUDGE_MS, OnDropTargetNudge,
                                     this);
      }
   } else {
      g_debug("%s: hide\n", __FUNCTION__);
      Hide();
      SetIsVisible(false);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::OnDropTargetNudge --
 *
 *    Move the mouse over the drop target again, from the main loop, a few
 *    times after UpdateDetWnd() shows it under Wayland.
 *
 *    The compositor offers a drag to the window under the pointer only when
 *    the pointer moves. Mutter, for one, does not offer it to the window on
 *    the move in UpdateDetWnd(), even after raising it, but does on the
 *    same move made from the main loop moments later. GTK draws the window's first
 *    frame from the main loop, which UpdateDetWnd() blocks until after its
 *    move, and that is the likely difference. The pointer is already on the
 *    window, so the extra moves do nothing else.
 *
 * Results:
 *    G_SOURCE_CONTINUE while nudges are left, else G_SOURCE_REMOVE.
 *
 * Side effects:
 *    Moves the mouse.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DragDetWnd::OnDropTargetNudge(gpointer data)  // IN: the DragDetWnd
{
   DragDetWnd *self = static_cast<DragDetWnd *>(data);

   if (!self->GetIsVisible() || self->mNudgesLeft <= 0) {
      self->mNudgeTimer = 0;
      return G_SOURCE_REMOVE;
   }
   self->mNudgesLeft--;
   self->SimulateMouseMove(self->mNudgeX, self->mNudgeY);
   if (self->mNudgesLeft > 0) {
      return G_SOURCE_CONTINUE;
   }
   self->mNudgeTimer = 0;
   return G_SOURCE_REMOVE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::CancelDropTargetNudges --
 *
 *    Stop the moves that UpdateDetWnd() scheduled through
 *    OnDropTargetNudge().
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Removes the nudge timer.
 *
 *-----------------------------------------------------------------------------
 */

void
DragDetWnd::CancelDropTargetNudges(void)
{
   if (mNudgeTimer != 0) {
      g_source_remove(mNudgeTimer);
      mNudgeTimer = 0;
   }
   mNudgesLeft = 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::OnXEvent --
 *
 *    Note when a drag enters the detection window and when it drops on it.
 *
 *    GTK keeps the drop after XdndDrop until it has read the value, and
 *    reports no drop until then. DnDUIX11::OnGHDropCheck() must not take a
 *    drag that has dropped for one that is still running.
 *
 * Results:
 *    FALSE, so that GDK handles the event as usual.
 *
 * Side effects:
 *    Sets mXdndDropped.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
DragDetWnd::OnXEvent(GdkX11Display *display,   // IN: unused
                     const XEvent *xevent,     // IN
                     gpointer data)            // IN: the DragDetWnd
{
   DragDetWnd *self = static_cast<DragDetWnd *>(data);
   Display *xdisplay;
   Window xwindow;

   if (xevent->type != ClientMessage) {
      return FALSE;
   }
   /* Only the first client message asks the server for the atoms. */
   if (self->mXdndEnter == None) {
      self->mXdndEnter = XInternAtom(xevent->xany.display, "XdndEnter", False);
      self->mXdndDrop = XInternAtom(xevent->xany.display, "XdndDrop", False);
   }
   if ((xevent->xclient.message_type != self->mXdndEnter &&
        xevent->xclient.message_type != self->mXdndDrop) ||
       !self->GetXWindow(&xdisplay, &xwindow) ||
       xevent->xclient.window != xwindow) {
      return FALSE;
   }
   self->mXdndDropped = xevent->xclient.message_type == self->mXdndDrop;
   if (self->mXdndDropped) {
      g_debug("%s: XdndDrop on the detection window\n", __FUNCTION__);
   }

   return FALSE;
}


/**
 *  Start drag from drag source with current coordinate.
 */

void
DragDetWnd::StartDrag(Glib::RefPtr<Gdk::ContentProvider> provider,
                      int mouseX,
                      int mouseY)
{
   g_debug("%s: start drag from offset (%d, %d)\n", __FUNCTION__,
           mouseX, mouseY);
   mSource->set_content(provider);
}


/**
 *  adjust the current x,y coordinate slightly.
 */

static
bool adjust_coord(int *x, int *y, int width, int height)
{
   bool change = false;
   /*
    * first do left and top edges.
    */
   if (*x <= 5){
      *x = 6;
      change = true;
   }

   if (*y <= 5){
      *y = 6;
      change = true;
   }

   /*
    * next, move result away from right and bottom edges.
    */
   if (*x > width - 5){
      *x = width - 6;
      change = true;
   }

   if (*y > height - 5){
      *y = height - 6;
      change = true;
   }
   return change;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TryXTestFakeDeviceButtonEvent --
 *
 *      Fake X mouse events in device level.
 *
 *      XXX The function will only be called if XTestFakeButtonEvent does
 *      not work for mouse button release. Later on we may only call this
 *      one for mouse button simulation if this is more reliable.
 *
 * Results:
 *      Returns true on success, false on failure.
 *
 * Side effects:
 *      Generates mouse events.
 *
 *-----------------------------------------------------------------------------
 */

static
bool TryXTestSimulateDeviceButtonEvent(GdkSurface* surface)
{
   XDeviceInfo *list = NULL;
   XDeviceInfo *list2 = NULL;
   XDevice *tdev = NULL;
   XDevice *buttonDevice = NULL;
   int numDevices = 0;
   int i = 0;
   int j = 0;
   XInputClassInfo *ip = NULL;
   Display *dndXDisplay;

   if (!surface) {
      g_debug("%s: unable to get widget\n", __FUNCTION__);
      return false;
   }

   dndXDisplay = GDK_SURFACE_XDISPLAY(surface);

   /* First get list of all input device. */
   if (!(list = XListInputDevices (dndXDisplay, &numDevices))) {
      g_debug("%s: XListInputDevices failed\n", __FUNCTION__);
      return false;
   } else {
      g_debug("%s: XListInputDevices got %d devices\n", __FUNCTION__, numDevices);
   }

   list2 = list;

   for (i = 0; i < numDevices; i++, list++) {
      /* We only care about mouse device. */
      if (list->use != IsXExtensionPointer) {
         continue;
      }

      tdev = XOpenDevice(dndXDisplay, list->id);
      if (!tdev) {
         g_debug("%s: XOpenDevice failed\n", __FUNCTION__);
         continue;
      }

      for (ip = tdev->classes, j = 0; j < tdev->num_classes; j++, ip++) {
         if (ip->input_class == ButtonClass) {
            buttonDevice = tdev;
            break;
         }
      }

      if (buttonDevice) {
         g_debug("%s: calling XTestFakeDeviceButtonEvent for %s\n",
               __FUNCTION__, list->name);
         XTestFakeDeviceButtonEvent(dndXDisplay, buttonDevice, 1, False,
                                    NULL, 0, CurrentTime);
         buttonDevice = NULL;
      }
      XCloseDevice(dndXDisplay, tdev);
   }
   XFreeDeviceList(list2);
   return true;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::SimulateXEvents --
 *
 *      Fake X mouse events and window movement for the detection window.
 *
 *      This function shows the detection window and generates button
 *      press/release and pointer motion events.
 *
 *      XXX This code should be implemented using GDK APIs.
 *          (gdk_display_warp_pointer?)
 *
 *
 * Results:
 *      Returns true if generated X events, false on failure.
 *
 * Side effects:
 *      A ton of things.
 *
 *-----------------------------------------------------------------------------
 */

bool
DragDetWnd::SimulateXEvents(const bool showWidget, const bool buttonEvent,
                            const bool buttonPress, const bool moveWindow,
                            const bool coordsProvided,
                            const int xCoord, const int yCoord)
{
   Display *dndXDisplay;
   Window rootWnd;
   Window dndXWindow;
   Window rootReturn;
   Window childReturn;
   GdkSurface* surface;

   bool ret = false;
   int x, y;
   int rootXReturn, rootYReturn;
   int winXReturn, winYReturn;
   unsigned int maskReturn;

   TRACE_CALL();

   x = xCoord;
   y = yCoord;

   surface = gtk_native_get_surface(mWnd->get_native()->gobj());
   dndXDisplay = GDK_SURFACE_XDISPLAY(surface);
   dndXWindow = GDK_SURFACE_XID(surface);
   rootWnd = RootWindow(dndXDisplay, DefaultScreen(dndXDisplay));

   /*
    * Turn on X synchronization in order to ensure that our X events occur in
    * the order called.  In particular, we want the window movement to occur
    * before the mouse movement so that the events we are coercing do in fact
    * happen.
    */
   XSynchronize(dndXDisplay, True);

   if (showWidget) {
      g_debug("%s: showing Gtk widget\n", __FUNCTION__);
      gtk_widget_set_visible(GTK_WIDGET(mWnd->gobj()), true);
      gtk_window_present(mWnd->gobj());

   }

   /* Get the current location of the mouse if coordinates weren't provided. */
   if (!coordsProvided) {
      if (!XQueryPointer(dndXDisplay, rootWnd, &rootReturn, &childReturn,
                          &rootXReturn, &rootYReturn, &winXReturn, &winYReturn,
                          &maskReturn)) {
         Warning("%s: XQueryPointer() returned False.\n", __FUNCTION__);
         goto exit;
      }

      g_debug("%s: current mouse is at (%d, %d)\n", __FUNCTION__,
            rootXReturn, rootYReturn);

      /*
       * Position away from the edge of the window.
       */
      int width = GetScreenWidth();
      int height = GetScreenHeight();

      x = rootXReturn;
      y = rootYReturn;

      if (adjust_coord(&x, &y, width, height)) {
         g_debug("%s: adjusting mouse position. root %d, %d, adjusted %d, %d\n",
               __FUNCTION__, rootXReturn, rootYReturn, x, y);
      }
   }

   if (moveWindow) {
      /*
       * Make sure the window is at this point and at the top (raised).  The
       * window is resized to be a bit larger than we would like to increase
       * the likelihood that mouse events are attributed to our window -- this
       * is okay since the window is invisible and hidden on cancels and DnD
       * finish.
       */
      XMoveResizeWindow(dndXDisplay,
                        dndXWindow,
                        x - DRAG_DET_WINDOW_WIDTH / 2 ,
                        y - DRAG_DET_WINDOW_WIDTH / 2,
                        DRAG_DET_WINDOW_WIDTH,
                        DRAG_DET_WINDOW_WIDTH);
      XRaiseWindow(dndXDisplay, dndXWindow);
      g_debug("%s: move wnd to (%d, %d, %d, %d)\n",
              __FUNCTION__,
              x - DRAG_DET_WINDOW_WIDTH / 2 ,
              y - DRAG_DET_WINDOW_WIDTH / 2,
              DRAG_DET_WINDOW_WIDTH,
              DRAG_DET_WINDOW_WIDTH);
   }

   /*
    * Generate mouse movements over the window.  The second one makes ungrabs
    * happen more reliably on KDE, but isn't necessary on GNOME.
    *
    * The coordinates may have come straight from the host. FakeMouse_Move()
    * keeps them within the range of the uinput device, and the X server
    * keeps a warp or an XTest motion on the screen.
    */
   if (mUseUInput) {
#ifdef USE_UINPUT
       FakeMouse_Move(x, y);
       FakeMouse_Move(x + 1, y + 1);
       g_debug("%s: Uinput simulating moving mouse\n", __FUNCTION__);

       /*
        * uinput moves the compositor's pointer, not the X server's copy of
        * it. Under Xwayland an X client learns the real position only while
        * the pointer is over one of its own surfaces, so XQueryPointer goes on
        * reporting the last position it saw. XDND picks its target from the
        * pointer position, so the drag source looks in the wrong place and the
        * detection window is never offered the drag.
        *
        * Warp the X pointer to the same coordinates so both agree.
        *
        * A host to guest XDND drag needs the opposite. Xwayland carries out
        * a warp from the client under the pointer by locking the Wayland
        * pointer, and Mutter's pointer then stays on the detection window
        * while only Xwayland's moves. Mutter bridges an X11 drag to Wayland
        * clients by following its own pointer, so the drag never reaches
        * one. The caller turns the warp off for that drag.
        */
       if (mWarpXPointer) {
          XWarpPointer(dndXDisplay, None, DefaultRootWindow(dndXDisplay),
                       0, 0, 0, 0, x + 1, y + 1);
       }
#endif
   } else {
       XTestFakeMotionEvent(dndXDisplay, -1, x, y, CurrentTime);
       XTestFakeMotionEvent(dndXDisplay, -1, x + 1, y + 1, CurrentTime);
   }

   g_debug("%s: move mouse to (%d, %d) and (%d, %d)\n", __FUNCTION__, x, y, x + 1, y + 1);

   if (buttonEvent)
   {
      g_debug("%s: faking left mouse button %s\n", __FUNCTION__,
              buttonPress ? "press" : "release");
      if (mUseUInput) {
#ifdef USE_UINPUT
         ret = FakeMouse_Click(buttonPress);
         g_debug("%s: Uinput simulating click mouse with ret %d\n", __FUNCTION__, ret);
#endif
      }
      else {
         XTestFakeButtonEvent(dndXDisplay, 1, buttonPress, CurrentTime);
         XSync(dndXDisplay, False);

         if (!buttonPress) {
            /*
             * The button release simulation may be failed with some distributions
             * like Ubuntu 10.4 and RHEL 6 for guest->host DnD. So first query
             * mouse button status. If some button is still down, we will try
             * mouse device level event simulation. For details please refer
             * to bug 552807.
             */
            if (!XQueryPointer(dndXDisplay, rootWnd, &rootReturn, &childReturn,
                               &rootXReturn, &rootYReturn, &winXReturn,
                               &winYReturn, &maskReturn)) {
               Warning("%s: XQueryPointer returned False.\n", __FUNCTION__);
               goto exit;
            }

            if ((maskReturn & Button1Mask)
                 || (maskReturn & Button2Mask)
                 || (maskReturn & Button3Mask)
                 || (maskReturn & Button4Mask)
                 || (maskReturn & Button5Mask)) {
               Debug("%s: XTestFakeButtonEvent was not working for button "
                     "release, trying XTestFakeDeviceButtonEvent now.\n",
                     __FUNCTION__);
               ret = TryXTestSimulateDeviceButtonEvent(surface);
            }
            else {
               g_debug("%s: XTestFakeButtonEvent was working for button release.\n",
                       __FUNCTION__);
               ret = true;
            }
         } else {
            ret = true;
         }
      }
   }

exit:
   XSynchronize(dndXDisplay, False);
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DragDetWnd::SimulateMouseMove --
 *
 *      Issue a fake mouse move event to the detection window.
 *
 * Results:
 *      Returns true on success, false on failure.
 *
 * Side effects:
 *      Generates mouse events.
 *
 *-----------------------------------------------------------------------------
 */

bool
DragDetWnd::SimulateMouseMove(const int x,        // IN: x-coord
                              const int y)        // IN: y-coord
{
   return SimulateXEvents(false, false, false, false, true, x, y);
}
