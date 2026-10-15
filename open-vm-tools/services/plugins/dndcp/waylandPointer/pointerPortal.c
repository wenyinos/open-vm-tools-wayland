/*********************************************************
 * Copyright (C) 2026 Broadcom. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Lesser GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/**
 * @file pointerPortal.c
 *
 *    Pointer integration under Wayland, through the RemoteDesktop portal and
 *    libei.
 *
 *    The portal is a D-Bus interface. Bringing a session up takes four calls:
 *
 *      1. CreateSession   - a session object, and its handle comes back in
 *                           the Response signal of the returned request.
 *      2. SelectDevices   - ask for the pointer.
 *      3. Start           - the user consents here, once per application.
 *      4. ConnectToEIS    - a socket the caller drives with libei.
 *
 *    Consent is interactive, so this runs the calls asynchronously from the
 *    caller's main loop rather than blocking it. The session lives in this
 *    process: the portal destroys it when the D-Bus connection goes away, so
 *    the descriptor and the libei context are kept together with the bus.
 *
 *    Once libei reports the pointer device and the compositor resumes it,
 *    events may be sent. Until then they are dropped, which is why
 *    PointerPortal_IsReady() exists.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define G_LOG_DOMAIN "dndcp"

#include "pointerPortal.h"

#include <libei.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>

#define PORTAL_BUS_NAME     "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_REGISTRY_IF  "org.freedesktop.host.portal.Registry"
#define PORTAL_REQUEST_IF   "org.freedesktop.portal.Request"
#define PORTAL_RD_IF        "org.freedesktop.portal.RemoteDesktop"

/* Bit 1 of the device types is the pointer; see the portal documentation. */
#define PORTAL_DEVICE_POINTER 2

/*
 * Ask the portal to remember the grant instead of asking again next time.
 * This is XdpSessionPersistenceMode 2, PERSISTENT.
 */
#define PORTAL_PERSIST_PERSISTENT 2

/* Linux input code for the primary button. */
#define BTN_LEFT 0x110

typedef enum {
   PORTAL_STATE_IDLE = 0,
   PORTAL_STATE_CREATING,
   PORTAL_STATE_SELECTING,
   PORTAL_STATE_STARTING,
   PORTAL_STATE_CONNECTING,
   PORTAL_STATE_READY,
   PORTAL_STATE_FAILED,
} PortalState;

static struct {
   GDBusConnection *bus;
   GCancellable *cancel;
   PortalState state;
   gboolean inited;

   gchar *session;              /* Portal session handle. */
   gchar *request;              /* Request handle of the call in flight. */
   guint signalSub;             /* Subscription for Response signals. */

   /*
    * The handle token is part of the request path the portal will use, so it
    * has to be chosen before the call and be unique per call.
    */
   guint tokenCount;
   gchar *token;                /* Token of the call in flight. */

   /*
    * Token from a previous persistent grant, if the desktop gave one. Passing
    * it back to SelectDevices restores that grant without a dialog.
    */
   gchar *restoreToken;
   gchar *restoreTokenPath;

   int eisFd;
   struct ei *ei;
   struct ei_device *device;
   gboolean deviceReady;        /* Device resumed; events may be sent now. */
   guint eiWatch;               /* GLib watch on the libei descriptor. */
} gPortal;


/*
 *-----------------------------------------------------------------------------
 *
 * PortalReset --
 *
 *    Drop everything a failed or finished attempt left behind, keeping the bus
 *    connection so a later attempt can reuse it.
 *
 *-----------------------------------------------------------------------------
 */

static void
PortalReset(void)
{
   if (gPortal.signalSub != 0) {
      g_dbus_connection_signal_unsubscribe(gPortal.bus, gPortal.signalSub);
      gPortal.signalSub = 0;
   }
   if (gPortal.eiWatch != 0) {
      g_source_remove(gPortal.eiWatch);
      gPortal.eiWatch = 0;
   }
   if (gPortal.ei != NULL) {
      ei_unref(gPortal.ei);
      gPortal.ei = NULL;
   }
   gPortal.device = NULL;
   gPortal.deviceReady = FALSE;
   if (gPortal.eisFd >= 0) {
      /* ei_setup_backend_fd() takes ownership once it succeeds. */
      if (gPortal.ei == NULL) {
         close(gPortal.eisFd);
      }
      gPortal.eisFd = -1;
   }
   g_clear_pointer(&gPortal.session, g_free);
   g_clear_pointer(&gPortal.request, g_free);
   g_clear_pointer(&gPortal.token, g_free);
}


/*
 *-----------------------------------------------------------------------------
 *
 * PortalFailed --
 *
 *    Mark the attempt failed and release what it took, so callers see
 *    IsAvailable() as FALSE and a later Init() can try again.
 *
 *-----------------------------------------------------------------------------
 */

static void
PortalFailed(const char *why)  // IN
{
   if (gPortal.state != PORTAL_STATE_FAILED) {
      g_message("%s: %s\n", __FUNCTION__, why);
   }
   PortalReset();
   gPortal.state = PORTAL_STATE_FAILED;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PointerReadable --
 *
 *    libei has events. Dispatch them and remember the device.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
PointerReadable(gint fd,             // IN: unused
                GIOCondition cond,   // IN
                gpointer data)       // IN: unused
{
   if ((cond & (G_IO_HUP | G_IO_ERR)) != 0) {
      PortalFailed("the EIS connection went away");
      return G_SOURCE_REMOVE;
   }

   /*
    * ei_dispatch() returns void and reports a broken connection as an
    * EI_EVENT_DISCONNECT event, which the loop below handles.
    */
   ei_dispatch(gPortal.ei);

   for (;;) {
      struct ei_event *event = ei_get_event(gPortal.ei);

      if (event == NULL) {
         break;
      }
      switch (ei_event_get_type(event)) {
      case EI_EVENT_SEAT_ADDED: {
         struct ei_seat *seat = ei_event_get_seat(event);

         /*
          * Buttons too: a drag holds the primary button down while it moves
          * the pointer, and the detection window watches for both.
          */
         ei_seat_bind_capabilities(seat,
                                   EI_DEVICE_CAP_POINTER,
                                   EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                   EI_DEVICE_CAP_BUTTON,
                                   NULL);
         break;
      }
      case EI_EVENT_DEVICE_ADDED:
         /*
          * Keep the first device that can move absolutely; the compositor
          * may add more than one.
          */
         if (gPortal.device == NULL &&
             ei_device_has_capability(ei_event_get_device(event),
                                      EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
            gPortal.device = ei_event_get_device(event);
            g_debug("%s: pointer device %s\n", __FUNCTION__,
                    ei_device_get_name(gPortal.device));
         }
         break;
      case EI_EVENT_DEVICE_RESUMED:
         if (gPortal.device != NULL &&
             ei_event_get_device(event) == gPortal.device) {
            ei_device_start_emulating(gPortal.device, 1);
            gPortal.deviceReady = TRUE;
            g_debug("%s: the pointer device is ready\n", __FUNCTION__);
         }
         break;
      case EI_EVENT_DEVICE_PAUSED:
         if (gPortal.device != NULL &&
             ei_event_get_device(event) == gPortal.device) {
            gPortal.deviceReady = FALSE;
         }
         break;
      case EI_EVENT_DISCONNECT:
         ei_event_unref(event);
         PortalFailed("the compositor disconnected the EIS session");
         return G_SOURCE_REMOVE;
      default:
         break;
      }
      ei_event_unref(event);
   }

   return G_SOURCE_CONTINUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PointerConnectEis --
 *
 *    Final step: ask for the EIS socket and hand it to libei.
 *
 *-----------------------------------------------------------------------------
 */

static void
PointerConnectEis(void)
{
   GUnixFDList *outList = NULL;
   GVariant *reply;
   GError *err = NULL;
   gint32 index;
   int fd;

   reply = g_dbus_connection_call_with_unix_fd_list_sync(
      gPortal.bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_RD_IF,
      "ConnectToEIS",
      g_variant_new("(o@a{sv})", gPortal.session,
                    g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &outList,
      gPortal.cancel, &err);

   if (reply == NULL) {
      PortalFailed(err != NULL ? err->message : "ConnectToEIS failed");
      g_clear_error(&err);
      return;
   }

   g_variant_get(reply, "(h)", &index);
   g_variant_unref(reply);

   if (outList == NULL) {
      PortalFailed("ConnectToEIS returned no descriptor list");
      return;
   }

   fd = g_unix_fd_list_get(outList, index, &err);
   g_object_unref(outList);
   if (fd < 0) {
      PortalFailed(err != NULL ? err->message : "no EIS descriptor");
      g_clear_error(&err);
      return;
   }

   gPortal.eisFd = fd;
   gPortal.ei = ei_new_sender(NULL);
   if (gPortal.ei == NULL) {
      PortalFailed("libei context could not be created");
      return;
   }
   ei_configure_name(gPortal.ei, "open-vm-tools");

   /*
    * From here libei owns the descriptor, so clear our copy first: a failure
    * past this point must not close it twice.
    */
   if (ei_setup_backend_fd(gPortal.ei, fd) != 0) {
      gPortal.eisFd = -1;
      PortalFailed("libei rejected the EIS descriptor");
      return;
   }
   gPortal.eisFd = -1;

   gPortal.eiWatch = g_unix_fd_add(ei_get_fd(gPortal.ei),
                                   G_IO_IN | G_IO_HUP | G_IO_ERR,
                                   PointerReadable, NULL);

   gPortal.state = PORTAL_STATE_READY;
   g_debug("%s: EIS session established\n", __FUNCTION__);
}


/*
 *-----------------------------------------------------------------------------
 *
 * PortalOptionsToken --
 *
 *    Build the a{sv} options for the next portal call, carrying the handle
 *    token the portal will use in the request path.
 *
 *    The options are built explicitly rather than through
 *    g_variant_new_parsed(), whose text format cannot express a bare string
 *    token without quotes.
 *
 * Results:
 *    A floating GVariant of type a{sv}.
 *
 *-----------------------------------------------------------------------------
 */

static GVariant *
PortalOptionsToken(const gchar *token)  // IN
{
   GVariantBuilder builder;

   g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
   g_variant_builder_add(&builder, "{sv}", "handle_token",
                         g_variant_new_string(token));

   return g_variant_builder_end(&builder);
}


/*
 *-----------------------------------------------------------------------------
 *
 * PortalNextRequest --
 *
 *    Move to the request the next step will create: pick its token and build
 *    the request object path the portal will answer on.
 *
 *    The portal names a request after the caller's bus name in which the dots
 *    are replaced by underscores, so the path can be worked out before the
 *    call is made, which is what lets this catch the Response.
 *
 *-----------------------------------------------------------------------------
 */

static void
PortalNextRequest(const char *prefix)  // IN
{
   g_free(gPortal.request);
   g_free(gPortal.token);

   gPortal.token = g_strdup_printf("%s%u", prefix, ++gPortal.tokenCount);
   gPortal.request = g_strdup_printf(
      "/org/freedesktop/portal/desktop/request/%s/%s",
      (const char *) g_object_get_data(G_OBJECT(gPortal.bus), "sender"),
      gPortal.token);
}


/*
 *-----------------------------------------------------------------------------
 *
 * PortalResponse --
 *
 *    One portal call answered. Move to the next step, or hand over to libei
 *    once Start is granted.
 *
 *-----------------------------------------------------------------------------
 */

static void
PortalResponse(GDBusConnection *conn,      // IN: unused
               const gchar *sender,        // IN: unused
               const gchar *path,          // IN
               const gchar *iface,         // IN: unused
               const gchar *signal,        // IN: unused
               GVariant *params,           // IN
               gpointer data)              // IN: unused
{
   guint32 code;
   GVariant *results = NULL;

   /* Only the request this step is waiting on matters. */
   if (g_strcmp0(path, gPortal.request) != 0) {
      return;
   }

   {
      GVariantIter iter;
      GVariant *value;

      g_variant_iter_init(&iter, params);
      if (g_variant_iter_next(&iter, "u", &code)) {
         if (g_variant_iter_next(&iter, "@a{sv}", &results)) {
            /* Held for the session handle below. */
         } else {
            results = NULL;
         }
      }
      (void) value;
   }

   if (code != 0) {
      /* 1 = the user cancelled, 2 = the request ended some other way. */
      PortalFailed(code == 1 ? "the user declined the pointer request"
                             : "the portal refused the pointer request");
      if (results != NULL) {
         g_variant_unref(results);
      }
      return;
   }

   switch (gPortal.state) {
   case PORTAL_STATE_CREATING: {
      GVariant *handle = results != NULL ?
         g_variant_lookup_value(results, "session_handle",
                                G_VARIANT_TYPE_STRING) : NULL;

      if (handle == NULL) {
         PortalFailed("the portal returned no session handle");
         break;
      }
      g_free(gPortal.session);
      gPortal.session = g_variant_dup_string(handle, NULL);
      g_variant_unref(handle);

      /* Ask for the pointer. */
      PortalNextRequest("ovtsel");
      gPortal.state = PORTAL_STATE_SELECTING;

      {
         GVariantBuilder opts;

         g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
         g_variant_builder_add(&opts, "{sv}", "types",
                               g_variant_new_uint32(PORTAL_DEVICE_POINTER));
         /*
          * Ask for the grant to be remembered. The desktop then stores it
          * against POINTER_PORTAL_APP_ID and, where it supports it, returns a
          * restore token in the Start results for a later session to reuse.
          */
         g_variant_builder_add(&opts, "{sv}", "persist_mode",
                               g_variant_new_uint32(PORTAL_PERSIST_PERSISTENT));
         if (gPortal.restoreToken != NULL) {
            g_variant_builder_add(&opts, "{sv}", "restore_token",
                                  g_variant_new_string(gPortal.restoreToken));
         }
         g_variant_builder_add(&opts, "{sv}", "handle_token",
                               g_variant_new_string(gPortal.token));

         g_dbus_connection_call(
            gPortal.bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_RD_IF,
            "SelectDevices",
            g_variant_new("(o@a{sv})", gPortal.session,
                          g_variant_builder_end(&opts)),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, gPortal.cancel,
            NULL, NULL);
      }
      break;
   }
   case PORTAL_STATE_SELECTING:
      /* Consent comes next; the portal puts up its dialog for this one. */
      PortalNextRequest("ovtstart");
      gPortal.state = PORTAL_STATE_STARTING;

      g_dbus_connection_call(
         gPortal.bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_RD_IF,
         "Start",
         /*
          * Start takes (session, parent_window, options). The window is empty
          * because vmusr owns no window: the portal then parents its dialog
          * itself.
          */
         g_variant_new("(os@a{sv})", gPortal.session, "",
                       PortalOptionsToken(gPortal.token)),
         G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, gPortal.cancel,
         NULL, NULL);
      break;
   case PORTAL_STATE_STARTING: {
      /*
       * A desktop that supports persistent grants returns a token here. Keep
       * it: passing it back to SelectDevices next time restores the grant
       * without a dialog.
       */
      GVariant *tokenValue = results != NULL
         ? g_variant_lookup_value(results, "restore_token",
                                  G_VARIANT_TYPE_STRING) : NULL;

      if (tokenValue != NULL) {
         const gchar *value = g_variant_get_string(tokenValue, NULL);

         g_free(gPortal.restoreToken);
         gPortal.restoreToken = g_strdup(value);
         if (gPortal.restoreTokenPath != NULL) {
            g_file_set_contents(gPortal.restoreTokenPath, value, -1, NULL);
         }
         g_debug("%s: the desktop will remember this grant\n", __FUNCTION__);
         g_variant_unref(tokenValue);
      }

      gPortal.state = PORTAL_STATE_CONNECTING;
      PointerConnectEis();
      break;
   }
   default:
      g_debug("%s: unexpected response in state %d\n", __FUNCTION__,
              gPortal.state);
      break;
   }

   if (results != NULL) {
      g_variant_unref(results);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PointerPortal_Init --
 *
 *    Create the session and start the chain of portal calls. The rest happens
 *    in PortalResponse(), on the caller's main loop.
 *
 *    Returns TRUE once the chain is started, not once it succeeded; the
 *    caller watches PointerPortal_IsReady() for that. A failure at any step
 *    leaves IsAvailable() FALSE.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
PointerPortal_Init(void)
{
   GVariant *reply;
   GError *err = NULL;
   const gchar *unique;

   if (gPortal.state == PORTAL_STATE_READY ||
       (gPortal.inited && gPortal.state != PORTAL_STATE_FAILED)) {
      return TRUE;
   }

   PortalReset();
   gPortal.state = PORTAL_STATE_IDLE;

   /*
    * Read the token a previous persistent grant left behind, if any. It is
    * stored next to the user's other state so it survives a reboot.
    */
   if (gPortal.restoreTokenPath == NULL) {
      gchar *stateDir = g_build_filename(g_get_user_state_dir(),
                                         "open-vm-tools", NULL);

      g_mkdir_with_parents(stateDir, 0700);
      gPortal.restoreTokenPath = g_build_filename(stateDir,
                                                  "portal-restore-token", NULL);
      g_free(stateDir);
   }
   if (gPortal.restoreToken == NULL) {
      gchar *contents = NULL;

      if (g_file_get_contents(gPortal.restoreTokenPath, &contents, NULL, NULL)) {
         g_strstrip(contents);
         if (*contents != '\0') {
            gPortal.restoreToken = contents;
            g_debug("%s: reusing the grant from a previous session\n",
                    __FUNCTION__);
         } else {
            g_free(contents);
         }
      }
   }

   if (gPortal.bus == NULL) {
      gPortal.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
      if (gPortal.bus == NULL) {
         g_debug("%s: no session bus: %s\n", __FUNCTION__,
                 err != NULL ? err->message : "unknown");
         g_clear_error(&err);
         return FALSE;
      }
      gPortal.cancel = g_cancellable_new();

      /*
       * Tell the portal which application this connection is, before any
       * other portal call. The portal attributes the grant to this id, so it
       * can remember the decision between sessions and the user can see and
       * change it in the desktop's permission settings. Without it the plugin
       * would be an anonymous caller, and the desktop would have nothing to
       * store a decision against.
       *
       * The id has to name an installed .desktop file, which is why the
       * plugin installs one. A failure is not fatal: the session still comes
       * up, the grant is just not remembered.
       */
      reply = g_dbus_connection_call_sync(
         gPortal.bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_REGISTRY_IF,
         "Register",
         g_variant_new("(s@a{sv})", POINTER_PORTAL_APP_ID,
                       g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
         NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
      if (reply == NULL) {
         g_debug("%s: could not register as %s: %s\n", __FUNCTION__,
                 POINTER_PORTAL_APP_ID, err != NULL ? err->message : "unknown");
         g_clear_error(&err);
      } else {
         g_variant_unref(reply);
         g_debug("%s: registered as %s\n", __FUNCTION__,
                 POINTER_PORTAL_APP_ID);
      }
   }

   /*
    * The portal names the request object after the bus name with the dots
    * replaced by underscores, so we have to know it before the call to catch
    * the Response that follows.
    */
   unique = g_dbus_connection_get_unique_name(gPortal.bus);
   if (unique != NULL) {
      gchar *sender = g_strdup(unique + 1);
      gchar *p;

      for (p = sender; *p != '\0'; p++) {
         if (*p == '.') {
            *p = '_';
         }
      }
      g_object_set_data_full(G_OBJECT(gPortal.bus), "sender", sender, g_free);
   }

   g_free(gPortal.token);
   gPortal.token = g_strdup_printf("ovtcreate%u", ++gPortal.tokenCount);
   g_free(gPortal.request);
   gPortal.request = g_strdup_printf(
      "/org/freedesktop/portal/desktop/request/%s/%s",
      (const char *) g_object_get_data(G_OBJECT(gPortal.bus), "sender"),
      gPortal.token);

   gPortal.signalSub = g_dbus_connection_signal_subscribe(
      gPortal.bus, PORTAL_BUS_NAME, PORTAL_REQUEST_IF, "Response",
      NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, PortalResponse, NULL, NULL);

   gPortal.state = PORTAL_STATE_CREATING;
   gPortal.inited = TRUE;

   {
      GVariantBuilder opts;
      GVariant *args;

      g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&opts, "{sv}", "session_handle_token",
                            g_variant_new_string("ovtsession"));
      g_variant_builder_add(&opts, "{sv}", "handle_token",
                            g_variant_new_string(gPortal.token));
      args = g_variant_new("(@a{sv})", g_variant_builder_end(&opts));

      reply = g_dbus_connection_call_sync(
         gPortal.bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_RD_IF,
         "CreateSession", args, G_VARIANT_TYPE("(o)"),
         G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
   }

   if (reply == NULL) {
      PortalFailed(err != NULL ? err->message : "CreateSession failed");
      g_clear_error(&err);
      return FALSE;
   }
   g_variant_unref(reply);

   g_debug("%s: waiting for the portal\n", __FUNCTION__);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PointerPortal_Exit --
 *
 *    Close the session down. The portal drops it when the connection goes
 *    away, so the objects only need releasing locally.
 *
 *-----------------------------------------------------------------------------
 */

void
PointerPortal_Exit(void)
{
   PortalReset();
   gPortal.state = PORTAL_STATE_IDLE;
   gPortal.inited = FALSE;
}


gboolean
PointerPortal_IsAvailable(void)
{
   return gPortal.state == PORTAL_STATE_READY ||
          gPortal.state == PORTAL_STATE_CONNECTING;
}


gboolean
PointerPortal_IsReady(void)
{
   return gPortal.state == PORTAL_STATE_READY && gPortal.deviceReady;
}


gboolean
PointerPortal_GetBounds(double *width,  // OUT
                        double *height) // OUT
{
   struct ei_region *region;

   if (!PointerPortal_IsReady() || width == NULL || height == NULL) {
      return FALSE;
   }

   /*
    * The device moves in the coordinate space of one region. On a compositor
    * with several outputs libei may describe more than one; the first covers
    * the case the plugin needs, since the guest has a single desktop.
    */
   region = ei_device_get_region(gPortal.device, 0);
   if (region == NULL) {
      return FALSE;
   }

   *width = ei_region_get_width(region);
   *height = ei_region_get_height(region);

   return TRUE;
}


gboolean
PointerPortal_MoveAbsolute(double x,  // IN
                           double y)  // IN
{
   if (!PointerPortal_IsReady()) {
      return FALSE;
   }

   ei_device_pointer_motion_absolute(gPortal.device, x, y);
   ei_device_frame(gPortal.device, ei_now(gPortal.ei));

   return TRUE;
}


gboolean
PointerPortal_Button(gboolean down)  // IN
{
   if (!PointerPortal_IsReady()) {
      return FALSE;
   }

   ei_device_button_button(gPortal.device, BTN_LEFT, down);
   ei_device_frame(gPortal.device, ei_now(gPortal.ei));

   return TRUE;
}
