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
 * @file wayland-data-control.h
 *
 *    Drive either data control protocol from one piece of code.
 *
 *    ext-data-control-v1 replaced zwlr-data-control-unstable-v1, which older
 *    KWin and wlroots releases still offer and nothing else. The two protocols
 *    have the same four interfaces, the same requests in the same order, and
 *    the same events in the same order, so one implementation can drive both
 *    once the interface pointers are chosen at run time.
 */

#ifndef _WAYLAND_DATA_CONTROL_H_
#define _WAYLAND_DATA_CONTROL_H_

#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The events of both protocols line up, so the same listener structures serve
 * for either one. The proxy argument is a wl_proxy because the concrete type
 * depends on the protocol.
 */
typedef struct {
   void (*send)(void *data, void *source, const char *mimeType, int32_t fd);
   void (*cancelled)(void *data, void *source);
} WaylandDataSourceListener;

typedef struct {
   void (*offer)(void *data, void *offer, const char *mimeType);
} WaylandDataOfferListener;

typedef struct {
   void (*data_offer)(void *data, void *device, void *offer);
   void (*selection)(void *data, void *device, void *offer);
   void (*finished)(void *data, void *device);
   void (*primary_selection)(void *data, void *device, void *offer);
} WaylandDataDeviceListener;

/*
 * One data control protocol a compositor may implement. The name is what the
 * registry advertises, the rest are the interfaces of its four objects.
 */
typedef struct {
   const char *name;
   const struct wl_interface *manager;
   const struct wl_interface *source;
   const struct wl_interface *device;
} WaylandDataControlProto;

/*
 * The protocols in the order they should be tried. The list ends with a NULL
 * name.
 */
const WaylandDataControlProto * const *WaylandDataControlProtocols(void);

/*
 * The protocol named by iface, or NULL when the registry global is not a data
 * control manager. Both protocols are always available: the code carries the
 * interface descriptions for each, so no protocol XML beyond the two is
 * needed at build time.
 */
const WaylandDataControlProto *WaylandDataControlMatch(const char *iface);

/*
 * Bind one of the protocols. version is the version the compositor
 * advertised, and is capped at what this code knows how to drive.
 */
void *WaylandDataControlBind(struct wl_registry *registry,
                             uint32_t name,
                             const WaylandDataControlProto *proto,
                            uint32_t version);

void *WaylandDataControlCreateSource(void *manager);
void *WaylandDataControlGetDevice(void *manager, struct wl_seat *seat);
void WaylandDataControlManagerDestroy(void *manager);

void WaylandDataControlSetSelection(void *device, void *source);
void WaylandDataControlDeviceDestroy(void *device);
int WaylandDataControlDeviceAddListener(void *device,
                                        const WaylandDataDeviceListener *l,
                                        void *data);

void WaylandDataSourceOffer(void *source, const char *mimeType);
void WaylandDataSourceDestroy(void *source);
int WaylandDataSourceAddListener(void *source,
                                 const WaylandDataSourceListener *l,
                                 void *data);

void WaylandDataOfferReceive(void *offer, const char *mimeType, int32_t fd);
void WaylandDataOfferDestroy(void *offer);
int WaylandDataOfferAddListener(void *offer,
                                const WaylandDataOfferListener *l,
                                void *data);
void WaylandDataOfferSetUserData(void *offer, void *userData);
void *WaylandDataOfferGetUserData(void *offer);

#ifdef __cplusplus
}
#endif

#endif /* _WAYLAND_DATA_CONTROL_H_ */
