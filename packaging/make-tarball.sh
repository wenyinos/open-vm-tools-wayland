#!/bin/sh
#
# Build the source tarball the packages are made from.
#
# The tarball holds the open-vm-tools project directory of a commit or tag,
# named the way every packaging format expects: one top-level directory with
# the project inside it.
#
#   packaging/make-tarball.sh [git-ref] [output-directory]
#
# The default ref is HEAD and the default output directory is the current
# one. The version in the file name comes from configure.ac.

set -eu

ref="${1:-HEAD}"
outdir="${2:-.}"

root="$(git rev-parse --show-toplevel)"
cd "$root"

# The version, as configure declares it: TOOLS_VERSION="13.1.0".
version="$(sed -n 's/^TOOLS_VERSION="\([^"]*\)".*/\1/p' \
	open-vm-tools/configure.ac | head -1)"
test -n "$version" || { echo "ERROR: cannot read the version from configure.ac" >&2; exit 1; }

# The build number, which buildNumber.h carries, e.g. 25218885.
build="$(sed -n 's/^[[:space:]]*\([0-9][0-9]*\)[[:space:]]*$/\1/p' \
	open-vm-tools/lib/include/buildNumber.h | head -1)"
test -n "$build" || { echo "ERROR: cannot read the build number from buildNumber.h" >&2; exit 1; }

name="open-vm-tools-${version}-${build}"
tarball="${outdir%/}/${name}.tar.gz"

mkdir -p "$outdir"

# Archive the project directory itself, so the tarball unpacks to
# $name/configure.ac rather than to $name/open-vm-tools/configure.ac.
#
# The commit's own timestamp goes in, so the same commit always produces the
# same tarball.
git archive \
	--format=tar.gz \
	--prefix="${name}/" \
	--worktree-attributes \
	-o "$tarball" \
	"${ref}:open-vm-tools"

echo "$tarball"
