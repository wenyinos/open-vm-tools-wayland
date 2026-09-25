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

/*
 * waylandDrag.h --
 *
 *    Start a host to guest drag natively, through wl_data_device, and take
 *    a guest to host drag the same way.
 *
 *    The detection window is a GTK window on the x11 backend, which GDK is
 *    pinned to process wide, so a host to guest drag is an XDND drag from an
 *    Xwayland client. KWin and Mutter bridge such a drag to native Wayland
 *    clients. wlroots does not, so on sway every drop onto a Wayland
 *    application is lost. That is wlroots issue #841. A wl_data_device drag
 *    needs no bridge to reach Wayland clients, and reaches X11 targets too.
 *
 *    A drag has to start from the serial of a button press on a surface the
 *    client owns. This module maps a transparent wlr-layer-shell overlay on
 *    each output, so that one of them is under the pointer wherever the host
 *    has put it. The caller moves and presses the pointer through the same
 *    uinput device the X11 path uses.
 */

#ifndef _WAYLAND_DRAG_H_
#define _WAYLAND_DRAG_H_

#include <glib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One format on offer. The data is copied when the drag is armed, so the
 * caller keeps ownership of its buffer.
 */
typedef struct {
   const char *mimeType;
   const void *data;
   size_t size;
} WaylandDragItem;

/* Called once when the drag ends, whatever the reason. */
typedef void (*WaylandDragDoneFunc)(void *clientData);

/*
 * Connect to the compositor named by WAYLAND_DISPLAY and bind the globals a
 * drag needs.
 *
 * Returns FALSE outside Wayland, when the compositor does not implement
 * wlr-layer-shell, or when it did not answer in time. The caller then keeps
 * using XDND. GNOME is the main compositor without layer shell, and there
 * XDND already reaches Wayland clients. The caller may call this again after
 * a failure. It returns at once for a compositor without layer shell.
 */
gboolean WaylandDrag_Init(void);
void WaylandDrag_Exit(void);
gboolean WaylandDrag_IsAvailable(void);

/*
 * The size of the only output in pixels, for the uinput pointer's range.
 * Returns FALSE when there is no connection, no mode yet, or more than one
 * output.
 */
gboolean WaylandDrag_GetOutputSize(int *width, int *height);

/*
 * Offer these formats and map the overlays, so that the next pointer press
 * lands on one of them. Returns FALSE, and leaves nothing behind, if there is
 * nothing to offer or no overlay was mapped.
 */
gboolean WaylandDrag_Arm(const WaylandDragItem *items, size_t nItems);

/*
 * Turn the press the caller has just faked into a drag. This waits on the
 * module's own display connection, never on the caller's main loop, and
 * gives up after timeoutMs. A compositor that sends the press elsewhere then
 * costs at most timeoutMs.
 *
 * done() runs when the drag ends, exactly once, and only if this returned
 * TRUE. A drag that ends before this returns makes it return FALSE.
 */
gboolean WaylandDrag_StartOnPress(unsigned int timeoutMs,
                                  WaylandDragDoneFunc done,
                                  void *clientData);

/* Handle what the compositor has sent so far, without waiting. */
void WaylandDrag_Pump(void);

/* Wait until the target will take the drop, or until timeoutMs has passed. */
gboolean WaylandDrag_WaitForAccept(unsigned int timeoutMs);

/*
 * Move the pointer one pixel away and back. The compositor checks what is
 * under the cursor again only on pointer motion.
 */
typedef void (*WaylandDragNudgeFunc)(void *clientData);

/*
 * Make the target see the drag leave and come back, then wait for it as
 * WaylandDrag_WaitForAccept() does. This is for a target that is still
 * judging the drag on the contents of an earlier one.
 */
gboolean WaylandDrag_Refocus(unsigned int timeoutMs,
                             WaylandDragNudgeFunc nudge,
                             void *clientData);

/* Discard an armed drag that never started. done() is not called. */
void WaylandDrag_Disarm(void);

/*
 * End the drag now, whatever it is doing, or disarm one that never started.
 * Ending a running drag destroys the source, which ends the compositor's
 * grab, and done() runs before this returns. Disarming does not call done().
 */
void WaylandDrag_Cancel(void);

/*
 * The drop target, for guest to host drags.
 *
 * The host asks whether a drag is leaving the guest when the pointer leaves
 * the guest's window. The guest then has to become the target of whatever
 * drag the guest's pointer holds, read what that drag carries, and tell the
 * host. The X11 detection window does this where the compositor carries a
 * Wayland drag into Xwayland. WaylandDrag_ShowDropTarget() does it natively:
 * it maps the same transparent overlays the drag source uses, this time
 * taking pointer input, and handles the drag through wl_data_device.
 *
 * The drop target and the drag source share the overlays, so only one of
 * them can be active. WaylandDrag_ShowDropTarget() refuses while a drag is
 * armed or running, and WaylandDrag_Arm() hides the drop target first.
 */

/*
 * Called once per drag that enters the drop target, when its contents have
 * been read. mimeType is the type read, which is text/uri-list or one of the
 * text types. The data is NUL-terminated, and size does not count the NUL.
 * The data belongs to this module and is valid only during the call.
 */
typedef void (*WaylandDropDataFunc)(const char *mimeType,
                                    const char *data,
                                    size_t size,
                                    void *clientData);

/* Called when the drag drops on the drop target, or leaves it. */
typedef void (*WaylandDropEventFunc)(void *clientData);

/*
 * Map the overlays as a drop target. They cover every output and take the
 * pointer, so the next pointer motion puts the drag over one of them. When
 * a drag enters, this module picks the first type the drag offers out of
 * text/uri-list, text/plain;charset=utf-8, UTF8_STRING and text/plain,
 * accepts it for copy only, so that a source never deletes what the host has
 * yet to read, and reads it. gotData() then runs with the
 * bytes. dropped() runs when the drag drops on an overlay, and this module
 * then finishes the drop once the bytes are in. left() runs when the drag
 * leaves before dropping. Any callback may be NULL. Contents that arrive
 * after the drop are not handed over, since the drag is over by then.
 *
 * A drag that offers none of those types, or whose contents cannot be read,
 * is of no use to the host. The drop target then lets the pointer through,
 * so the drag moves on to the windows under it on its next motion.
 *
 * Calling this while the drop target is shown replaces the callbacks and
 * gives the overlays the pointer again, unless the drag it holds is of no
 * use to the host. Returns FALSE when no overlay was
 * mapped, when this module is not connected, or when a drag source is armed
 * or running.
 *
 * Waits on this module's own connection, never on the caller's main loop,
 * for at most half a second while the compositor maps the overlays.
 */
gboolean WaylandDrag_ShowDropTarget(WaylandDropDataFunc gotData,
                                    WaylandDropEventFunc dropped,
                                    WaylandDropEventFunc left,
                                    void *clientData);

/*
 * Destroy the overlays and any offer they hold, and stop any read. No
 * callback runs after this returns.
 */
void WaylandDrag_HideDropTarget(void);

/* Whether the drop target is shown. */
gboolean WaylandDrag_DropTargetShown(void);

/*
 * Whether the drop target holds a drag that has not dropped and whose
 * contents it has read.
 */
gboolean WaylandDrag_DropTargetHasData(void);

/*
 * Run gotData() again for the drag the drop target holds, if its contents
 * are in and it has not dropped. Returns TRUE if gotData() ran.
 */
gboolean WaylandDrag_DropTargetResend(void);

/*
 * Whether a drag is over the drop target, or has dropped on it and is not
 * finished yet.
 */
gboolean WaylandDrag_DropTargetHasDrag(void);

/*
 * Whether a drag, or the pointer without one, has entered the drop target
 * since it was shown. Neither means the compositor does not route the
 * pointer to it.
 */
gboolean WaylandDrag_DropTargetReached(void);

/*
 * Whether the compositor has sent wl_data_device.drop for the drag that
 * last entered the drop target.
 */
gboolean WaylandDrag_DropTargetDropped(void);

/*
 * Give the overlays the pointer, or take it away. Without the pointer the
 * overlays stay mapped but let the drag through to the windows under them,
 * which is what a drag that comes back into the guest needs.
 */
void WaylandDrag_DropTargetSetInput(gboolean input);

#ifdef __cplusplus
}
#endif

#endif /* _WAYLAND_DRAG_H_ */
