#!/bin/sh
#
# Build the open-vm-tools-wayland Arch packages from the current tree.
#
# Usage: packaging/arch/build.sh [OUTDIR]
#
# Writes the source tarball and the built packages into OUTDIR
# (default: <repo>/dist/arch). Run it inside an archlinux:base-devel container.
#
# makepkg refuses to run as root, so when invoked as root this drops to an
# unprivileged user for the build itself.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
outdir=${1:-$root/dist/arch}

mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)

tarball=$("$root/packaging/make-tarball.sh" HEAD "$outdir")
srcname=$(basename "$tarball" .tar.gz)
# srcname is open-vm-tools-<version>-<build>, as make-tarball.sh writes it.
version=$(printf '%s' "$srcname" | sed -e 's/^open-vm-tools-//' -e 's/-[0-9][0-9]*$//')
build=$(printf '%s' "$srcname" | sed -e 's/.*-//')

work=$outdir/build
rm -rf "$work"
mkdir -p "$work"

# The checked-in PKGBUILD carries the version and the build number so it stays
# usable on its own; keep it in step with the tree it was generated from.
sed -e "s/^pkgver=.*/pkgver=${version}/" \
    -e "s/^_buildnumber=.*/_buildnumber=${build}/" \
    "$here/PKGBUILD" > "$work/PKGBUILD"
cp "$here/open-vm-tools-wayland.install" \
   "$here/open-vm-tools-wayland-desktop.install" "$work/"
cp "$tarball" "$work/"
cp "$root/packaging/common/vmtoolsd.service" \
   "$root/packaging/common/vgauthd.service" \
   "$root/packaging/common/open-vm-tools.conf" "$work/"
cp "$root/packaging/common/run-vmblock\\x2dfuse.mount" "$work/"
cp "$here/vmtoolsd.pam" "$work/"

echo "Building $srcname (version $version, build $build)"

# Install what the build needs up front rather than letting makepkg -s resolve
# the dependency lists. The desktop package depends on its sibling, which is
# not in any repository while the split package is being built, and makepkg
# cannot install that.
if [ "${OVT_NO_DEPS:-0}" != 1 ] && command -v pacman >/dev/null 2>&1; then
	pacman -S --noconfirm --needed --asdeps \
		autoconf automake libtool pkgconf rpcsvc-proto glib2-devel \
		glib2 gtk4 gtkmm-4.0 libdrm libmspack libtirpc openssl libxml2 xmlsec \
		fuse3 pam libx11 libxext libxi libxinerama libxrandr libxrender \
		libxtst libei wayland wayland-protocols
fi

if [ "$(id -u)" = 0 ]; then
	builder=builder
	if ! id -u "$builder" >/dev/null 2>&1; then
		useradd -m "$builder"
	fi
	chown -R "$builder" "$work"
	chmod 755 "$outdir"
	su "$builder" -c "cd '$work' && makepkg -f --noconfirm"
else
	cd "$work"
	makepkg -f --noconfirm
fi

find "$work" -maxdepth 1 -name '*.pkg.tar.*' -exec cp -v {} "$outdir/" \;
