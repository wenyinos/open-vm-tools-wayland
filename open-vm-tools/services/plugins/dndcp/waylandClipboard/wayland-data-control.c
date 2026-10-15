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
 * @file wayland-data-control.c
 *
 *    Drive either data control protocol from one piece of code.
 *
 *    The two protocols carry the same four interfaces, with the same
 *    requests and events in the same order, so every call but the two that
 *    create an object is plain wl_proxy_marshal_flags() on the right opcode
 *    and works whichever protocol the proxy belongs to. Only
 *    create_data_source() and get_data_device() have to name the interface of
 *    the object they create, and those are the calls that take the protocol
 *    this module recorded when it bound the manager.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define G_LOG_DOMAIN "dndcp"

#include "wayland-data-control.h"

#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

/*
 * Opcodes, from either protocol's XML. Both number their requests the same
 * way; a build without the ext protocol still needs the numbers, so they are
 * written out here rather than taken from the generated macros.
 */
#define DC_MANAGER_CREATE_DATA_SOURCE 0
#define DC_MANAGER_GET_DATA_DEVICE 1
#define DC_MANAGER_DESTROY 2
#define DC_DEVICE_SET_SELECTION 0
#define DC_DEVICE_DESTROY 1
#define DC_SOURCE_OFFER 0
#define DC_SOURCE_DESTROY 1
#define DC_OFFER_RECEIVE 0
#define DC_OFFER_DESTROY 1

static const WaylandDataControlProto gExtProto = {
   .name = "ext_data_control_manager_v1",
   .manager = &ext_data_control_manager_v1_interface,
   .source = &ext_data_control_source_v1_interface,
   .device = &ext_data_control_device_v1_interface,
};

static const WaylandDataControlProto gWlrProto = {
   .name = "zwlr_data_control_manager_v1",
   .manager = &zwlr_data_control_manager_v1_interface,
   .source = &zwlr_data_control_source_v1_interface,
   .device = &zwlr_data_control_device_v1_interface,
};

/* What the manager in use was bound as. Set by WaylandDataControlBind(). */
static const WaylandDataControlProto *gBoundProto = NULL;


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDataControlProtocols --
 *
 *    The protocols to try, in order. ext-data-control-v1 is the standard one
 *    and is tried first; a compositor that predates it offers the wlr one.
 *
 * Results:
 *    A NULL-terminated array of protocols.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

const WaylandDataControlProto * const *
WaylandDataControlProtocols(void)
{
   static const WaylandDataControlProto * const protos[] = {
      &gExtProto,
      &gWlrProto,
      NULL,
   };

   return protos;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDataControlMatch --
 *
 *    Which protocol, if either, a registry global names.
 *
 * Results:
 *    The protocol, or NULL when the global is something else.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

const WaylandDataControlProto *
WaylandDataControlMatch(const char *iface)  // IN
{
   const WaylandDataControlProto * const *protos;
   const char *forced = g_getenv("VMTOOLS_DATA_CONTROL_PROTOCOL");

   if (iface == NULL) {
      return NULL;
   }
   for (protos = WaylandDataControlProtocols(); *protos != NULL; protos++) {
      /*
       * A compositor that offers both protocols is normally driven through
       * ext-data-control-v1. The override exists for a compositor whose
       * implementation of one of them is broken, and for testing either
       * path on a compositor that offers both.
       */
      if (forced != NULL && strcmp(forced, (*protos)->name) != 0) {
         continue;
      }
      if (strcmp(iface, (*protos)->name) == 0) {
         return *protos;
      }
   }

   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDataControlBind --
 *
 *    Bind the manager for one protocol.
 *
 *    The version the compositor advertises is capped at the one the interface
 *    describes, which is what wl_registry_bind() wants.
 *
 * Results:
 *    The manager proxy, or NULL when the bind fails.
 *
 * Side effects:
 *    Records the protocol, which the object-creating calls below need.
 *
 *-----------------------------------------------------------------------------
 */

void *
WaylandDataControlBind(struct wl_registry *registry,  // IN
                       uint32_t name,                 // IN
                       const WaylandDataControlProto *proto, // IN
                       uint32_t version)              // IN
{
   uint32_t cap;
   void *manager;

   if (registry == NULL || proto == NULL) {
      return NULL;
   }
   cap = proto->manager->version;
   manager = wl_registry_bind(registry, name, proto->manager,
                              MIN(version, cap));
   if (manager != NULL) {
      gBoundProto = proto;
      g_debug("%s: bound %s v%u\n", __FUNCTION__, proto->name,
              MIN(version, cap));
   }

   return manager;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDataControlCreateSource --
 *
 *    Create a data source on the manager.
 *
 * Results:
 *    The source proxy, or NULL when no manager was bound.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

void *
WaylandDataControlCreateSource(void *manager)  // IN
{
   if (manager == NULL || gBoundProto == NULL) {
      return NULL;
   }

   return wl_proxy_marshal_flags((struct wl_proxy *) manager,
                                 DC_MANAGER_CREATE_DATA_SOURCE,
                                 gBoundProto->source,
                                 wl_proxy_get_version((struct wl_proxy *) manager),
                                 0, NULL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaylandDataControlGetDevice --
 *
 *    Create the data device for a seat on the manager.
 *
 * Results:
 *    The device proxy, or NULL when no manager was bound.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

void *
WaylandDataControlGetDevice(void *manager,       // IN
                            struct wl_seat *seat) // IN
{
   if (manager == NULL || gBoundProto == NULL) {
      return NULL;
   }

   return wl_proxy_marshal_flags((struct wl_proxy *) manager,
                                 DC_MANAGER_GET_DATA_DEVICE,
                                 gBoundProto->device,
                                 wl_proxy_get_version((struct wl_proxy *) manager),
                                 0, NULL, seat);
}


void
WaylandDataControlManagerDestroy(void *manager)  // IN
{
   if (manager != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) manager, DC_MANAGER_DESTROY,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) manager),
                             WL_MARSHAL_FLAG_DESTROY);
   }
}


void
WaylandDataControlSetSelection(void *device,  // IN
                               void *source)  // IN
{
   if (device != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) device, DC_DEVICE_SET_SELECTION,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) device),
                             0, source);
   }
}


void
WaylandDataControlDeviceDestroy(void *device)  // IN
{
   if (device != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) device, DC_DEVICE_DESTROY,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) device),
                             WL_MARSHAL_FLAG_DESTROY);
   }
}


int
WaylandDataControlDeviceAddListener(void *device,                     // IN
                                    const WaylandDataDeviceListener *l, // IN
                                    void *data)                       // IN
{
   if (device == NULL) {
      return -1;
   }

   return wl_proxy_add_listener((struct wl_proxy *) device,
                                (void (**)(void)) l, data);
}


void
WaylandDataSourceOffer(void *source,          // IN
                       const char *mimeType)  // IN
{
   if (source != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) source, DC_SOURCE_OFFER,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) source),
                             0, mimeType);
   }
}


void
WaylandDataSourceDestroy(void *source)  // IN
{
   if (source != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) source, DC_SOURCE_DESTROY,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) source),
                             WL_MARSHAL_FLAG_DESTROY);
   }
}


int
WaylandDataSourceAddListener(void *source,                        // IN
                             const WaylandDataSourceListener *l,  // IN
                             void *data)                          // IN
{
   if (source == NULL) {
      return -1;
   }

   return wl_proxy_add_listener((struct wl_proxy *) source,
                                (void (**)(void)) l, data);
}


void
WaylandDataOfferReceive(void *offer,          // IN
                        const char *mimeType, // IN
                        int32_t fd)           // IN
{
   if (offer != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) offer, DC_OFFER_RECEIVE,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) offer),
                             0, mimeType, fd);
   }
}


void
WaylandDataOfferDestroy(void *offer)  // IN
{
   if (offer != NULL) {
      wl_proxy_marshal_flags((struct wl_proxy *) offer, DC_OFFER_DESTROY,
                             NULL,
                             wl_proxy_get_version((struct wl_proxy *) offer),
                             WL_MARSHAL_FLAG_DESTROY);
   }
}


int
WaylandDataOfferAddListener(void *offer,                       // IN
                            const WaylandDataOfferListener *l, // IN
                            void *data)                        // IN
{
   if (offer == NULL) {
      return -1;
   }

   return wl_proxy_add_listener((struct wl_proxy *) offer,
                                (void (**)(void)) l, data);
}


void
WaylandDataOfferSetUserData(void *offer,     // IN
                            void *userData)  // IN
{
   if (offer != NULL) {
      wl_proxy_set_user_data((struct wl_proxy *) offer, userData);
   }
}


void *
WaylandDataOfferGetUserData(void *offer)  // IN
{
   return offer != NULL ? wl_proxy_get_user_data((struct wl_proxy *) offer)
                        : NULL;
}
