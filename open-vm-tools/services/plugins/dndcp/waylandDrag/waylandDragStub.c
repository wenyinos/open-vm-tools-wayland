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
 * waylandDragStub.c --
 *
 *    The native drag source for a build without Wayland support. A native
 *    drag is never available, so the caller always uses XDND.
 */

#include "waylandDrag.h"


gboolean
WaylandDrag_Init(void)
{
   return FALSE;
}


void
WaylandDrag_Exit(void)
{
}


gboolean
WaylandDrag_IsAvailable(void)
{
   return FALSE;
}


gboolean
WaylandDrag_GetOutputSize(int *width,   // OUT: unused
                          int *height)  // OUT: unused
{
   return FALSE;
}


gboolean
WaylandDrag_Arm(const WaylandDragItem *items,  // IN: unused
                size_t nItems)                 // IN: unused
{
   return FALSE;
}


gboolean
WaylandDrag_StartOnPress(unsigned int timeoutMs,     // IN: unused
                         WaylandDragDoneFunc done,   // IN: unused
                         void *clientData)           // IN: unused
{
   return FALSE;
}


void
WaylandDrag_Pump(void)
{
}


gboolean
WaylandDrag_WaitForAccept(unsigned int timeoutMs)   // IN: unused
{
   return FALSE;
}


gboolean
WaylandDrag_Refocus(unsigned int timeoutMs,        // IN: unused
                    WaylandDragNudgeFunc nudge,    // IN: unused
                    void *clientData)              // IN: unused
{
   return FALSE;
}


void
WaylandDrag_Disarm(void)
{
}


void
WaylandDrag_Cancel(void)
{
}
