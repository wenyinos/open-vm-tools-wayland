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
 *    Move and click the pointer through the XDG desktop portal.
 *
 *    A Wayland client cannot move the pointer itself: the protocol gives it no
 *    way to, and the compositor owns the cursor. The RemoteDesktop portal is
 *    the interface the desktop exposes for exactly this, and it is shared by
 *    every compositor that implements the portal, so one code path covers all
 *    of them.
 *
 *    The host needs two things from the guest pointer, and both are here:
 *
 *      - where the pointer is, which the host records so it can put the host
 *        cursor back in the right place when focus returns to it;
 *      - the ability to put the pointer somewhere, which is what a drag does
 *        when it repositions it over the drag detection window.
 */

#ifndef _POINTER_PORTAL_H_
#define _POINTER_PORTAL_H_

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up a RemoteDesktop session that may move the pointer and inject
 * buttons. Returns FALSE when there is no session bus, no portal, the portal
 * has no RemoteDesktop interface, or the user refused the request. The caller
 * then keeps whatever it did before.
 *
 * The call drives the portal synchronously through a nested main loop, because
 * the caller runs on the main loop and has no place to yield. The portal shows
 * a dialog on the first call, so this can block for as long as the user takes
 * to answer it.
 */
gboolean PointerPortal_Init(void);
void PointerPortal_Exit(void);
gboolean PointerPortal_IsAvailable(void);

/*
 * The pointer position the portal last reported, in the coordinate space of
 * the host's pointer, or FALSE when no position is known yet.
 *
 * The portal does not report the position on request. It sends motion events
 * once the session is up, and this keeps the latest one.
 */
gboolean PointerPortal_GetPosition(int *x, int *y);

/*
 * Whether the portal has ever delivered a pointer event. Until it has, its
 * notion of the position is not the host's, and a caller that records the
 * position for the host should wait.
 */
gboolean PointerPortal_HasPosition(void);

/*
 * Move the pointer. The portal takes absolute coordinates in its own stream's
 * coordinate space, so the caller passes the same space it reads back from
 * PointerPortal_GetPosition().
 */
gboolean PointerPortal_MoveAbsolute(double x, double y);

/* Press (down TRUE) or release the left button. */
gboolean PointerPortal_Button(gboolean down);

#ifdef __cplusplus
}
#endif

#endif /* _POINTER_PORTAL_H_ */
