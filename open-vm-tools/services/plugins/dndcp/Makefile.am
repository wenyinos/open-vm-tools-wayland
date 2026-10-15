################################################################################
### Copyright (c) 2009-2025 Broadcom. All Rights Reserved.
### Broadcom Confidential. The term "Broadcom" refers to Broadcom Inc.
### and/or its subsidiaries.
###
### This program is free software; you can redistribute it and/or modify
### it under the terms of version 2 of the GNU General Public License as
### published by the Free Software Foundation.
###
### This program is distributed in the hope that it will be useful,
### but WITHOUT ANY WARRANTY; without even the implied warranty of
### MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
### GNU General Public License for more details.
###
### You should have received a copy of the GNU General Public License
### along with this program; if not, write to the Free Software
### Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
################################################################################

AUTOMAKE_OPTIONS = subdir-objects

plugindir = @VMUSR_PLUGIN_INSTALLDIR@
plugin_LTLIBRARIES = libdndcp.la

libdndcp_la_CPPFLAGS =
libdndcp_la_CPPFLAGS += @GTK_CPPFLAGS@
libdndcp_la_CPPFLAGS += @PLUGIN_CPPFLAGS@
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/dnd
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/dndGuest
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/dndGuestBase
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/stringxx
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/xutils
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/waylandClipboard
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/waylandDrag

libdndcp_la_CPPFLAGS += -I$(top_srcdir)/include
libdndcp_la_CPPFLAGS += @XDR_CPPFLAGS@

# Passing C++ related flags to CPPFLAGS generates error.
# So, we need to pass these to C++ compilation only.
libdndcp_la_CXXFLAGS =
libdndcp_la_CXXFLAGS += @SIGCXX_CPPFLAGS@
libdndcp_la_CXXFLAGS += @GTKMM_CPPFLAGS@

libdndcp_la_LDFLAGS =
libdndcp_la_LDFLAGS += @PLUGIN_LDFLAGS@

libdndcp_la_LIBADD =
libdndcp_la_LIBADD += @COMMON_XLIBS@
libdndcp_la_LIBADD += @GTK_LIBS@
libdndcp_la_LIBADD += @GTKMM_LIBS@
libdndcp_la_LIBADD += @VMTOOLS_LIBS@
libdndcp_la_LIBADD += @HGFS_LIBS@
libdndcp_la_LIBADD += @THREAD_LIBS@
libdndcp_la_LIBADD += $(top_builddir)/lib/hgfsUri/hgfsUriPosix.lo
libdndcp_la_LIBADD += @XDR_LIBS@

libdndcp_la_SOURCES =

libdndcp_la_SOURCES += dnd/dndClipboard.c
libdndcp_la_SOURCES += dnd/dndCommon.c
libdndcp_la_SOURCES += dnd/dndCPMsgV4.c
libdndcp_la_SOURCES += dnd/dndLinux.c
libdndcp_la_SOURCES += dnd/dndMsg.c

libdndcp_la_SOURCES += dndGuestBase/copyPasteDnDWrapper.cpp
libdndcp_la_SOURCES += dndGuestBase/dndFileList.cc
libdndcp_la_SOURCES += dndGuestBase/dndRpcV3.cc
libdndcp_la_SOURCES += dndGuestBase/guestDnDCPMgr.cc
libdndcp_la_SOURCES += dndGuestBase/guestDnDDest.cc
libdndcp_la_SOURCES += dndGuestBase/guestDnDSrc.cc
libdndcp_la_SOURCES += dndGuestBase/guestDnDMgr.cc
libdndcp_la_SOURCES += dndGuestBase/guestFileTransfer.cc

libdndcp_la_SOURCES += dndGuest/copyPasteRpcV3.cc
libdndcp_la_SOURCES += dndGuest/guestCopyPasteDest.cc
libdndcp_la_SOURCES += dndGuest/guestCopyPasteMgr.cc
libdndcp_la_SOURCES += dndGuest/guestCopyPasteSrc.cc
libdndcp_la_SOURCES += dndGuest/vmGuestDnDCPMgr.cc
libdndcp_la_SOURCES += dndGuest/vmGuestFileTransfer.cc
libdndcp_la_SOURCES += dndGuest/vmGuestDnDSrc.cc
libdndcp_la_SOURCES += dndGuest/copyPasteRpcV4.cc
libdndcp_la_SOURCES += dndGuest/vmGuestDnDMgr.cc
libdndcp_la_SOURCES += dndGuest/dndRpcV4.cc
libdndcp_la_SOURCES += dndGuest/fileTransferRpcV4.cc
libdndcp_la_SOURCES += dndGuest/rpcV3Util.cpp
libdndcp_la_SOURCES += dndGuest/rpcV4Util.cpp
libdndcp_la_SOURCES += dndGuest/dndCPTransportGuestRpc.cpp

libdndcp_la_SOURCES += stringxx/string.cc
if HAVE_GTK4
libdndcp_la_SOURCES += xutils/xutilsGTK4.cc
else
libdndcp_la_SOURCES += xutils/xutils.cc
endif

libdndcp_la_SOURCES += dndcp.cpp
libdndcp_la_SOURCES += pointer.cpp
libdndcp_la_SOURCES += copyPasteCompat.c
libdndcp_la_SOURCES += vmCopyPasteDnDWrapper.cpp
libdndcp_la_SOURCES += copyPasteDnDX11.cpp
libdndcp_la_SOURCES += copyPasteDnDUtil.cpp
# Renders rich text as plain text, for a host clip that carries only rich
# text. Used by all three copy and paste backends.
libdndcp_la_SOURCES += rtfToText/rtfToText.c
if HAVE_GTK4
libdndcp_la_SOURCES += copyPasteCompatX11GTK4.c
libdndcp_la_SOURCES += copyPasteUIX11GTK4.cpp
libdndcp_la_SOURCES += dndUIX11GTK4.cpp
libdndcp_la_SOURCES += dragDetWndX11GTK4.cpp
else
libdndcp_la_SOURCES += copyPasteCompatX11.c
libdndcp_la_SOURCES += copyPasteUIX11.cpp
libdndcp_la_SOURCES += dndUIX11.cpp
libdndcp_la_SOURCES += dragDetWndX11.cpp
endif

if HAVE_WAYLAND
libdndcp_la_CPPFLAGS += @WAYLAND_CFLAGS@
libdndcp_la_LIBADD += @WAYLAND_LIBS@

# The Wayland backend is built for both the GTK3 and the GTK4 configuration.
# It owns the selection through ext-data-control-v1 and falls back to the X
# selection where the compositor does not implement it. On that fallback it
# watches the selection through GdkClipboard in GTK4 and GtkClipboard in GTK3.
libdndcp_la_SOURCES += copyPasteDnDWayland.cpp
libdndcp_la_SOURCES += copyPasteUIWayland.cpp
libdndcp_la_SOURCES += waylandClipboard/wayland-data-control.c
libdndcp_la_SOURCES += waylandClipboard/waylandClipboard.c
libdndcp_la_SOURCES += waylandDrag/waylandDrag.c
else
# Without Wayland support a native drag is never available and dndUIX11GTK4
# uses XDND.
libdndcp_la_SOURCES += waylandDrag/waylandDragStub.c
endif

if HAVE_LIBEI
# Pointer integration through the RemoteDesktop portal, which needs libei to
# drive the EIS socket the portal returns. See waylandPointer/pointerPortal.c.
libdndcp_la_CPPFLAGS += @LIBEI_CFLAGS@
libdndcp_la_LIBADD += @LIBEI_LIBS@
libdndcp_la_SOURCES += waylandPointer/pointerPortal.c

# The portal attributes a RemoteDesktop request to an application id, and that
# id has to name an installed .desktop file. The plugin registers the id in
# pointerPortal.c; this installs the file it refers to, which is also what
# gives the user an entry to manage in the desktop's permission settings.
portal_app_id = org.openvm.tools
portal_desktop_file = $(portal_app_id).desktop
portal_desktop_src = $(top_srcdir)/services/plugins/dndcp/$(portal_desktop_file).in

applicationsdir = $(datadir)/applications
applications_DATA = $(portal_desktop_file)

$(portal_desktop_file): $(portal_desktop_src)
	sed 's,_bindir_,$(bindir),' $(portal_desktop_src) > $@
endif

if LINUX
libdndcp_la_CPPFLAGS += -I$(top_srcdir)/services/plugins/dndcp/fakeMouse
libdndcp_la_SOURCES  += fakeMouseWayland/fakeMouseWayland.cpp
libdndcp_la_CPPFLAGS += -DUSE_UINPUT
endif

if HAVE_MKDTEMP
libdndcp_la_CPPFLAGS += -DDND_IS_XDG
libdndcp_la_SOURCES  += dnd/dndXdg.c
endif

# DND_VM is used to distinguish the dnd feature between WS/FS and View
libdndcp_la_CPPFLAGS += -DDND_VM

BUILT_SOURCES =
BUILT_SOURCES += cpFileContents.h
BUILT_SOURCES += cpFileContents_xdr.c

CLEANFILES =
CLEANFILES += $(BUILT_SOURCES)
if HAVE_LIBEI
CLEANFILES += $(portal_desktop_file)
endif

libdndcp_la_SOURCES += cpFileContents_xdr.c

cpFileContents.h: cpFileContents.x
	@RPCGEN_WRAPPER@ services/plugins/dndcp/cpFileContents.x $@

cpFileContents_xdr.c: cpFileContents.x cpFileContents.h
	@RPCGEN_WRAPPER@ services/plugins/dndcp/cpFileContents.x $@

if HAVE_WAYLAND
# The protocol code is generated by wayland-scanner from the XML in
# wayland-protocols, except for wlr-layer-shell and wlr-data-control, which
# are not part of it and are kept next to their users.
EXT_DATA_CONTROL_XML = $(WAYLAND_PROTOCOLS_DATADIR)/staging/ext-data-control/ext-data-control-v1.xml
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DATADIR)/stable/xdg-shell/xdg-shell.xml
LAYER_SHELL_XML = $(srcdir)/waylandDrag/wlr-layer-shell-unstable-v1.xml
WLR_DATA_CONTROL_XML = $(srcdir)/waylandClipboard/wlr-data-control-unstable-v1.xml

BUILT_SOURCES += ext-data-control-v1-client-protocol.h
BUILT_SOURCES += ext-data-control-v1-protocol.c
BUILT_SOURCES += wlr-data-control-unstable-v1-client-protocol.h
BUILT_SOURCES += wlr-data-control-unstable-v1-protocol.c
BUILT_SOURCES += wlr-layer-shell-unstable-v1-client-protocol.h
BUILT_SOURCES += wlr-layer-shell-unstable-v1-protocol.c
BUILT_SOURCES += xdg-shell-protocol.c

nodist_libdndcp_la_SOURCES =
nodist_libdndcp_la_SOURCES += ext-data-control-v1-protocol.c
nodist_libdndcp_la_SOURCES += wlr-data-control-unstable-v1-protocol.c
nodist_libdndcp_la_SOURCES += wlr-layer-shell-unstable-v1-protocol.c
# wlr-layer-shell names xdg_popup in a request this does not use, and the
# generated code refers to its interface either way.
nodist_libdndcp_la_SOURCES += xdg-shell-protocol.c

ext-data-control-v1-client-protocol.h: $(EXT_DATA_CONTROL_XML)
	$(WAYLAND_SCANNER) client-header $(EXT_DATA_CONTROL_XML) $@

ext-data-control-v1-protocol.c: $(EXT_DATA_CONTROL_XML)
	$(WAYLAND_SCANNER) private-code $(EXT_DATA_CONTROL_XML) $@

wlr-data-control-unstable-v1-client-protocol.h: $(WLR_DATA_CONTROL_XML)
	$(WAYLAND_SCANNER) client-header $(WLR_DATA_CONTROL_XML) $@

wlr-data-control-unstable-v1-protocol.c: $(WLR_DATA_CONTROL_XML)
	$(WAYLAND_SCANNER) private-code $(WLR_DATA_CONTROL_XML) $@

wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL_XML)
	$(WAYLAND_SCANNER) client-header $(LAYER_SHELL_XML) $@

wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL_XML)
	$(WAYLAND_SCANNER) private-code $(LAYER_SHELL_XML) $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL_XML) $@
endif

EXTRA_DIST = waylandDrag/wlr-layer-shell-unstable-v1.xml
EXTRA_DIST += waylandClipboard/wlr-data-control-unstable-v1.xml
if HAVE_LIBEI
EXTRA_DIST += $(portal_desktop_file).in
endif
