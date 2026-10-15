#!/bin/sh
#
# Build the open-vm-tools-wayland .deb files from the current tree.
#
# Usage: packaging/debian/build.sh [OUTDIR]
#
# Writes the source tarball and the built packages into OUTDIR
# (default: <repo>/dist/deb). Run it inside a Debian or Ubuntu container.
#
# When run as root with mk-build-deps available (devscripts and equivs
# installed), the Build-Depends listed in debian/control are installed first,
# so the list lives in exactly one place.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
outdir=${1:-$root/dist/deb}

mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)

work=$outdir/build
rm -rf "$work"
mkdir -p "$work"

tarball=$("$root/packaging/make-tarball.sh" HEAD "$work")
srcname=$(basename "$tarball" .tar.gz)
# srcname is open-vm-tools-<version>-<build>, as make-tarball.sh writes it.
version=$(printf '%s' "$srcname" | sed -e 's/^open-vm-tools-//' -e 's/-[0-9][0-9]*$//')

tar -xzf "$tarball" -C "$work"
srcdir=$work/$srcname

# The packaging lives outside the archived subtree, so drop it in place.
cp -a "$here" "$srcdir/debian"
rm -f "$srcdir/debian/build.sh"
chmod +x "$srcdir/debian/rules"

# Builds for several distributions would otherwise carry the same file name.
# Fold the caller's tag into the version so they can sit side by side on the
# release page and still say which distribution they were built for.
if [ -n "${OVT_DEB_TAG:-}" ]; then
	debversion="$version+$OVT_DEB_TAG"
	sed -i "1s/^open-vm-tools-wayland ($version)/open-vm-tools-wayland ($debversion)/" \
		"$srcdir/debian/changelog"
	echo "Building $srcname as $debversion"
else
	echo "Building $srcname"
fi

cd "$srcdir"

if [ "${OVT_NO_DEPS:-0}" != 1 ] && [ "$(id -u)" = 0 ] && command -v mk-build-deps >/dev/null 2>&1; then
	apt-get update
	mk-build-deps --install --remove \
		--tool 'apt-get -y --no-install-recommends' debian/control
fi

DEB_BUILD_OPTIONS="parallel=$(nproc)" dpkg-buildpackage -b -us -uc

find "$work" -maxdepth 1 -name '*.deb' -exec cp -v {} "$outdir/" \;
