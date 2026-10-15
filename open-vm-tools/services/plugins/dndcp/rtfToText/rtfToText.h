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
 * @file rtfToText.h
 *
 *    Render rich text as plain text.
 *
 *    The host sometimes sends a selection that carries only rich text, with
 *    no plain rendering next to it, as AbiWord does. Nothing that accepts only
 *    plain text can then take it. A rendering derived from the rich text gives
 *    those applications something to paste.
 *
 *    This is a rendering, not the document: control words other than the ones
 *    that describe layout are dropped. It is offered alongside the rich text,
 *    never instead of it.
 */

#ifndef _RTF_TO_TEXT_H_
#define _RTF_TO_TEXT_H_

#include <glib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Render an RTF document as plain UTF-8 text.
 *
 * Returns a NUL-terminated string to free with g_free(), or NULL when the
 * input is not RTF or holds no text. *outLen receives the length without the
 * terminator.
 */
gchar *RtfToText(const char *rtf, size_t len, size_t *outLen);

#ifdef __cplusplus
}
#endif

#endif /* _RTF_TO_TEXT_H_ */
