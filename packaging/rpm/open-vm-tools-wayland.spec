#
# RPM spec for the open-vm-tools Wayland fork.
#
# This package deliberately carries a name of its own instead of squatting on
# "open-vm-tools". It obsoletes and provides the distribution package so that
# dnf swaps one for the other in a single transaction; the two can never be
# installed side by side.
#
# Do not add "Conflicts: open-vm-tools" here. With both Obsoletes and Conflicts
# pointing at the same name, dnf resolves the conflict before it processes the
# obsoletes and refuses the transaction with
#   "installed package open-vm-tools conflicts with open-vm-tools provided by ..."
# Obsoletes alone performs the replacement cleanly.
#
# Build with packaging/rpm/build.sh, which derives the version and the source
# tarball name from the tree.

%global toolsname    open-vm-tools
%global toolsbuild   25218885
%global toolsdaemon  vmtoolsd
%global vgauthdaemon vgauthd

%{!?srcname:     %global srcname open-vm-tools-13.1.0-25218885}
%{!?toolsversion: %global toolsversion 13.1.0}

Name:           open-vm-tools-wayland
Version:        %{toolsversion}
Release:        1%{?dist}
Summary:        Open Virtual Machine Tools for VMware guests, with native Wayland support

License:        GPL-2.0-only AND W3C AND LGPL-2.1-only AND ICU AND ISC AND MIT
URL:            https://github.com/wenyinos/open-vm-tools-wayland
Source0:        %{srcname}.tar.gz
Source1:        %{toolsdaemon}.service
Source2:        %{vgauthdaemon}.service
Source3:        run-vmblock\x2dfuse.mount
Source4:        open-vm-tools.conf
Source5:        vmtoolsd.pam

ExclusiveArch:  x86_64

# Non-coexistence with the distribution package. Unversioned Obsoletes matches
# every version, so a later distribution build does not slip in beside this one.
Provides:       open-vm-tools = %{version}-%{release}
Obsoletes:      open-vm-tools
Provides:       open-vm-tools-sdmp = %{version}-%{release}
Obsoletes:      open-vm-tools-sdmp
Obsoletes:      open-vm-tools-containerinfo
Obsoletes:      open-vm-tools-test
Obsoletes:      open-vm-tools-devel

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  libtool
BuildRequires:  make
BuildRequires:  gcc-c++
BuildRequires:  fuse3-devel >= 3.10.0
BuildRequires:  libtool-ltdl-devel
BuildRequires:  libX11-devel
BuildRequires:  libXext-devel
BuildRequires:  libXi-devel
BuildRequires:  libXinerama-devel
BuildRequires:  libXrandr-devel
BuildRequires:  libXrender-devel
BuildRequires:  libXtst-devel
BuildRequires:  pam-devel
BuildRequires:  pkgconfig(glib-2.0) >= 2.34.0
BuildRequires:  pkgconfig(gmodule-2.0) >= 2.34.0
BuildRequires:  pkgconfig(gobject-2.0) >= 2.34.0
BuildRequires:  pkgconfig(gthread-2.0) >= 2.34.0
BuildRequires:  pkgconfig(gtk4) >= 4.0.0
BuildRequires:  pkgconfig(gtkmm-4.0) >= 4.0.0
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libei-1.0)
BuildRequires:  pkgconfig(libmspack) >= 0.0.20040308alpha
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(libtirpc)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols) >= 1.39
BuildRequires:  rpcgen
BuildRequires:  systemd-rpm-macros
BuildRequires:  systemd-udev
BuildRequires:  xmlsec1-openssl-devel

Requires:       coreutils
Requires:       pkgconfig

%description
open-vm-tools supplies the guest-side drivers and agents that let a VMware
guest report its status to the host, synchronise its clock, resize its
display, and share the clipboard and dragged files with the host.

This build is the upstream 13.1.0 release plus the Wayland work: clipboard and
drag-and-drop over the Wayland data-control protocols, and pointer integration
through the XDG RemoteDesktop portal and libei. The X11 paths are unchanged and
are still used in an X11 session.

It installs the same binaries and plugins as the distribution package, so it
replaces it rather than living beside it.

%package desktop
Summary:        Desktop integration plugins for open-vm-tools-wayland
Requires:       %{name} = %{version}-%{release}
Provides:       open-vm-tools-desktop = %{version}-%{release}
Obsoletes:      open-vm-tools-desktop

%description desktop
The per-user plugins that run inside the desktop session: clipboard and
drag-and-drop, automatic display resizing, and the VMware tools autostart
entry. Install this on a guest with a graphical session.

%prep
%autosetup -n %{srcname}

%build
autoreconf -vif

%configure \
    --disable-static \
    --disable-docs \
    --enable-resolutionkms \
    --enable-servicediscovery \
    --enable-vmwgfxctrl \
    --with-fuse \
    --with-gtk4 \
    --with-tirpc \
    --with-wayland \
    --with-xmlsec1 \
    --without-kernel-modules

sed -i -e 's! -shared ! -Wl,--as-needed\0!g' libtool
%make_build

%install
%make_install

# Configuration and data files carry no exec bit.
chmod a-x %{buildroot}%{_sysconfdir}/pam.d/*
chmod a-x %{buildroot}%{_sysconfdir}/vmware-tools/*.conf
chmod a-x %{buildroot}%{_sysconfdir}/vmware-tools/vgauth/schemas/*
chmod a-x %{buildroot}%{_udevrulesdir}/99-vmware-scsi-udev.rules

# Static libtool archives are not packaged.
find %{buildroot}%{_libdir} -name '*.la' -delete

# No -dev package is shipped, so the headers, the pkg-config files and the
# unversioned linker symlinks have nowhere to go.
rm -rf %{buildroot}%{_includedir}/libDeployPkg %{buildroot}%{_includedir}/vmGuestLib
rm -rf %{buildroot}%{_libdir}/pkgconfig
rm -f %{buildroot}%{_libdir}/libDeployPkg.so %{buildroot}%{_libdir}/libguestlib.so \
      %{buildroot}%{_libdir}/libguestStoreClient.so %{buildroot}%{_libdir}/libhgfs.so \
      %{buildroot}%{_libdir}/libvgauth.so %{buildroot}%{_libdir}/libvmtools.so

# The autostart entry has no use for the legacy Encoding key.
sed -i '/^Encoding/d' %{buildroot}%{_sysconfdir}/xdg/autostart/vmware-user.desktop

install -p -m 644 -D %{SOURCE1} %{buildroot}%{_unitdir}/%{toolsdaemon}.service
install -p -m 644 -D %{SOURCE2} %{buildroot}%{_unitdir}/%{vgauthdaemon}.service
install -p -m 644 -D '%{SOURCE3}' %{buildroot}%{_unitdir}/run-vmblock\\x2dfuse.mount
install -p -m 644 -D %{SOURCE4} %{buildroot}%{_modulesloaddir}/open-vm-tools.conf
install -p -m 644 -D %{SOURCE5} %{buildroot}%{_sysconfdir}/pam.d/vmtoolsd

%post
# Shared Folders needs its mount point to exist before the mount unit runs.
if [ -f %{_bindir}/vmware-checkvm -a -f %{_bindir}/vmhgfs-fuse ] && \
   %{_bindir}/systemd-detect-virt | grep -iq VMware &&               \
   %{_bindir}/vmware-checkvm &> /dev/null &&                         \
   %{_bindir}/vmware-checkvm -p | grep -q Workstation &&             \
   %{_bindir}/vmhgfs-fuse -e &> /dev/null; then
   mkdir -p /mnt/hgfs
fi

%systemd_post %{vgauthdaemon}.service %{toolsdaemon}.service

# Presets are not applied when the package is installed into a container or a
# chroot, so enable the daemon explicitly in a VMware guest.
if %{_bindir}/systemd-detect-virt | grep -iq VMware; then
   %{_bindir}/systemctl enable %{vgauthdaemon}.service %{toolsdaemon}.service &> /dev/null || /bin/true
fi

%preun
%systemd_preun %{toolsdaemon}.service %{vgauthdaemon}.service

if [ "$1" = "0" -a -f %{_bindir}/vmware-checkvm ] && \
   %{_bindir}/systemd-detect-virt | grep -iq VMware && \
   %{_bindir}/vmware-checkvm &> /dev/null; then
   # Tell the host that the tools are going away.
   if [ -f %{_bindir}/vmware-rpctool ]; then
      %{_bindir}/vmware-rpctool 'tools.set.version 0' &> /dev/null || /bin/true
   fi

   if [ -d /mnt/hgfs ] && %{_bindir}/vmware-checkvm -p | grep -q Workstation; then
      umount /mnt/hgfs &> /dev/null || /bin/true
      rmdir /mnt/hgfs &> /dev/null || /bin/true
   fi
fi

%postun
%systemd_postun_with_restart %{toolsdaemon}.service %{vgauthdaemon}.service

%post desktop
%systemd_post run-vmblock\\x2dfuse.mount

if [ -f %{_bindir}/vmware-checkvm ] &&               \
   %{_bindir}/systemd-detect-virt | grep -iq VMware && \
   %{_bindir}/vmware-checkvm &> /dev/null &&         \
   %{_bindir}/vmware-checkvm -p | grep -q Workstation; then
   %{_bindir}/systemctl enable run-vmblock\\x2dfuse.mount &> /dev/null || /bin/true
fi

%preun desktop
%systemd_preun run-vmblock\\x2dfuse.mount

%postun desktop
%systemd_postun run-vmblock\\x2dfuse.mount

%files
%license COPYING
%doc AUTHORS ChangeLog NEWS README
%config(noreplace) %{_sysconfdir}/pam.d/vmtoolsd
%dir %{_sysconfdir}/vmware-tools/
%dir %{_sysconfdir}/vmware-tools/vgauth/
%dir %{_sysconfdir}/vmware-tools/vgauth/schemas/
%config(noreplace) %{_sysconfdir}/vmware-tools/vgauth.conf
# Users are not expected to edit the example or the schemas.
%config %{_sysconfdir}/vmware-tools/tools.conf.example
%config %{_sysconfdir}/vmware-tools/vgauth/schemas/*
%{_sysconfdir}/vmware-tools/*-vm-default
%{_sysconfdir}/vmware-tools/statechange.subr
%{_sysconfdir}/vmware-tools/scripts
%{_bindir}/VGAuthService
%{_bindir}/vm-support
%{_bindir}/vmhgfs-fuse
%{_bindir}/vmtoolsd
%{_bindir}/vmware-alias-import
%{_bindir}/vmware-checkvm
%{_bindir}/vmware-hgfsclient
%{_bindir}/vmware-namespace-cmd
%{_bindir}/vmware-rpctool
%{_bindir}/vmware-toolbox-cmd
%{_bindir}/vmware-vgauth-cmd
%{_bindir}/vmware-vgauth-smoketest
%{_bindir}/vmware-xferlogs
%{_libdir}/libDeployPkg.so.*
%{_libdir}/libguestlib.so.*
%{_libdir}/libguestStoreClient.so.*
%{_libdir}/libhgfs.so.*
%{_libdir}/libvgauth.so.*
%{_libdir}/libvmtools.so.*
%dir %{_libdir}/%{toolsname}/
%dir %{_libdir}/%{toolsname}/plugins/
%dir %{_libdir}/%{toolsname}/plugins/common/
%{_libdir}/%{toolsname}/plugins/common/*.so
%dir %{_libdir}/%{toolsname}/plugins/vmsvc/
%{_libdir}/%{toolsname}/plugins/vmsvc/*.so
%{_libdir}/%{toolsname}/serviceDiscovery/
%{_datadir}/%{toolsname}/
%{_udevrulesdir}/99-vmware-scsi-udev.rules
%{_unitdir}/%{toolsdaemon}.service
%{_unitdir}/%{vgauthdaemon}.service
%{_modulesloaddir}/open-vm-tools.conf

%files desktop
%{_sysconfdir}/xdg/autostart/vmware-user.desktop
%{_bindir}/vmware-user
%{_bindir}/vmwgfxctrl
%attr(4755,-,-) %{_bindir}/vmware-user-suid-wrapper
%{_bindir}/vmware-vmblock-fuse
%{_libdir}/%{toolsname}/plugins/vmusr/
%{_unitdir}/run-vmblock\x2dfuse.mount
# The RemoteDesktop portal resolves the app id below to this entry, so the
# pointer integration cannot work without it.
%{_datadir}/applications/org.openvm.tools.desktop

%changelog
* Thu Oct 15 2026 Wenyin Root <wenyinos@users.noreply.github.com> - 13.1.0-1
- Package the Wayland fork of open-vm-tools 13.1.0 (build %{toolsbuild}).
- Obsolete and provide open-vm-tools so the two cannot be installed together.
