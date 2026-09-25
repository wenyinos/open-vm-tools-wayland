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

/**
 * @file copyPasteDnDWayland.h
 *
 * Concrete CopyPasteDnDImpl for Wayland sessions. Copy and paste is driven
 * by CopyPasteUIWayland, which owns the Wayland selection through
 * ext-data-control-v1. Drag and drop is delegated to DnDUIX11, which runs its
 * detection window under Xwayland. The RPC transport runs on the toolbox
 * GLib main loop.
 */

#ifndef __COPYPASTEDNDWAYLAND_H__
#define __COPYPASTEDNDWAYLAND_H__

#include "vm_basic_types.h"
#include "dnd.h"
#ifdef GTK4
#include <gtkmm/application.h>
#include "dndUIX11GTK4.h"
#else
#include "dndUIX11.h"
#endif
#include <gtk/gtk.h>
#include "copyPasteDnDImpl.h"

class CopyPasteUIWayland;

class CopyPasteDnDWayland : public CopyPasteDnDImpl
{
public:
   CopyPasteDnDWayland();
   ~CopyPasteDnDWayland();
   virtual gboolean Init(ToolsAppCtx *ctx);
   virtual void PointerInit();
   virtual gboolean RegisterCP();
   virtual void UnregisterCP();
   virtual gboolean RegisterDnD();
   virtual void UnregisterDnD();
   virtual void CopyPasteVersionChanged(const int version);
   virtual void DnDVersionChanged(const int version);
   virtual void SetCopyPasteAllowed(bool allowed);
   virtual void SetDnDAllowed(bool allowed);
   virtual uint32 GetCaps();

private:
   ToolsAppCtx *m_ctx;
   CopyPasteUIWayland *m_copyPasteUI;
   DnDUIX11 *m_dndUI;
#if defined(GTK3)
   Gtk::Main *m_main;
#endif
   /* GTK is up on an X display. Drag and drop needs it. */
   bool m_haveGtk;
};

#endif // __COPYPASTEDNDWAYLAND_H__
