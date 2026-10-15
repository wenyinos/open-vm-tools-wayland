#!/bin/sh
#
# Build the open-vm-tools-wayland RPMs from the current tree.
#
# Usage: packaging/rpm/build.sh [OUTDIR]
#
# Writes the source tarball and the built packages into OUTDIR
# (default: <repo>/dist/rpm). Needs rpmbuild and everything listed in
# BuildRequires; run it inside a Fedora container, or install them with
#   dnf builddep -y packaging/rpm/open-vm-tools-wayland.spec

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
outdir=${1:-$root/dist/rpm}

mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)

tarball=$("$root/packaging/make-tarball.sh" HEAD "$outdir")
srcname=$(basename "$tarball" .tar.gz)
# srcname is open-vm-tools-<version>-<build>, as make-tarball.sh writes it.
version=$(printf '%s' "$srcname" | sed -e 's/^open-vm-tools-//' -e 's/-[0-9][0-9]*$//')
build=$(printf '%s' "$srcname" | sed -e 's/.*-//')

echo "Building $srcname (version $version, build $build)"

topdir=$outdir/rpmbuild
rm -rf "$topdir"
mkdir -p "$topdir/BUILD" "$topdir/BUILDROOT" "$topdir/RPMS" "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS"

cp "$tarball" "$topdir/SOURCES/"
cp "$here/../common/vmtoolsd.service" "$here/../common/vgauthd.service" \
   "$here/../common/open-vm-tools.conf" "$here/vmtoolsd.pam" "$topdir/SOURCES/"
cp "$here/../common/run-vmblock\\x2dfuse.mount" "$topdir/SOURCES/"

rpmbuild -bb "$here/open-vm-tools-wayland.spec" \
	--define "_topdir $topdir" \
	--define "srcname $srcname" \
	--define "toolsversion $version" \
	--define "toolsbuild $build"

find "$topdir/RPMS" -name '*.rpm' -exec cp -v {} "$outdir/" \;
