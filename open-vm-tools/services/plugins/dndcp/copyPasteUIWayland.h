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
 * @file copyPasteUIWayland.h
 *
 * This class implements host<->guest copy/paste (version 3+ of the protocol)
 * for Wayland sessions. The local selection is owned and read through
 * ext-data-control-v1 where the compositor implements it. This is the
 * counterpart of copyPasteUIX11.h for the Wayland platform.
 *
 * Text, RTF and PNG images are supported.
 */

#ifndef __COPYPASTE_UI_WAYLAND_H__
#define __COPYPASTE_UI_WAYLAND_H__

#include <string>
#include <vector>
#include <glib.h>
#include <gdk/gdk.h>
#include <sigc++/trackable.h>

#include "vm_basic_types.h"
#include "dnd.h"

extern "C" {
#include "dndClipboard.h"
#include "vmware/tools/guestrpc.h"
}

#include "guestCopyPaste.hh"

class CopyPasteUIWayland
   : public sigc::trackable
{
public:
   CopyPasteUIWayland();
   virtual ~CopyPasteUIWayland();
   bool Init();
   void VmxCopyPasteVersionChanged(RpcChannel *chan, uint32 version);
   void SetCopyPasteAllowed(bool isCopyPasteAllowed)
   { mCP->SetCopyPasteAllowed(isCopyPasteAllowed); }

private:
   /* Called when the host sends new clipboard contents. */
   void GetRemoteClipboardCB(const CPClipboard *clip);
   /* Called when the host asks for the guest clipboard. */
   void GetLocalClipboard(void);
   void SendClipNotChanged(void);
   void SendLocalClip(void);
   /* Forget the formats recorded in mLastClipboard*. */
   void ClearLastClipboard(void);

   GuestCopyPasteMgr *mCP;
   CPClipboard mClipboard;
   std::string mLastClipboardText;
   std::string mLastClipboardImage;
   std::string mLastClipboardRtf;
   /* The last host clip, serialized, to tell a repeat from a new one. */
   std::string mLastHostClip;
   bool mInited;

   bool GetSelectionGeneration(guint64 *generation);
   /* The generation last read. See GetLocalClipboard(). */
   guint64 mReadGeneration;
   /* The generation last read again after its reads ran out of time. */
   guint64 mRetriedGeneration;
};

#endif // __COPYPASTE_UI_WAYLAND_H__
