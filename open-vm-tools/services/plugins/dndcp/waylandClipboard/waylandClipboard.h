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
 * waylandClipboard.h --
 *
 *    Own and read the Wayland selection directly, through
 *    ext-data-control-v1.
 *
 *    The alternative is to run wl-clipboard. wl-copy takes one --type and
 *    serves one payload, so the fallback has to pick one format and drops
 *    the rest, for example the RTF of a rich text copy. A data control
 *    source can offer several types and answer each request with the bytes
 *    for that type, as the X11 backend does with multiple selection targets.
 */

#ifndef _WAYLAND_CLIPBOARD_H_
#define _WAYLAND_CLIPBOARD_H_

#include <glib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One format on offer. The data is copied when the offer is made, so the
 * caller keeps ownership of its buffer.
 */
typedef struct {
   const char *mimeType;
   const void *data;
   size_t size;
} WaylandClipItem;

/*
 * Connect and bind ext-data-control-v1.
 *
 * Returns FALSE when the compositor does not implement the protocol. GNOME is
 * the main case, since it does not implement data control protocols.
 */
gboolean WaylandClipboard_Init(void);
void WaylandClipboard_Exit(void);
gboolean WaylandClipboard_IsAvailable(void);

/*
 * Take the selection and offer every item. This module serves the items from
 * memory for as long as it holds the selection, so this returns immediately.
 */
gboolean WaylandClipboard_Offer(const WaylandClipItem *items, size_t nItems);

/* Is mimeType among the types the current selection owner offers? */
gboolean WaylandClipboard_HasType(const char *mimeType);

/*
 * Read the current selection as mimeType. Returns NULL when the type is not
 * on offer, the owner does not finish within timeoutMs, the data is empty or
 * larger than CPCLIPITEM_MAX_SIZE_V3, or the read fails. Free with g_free().
 */
void *WaylandClipboard_Receive(const char *mimeType,
                               size_t *size,
                               unsigned int timeoutMs);

/*
 * TRUE while this module owns the selection. Callers use it to avoid reading
 * back their own offer.
 */
gboolean WaylandClipboard_IsOwner(void);

/*
 * A number that changes each time the compositor announces a new selection,
 * including one this module installed. Callers compare it to learn whether
 * the selection changed since they last read it.
 */
guint64 WaylandClipboard_GetSelectionSerial(void);

#ifdef __cplusplus
}
#endif

#endif /* _WAYLAND_CLIPBOARD_H_ */
