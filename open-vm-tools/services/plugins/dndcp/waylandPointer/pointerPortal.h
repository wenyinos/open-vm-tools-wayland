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
 * @file pointerPortal.h
 *
 *    Pointer integration under Wayland, through the RemoteDesktop portal and
 *    libei.
 *
 *    A Wayland client cannot move the pointer or read its position: the
 *    protocol gives it no way to, and the compositor owns both. The desktop
 *    exposes them through the RemoteDesktop portal instead, which hands the
 *    client an EIS socket that libei drives. That is the interface intended
 *    for this, and it is shared by every compositor that implements the
 *    portal, so one code path covers all of them.
 *
 *    The portal asks the user to consent the first time, and the grant is
 *    remembered per application afterwards.
 */

#ifndef _POINTER_PORTAL_H_
#define _POINTER_PORTAL_H_

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The application id the plugin registers with the portal.
 *
 * The portal attributes a grant to this id, which is what lets the desktop
 * remember a decision instead of asking at every login, and lets the user
 * find and change it in the desktop's permission settings. It is deliberately
 * not the id of any VMware product: the plugin is a separate application and
 * its permissions are its own.
 */
#define POINTER_PORTAL_APP_ID "org.openvm.tools"

/*
 * Start a RemoteDesktop session that may move the pointer, and connect libei
 * to it. Returns FALSE when there is no session bus, no portal, no
 * RemoteDesktop interface, libei rejects the connection, or the user declined.
 *
 * The portal asks for consent on the first call, so this can block until the
 * user answers. A caller that cannot wait should call it from an idle
 * callback.
 */
gboolean PointerPortal_Init(void);
void PointerPortal_Exit(void);
gboolean PointerPortal_IsAvailable(void);

/*
 * Whether the pointer device is ready to take events. The device appears
 * asynchronously after Init(), and events sent before it is resumed are
 * dropped by the compositor.
 */
gboolean PointerPortal_IsReady(void);

/*
 * The bounds of the region the pointer moves in, in the same coordinate space
 * MoveAbsolute() takes. Returns FALSE before the device is ready.
 */
gboolean PointerPortal_GetBounds(double *width, double *height);

/*
 * Move the pointer. Coordinates are absolute, in the space reported by
 * PointerPortal_GetBounds(). Events outside it are silently dropped by the
 * compositor.
 */
gboolean PointerPortal_MoveAbsolute(double x, double y);

/* Press (down TRUE) or release the left button. */
gboolean PointerPortal_Button(gboolean down);

#ifdef __cplusplus
}
#endif

#endif /* _POINTER_PORTAL_H_ */
