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
 * @file copyPasteDnDWayland.cpp
 *
 * Copy and paste for a Wayland session, derived from copyPasteDnDX11.cpp.
 * It creates no invisible X window, registers no legacy backdoor copy paste,
 * and sets none of the Xlib globals that CopyPasteDnDX11::Init() sets.
 * Drag and drop is delegated to DnDUIX11. Without an X display, copy and
 * paste still works and drag and drop is left off.
 */

#define G_LOG_DOMAIN "dndcp"

#include <string.h>

#include "copyPasteDnDWrapper.h"
#include "copyPasteDnDWayland.h"
#include "copyPasteUIWayland.h"

extern "C" {
#include "dndCPMsgV4.h"
#include "vmblock.h"
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::CopyPasteDnDWayland --
 *
 *    Constructor.
 *
 *-----------------------------------------------------------------------------
 */

CopyPasteDnDWayland::CopyPasteDnDWayland()
 : m_ctx(NULL),
   m_copyPasteUI(NULL),
   m_dndUI(NULL),
   m_blockInited(false),
#if defined(GTK3)
   m_main(NULL),
#endif
   m_haveGtk(false)
{
   memset(&m_blockCtrl, 0, sizeof m_blockCtrl);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::~CopyPasteDnDWayland --
 *
 *    Destructor.
 *
 *-----------------------------------------------------------------------------
 */

CopyPasteDnDWayland::~CopyPasteDnDWayland()
{
   if (m_copyPasteUI) {
      delete m_copyPasteUI;
      m_copyPasteUI = NULL;
   }
   if (m_dndUI) {
      delete m_dndUI;
      m_dndUI = NULL;
   }
   if (m_blockInited && DnD_BlockIsReady(&m_blockCtrl)) {
      DnD_UninitializeBlocking(&m_blockCtrl);
      m_blockInited = false;
   }
#if defined(GTK3)
   delete m_main;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::Init --
 *
 *    Initialize the Wayland platform DnD/CP. Pin GDK to its X11 backend for
 *    drag and drop and bring up vmblock. The copy/paste UI is created lazily
 *    in RegisterCP, mirroring the X11 backend.
 *
 *    Gtk::Application and Gtk::Main call gtk_init(), which exits the process
 *    when it cannot open a display, and a Wayland session may have no
 *    Xwayland. So this calls gtk_init_check() first and creates them only
 *    when it succeeds. Without GTK, drag and drop stays off.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
CopyPasteDnDWayland::Init(ToolsAppCtx *ctx)
{
   m_ctx = ctx;

   /*
    * Drag and drop is delegated to DnDUIX11, which needs GTK on its X11
    * backend. Under a Wayland session that makes the detection window an
    * Xwayland client. DnDUIX11 then uses XDND, which the compositor bridges
    * to native clients. The X11-specific code would crash on a Wayland
    * GdkSurface. This mirrors CopyPasteDnDX11::Init. Where the compositor has
    * no ext-data-control-v1, copy and paste owns the X selection through GDK
    * and learns from GDK when the selection changes.
    */
   gdk_set_allowed_backends("x11");
#ifdef GTK4
   m_haveGtk = gtk_init_check();
   if (m_haveGtk) {
      Gtk::Application::create("com.tools.gtk4");
   }
#else
   int argc = 1;
   const char *argv[] = {"", NULL};
   char **argvp = (char **) argv;

   m_haveGtk = gtk_init_check(&argc, &argvp);
   if (m_haveGtk) {
      m_main = new Gtk::Main(&argc, &argvp, false);
   }
#endif
   if (!m_haveGtk) {
      g_warning("%s: cannot open an X display, drag and drop is disabled\n",
                __FUNCTION__);
   }

   /*
    * Bring up the vmblock subsystem. vmware-user-suid-wrapper opens
    * VMBLOCK_FUSE_DEVICE as root and passes the descriptor as --blockFd, so
    * the common path is CompleteBlockInitialization; InitializeBlocking is the
    * fallback for a daemon started without the wrapper, and will fail without
    * privilege. Host to guest file paste needs vmblock. It lets this backend
    * publish a URI list at once and stall the reader until the bytes arrive.
    */
   if (!m_blockInited && ctx != NULL) {
      memset(&m_blockCtrl, 0, sizeof m_blockCtrl);
      m_blockCtrl.fd = ctx->blockFD;
      m_blockCtrl.fd >= 0 ?
         DnD_CompleteBlockInitialization(m_blockCtrl.fd, &m_blockCtrl) :
         DnD_InitializeBlocking(&m_blockCtrl);
      m_blockInited = true;
      g_debug("%s: vmblock %s (fd %d)\n", __FUNCTION__,
              DnD_BlockIsReady(&m_blockCtrl) ? "ready" : "unavailable",
              m_blockCtrl.fd);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::PointerInit --
 *
 *    No-op on Wayland. Pointer_Init() works through the Xlib globals that
 *    CopyPasteDnDX11::Init() sets, and this backend leaves them unset.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::PointerInit()
{
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::RegisterCP --
 *
 *    Register copy and paste capabilities with the VMX. Mirrors
 *    CopyPasteDnDX11::RegisterCP without the legacy backdoor registration.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
CopyPasteDnDWayland::RegisterCP()
{
   CopyPasteDnDWrapper *wrapper = CopyPasteDnDWrapper::GetInstance();

   if (wrapper->IsCPRegistered()) {
      return TRUE;
   }

   if (!wrapper->IsCPEnabled()) {
      return FALSE;
   }

   m_copyPasteUI = new CopyPasteUIWayland();
   if (m_copyPasteUI) {
      m_copyPasteUI->SetBlockControl(&m_blockCtrl);
      if (m_copyPasteUI->Init()) {
         wrapper->SetCPIsRegistered(TRUE);
         int version = wrapper->GetCPVersion();
         g_debug("%s: version is %d\n", __FUNCTION__, version);
         if (version >= 3) {
            CopyPasteVersionChanged(version);
            m_copyPasteUI->SetCopyPasteAllowed(TRUE);
         }
      } else {
         delete m_copyPasteUI;
         m_copyPasteUI = NULL;
      }
   }
   return wrapper->IsCPRegistered();
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::UnregisterCP --
 *
 *    Unregister copy paste capabilities and clean up.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::UnregisterCP()
{
   CopyPasteDnDWrapper *wrapper = CopyPasteDnDWrapper::GetInstance();

   if (wrapper->IsCPRegistered()) {
      if (m_copyPasteUI) {
         delete m_copyPasteUI;
         m_copyPasteUI = NULL;
      }
      wrapper->SetCPIsRegistered(FALSE);
      wrapper->SetCPVersion(-1);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::RegisterDnD --
 *
 *    Drag and drop is delegated to DnDUIX11, which drives XDND through
 *    Xwayland. A drag is driven by real pointer input, so it does not need the
 *    data-control protocols. Without GTK there is no drag and drop.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
CopyPasteDnDWayland::RegisterDnD()
{
   CopyPasteDnDWrapper *wrapper = CopyPasteDnDWrapper::GetInstance();

   if (!wrapper->IsDnDEnabled() || !m_haveGtk) {
      return FALSE;
   }

   if (!wrapper->IsDnDRegistered()) {
      m_dndUI = new DnDUIX11(wrapper->GetToolsAppCtx());
      if (m_dndUI != NULL) {
         if (m_dndUI->Init()) {
            wrapper->SetDnDIsRegistered(TRUE);
            m_dndUI->SetDnDAllowed(TRUE);
            int version = wrapper->GetDnDVersion();
            g_debug("%s: dnd version is %d\n", __FUNCTION__, version);
            if (version >= 3) {
               DnDVersionChanged(version);
            }
         } else {
            delete m_dndUI;
            m_dndUI = NULL;
         }
      }
   }
   g_debug("%s: dnd is registered? %d\n", __FUNCTION__,
           (int) wrapper->IsDnDRegistered());
   return wrapper->IsDnDRegistered();
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::UnregisterDnD --
 *
 *    Unregister drag and drop capabilities and clean up.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::UnregisterDnD()
{
   CopyPasteDnDWrapper *wrapper = CopyPasteDnDWrapper::GetInstance();

   if (!wrapper->IsDnDRegistered()) {
      return;
   }
   if (m_dndUI != NULL) {
      delete m_dndUI;
      m_dndUI = NULL;
   }
   wrapper->SetDnDIsRegistered(FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::CopyPasteVersionChanged --
 *
 *    Communicate a copy paste version change to the UI layer.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::CopyPasteVersionChanged(const int version)
{
   CopyPasteDnDWrapper *wrapper = CopyPasteDnDWrapper::GetInstance();
   ToolsAppCtx *ctx = wrapper->GetToolsAppCtx();

   g_debug("%s: version %d\n", __FUNCTION__, version);
   if (ctx != NULL && m_copyPasteUI != NULL) {
      m_copyPasteUI->VmxCopyPasteVersionChanged(ctx->rpc, version);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::DnDVersionChanged --
 *
 *    Communicate a drag and drop version change to the UI layer.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::DnDVersionChanged(const int version)
{
   CopyPasteDnDWrapper *wrapper = CopyPasteDnDWrapper::GetInstance();
   ToolsAppCtx *ctx = wrapper->GetToolsAppCtx();

   if (m_dndUI == NULL || ctx == NULL) {
      return;
   }
   g_debug("%s: calling VmxDnDVersionChanged (version %d)\n",
           __FUNCTION__, version);
   m_dndUI->VmxDnDVersionChanged(ctx->rpc, version);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::SetCopyPasteAllowed --
 *
 *    Communicate copy paste allowed state to the UI layer.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::SetCopyPasteAllowed(bool allowed)
{
   if (m_copyPasteUI != NULL) {
      m_copyPasteUI->SetCopyPasteAllowed(allowed);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::SetDnDAllowed --
 *
 *    Communicate drag and drop allowed state to the UI layer.
 *
 *-----------------------------------------------------------------------------
 */

void
CopyPasteDnDWayland::SetDnDAllowed(bool allowed)
{
   if (m_dndUI != NULL) {
      m_dndUI->SetDnDAllowed(allowed);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CopyPasteDnDWayland::GetCaps --
 *
 *    Report what this backend supports, which is copy and paste of every
 *    format the X11 backend handles.
 *
 *    Copy and paste owns the Wayland selection directly, so one selection can
 *    carry rich text and its plain rendering at once.
 *
 *-----------------------------------------------------------------------------
 */

uint32
CopyPasteDnDWayland::GetCaps()
{
   return DND_CP_CAP_VALID |
          DND_CP_CAP_CP |
          DND_CP_CAP_FORMATS_CP |
          DND_CP_CAP_ACTIVE_CP |
          DND_CP_CAP_BIG_BUFFER;
}
