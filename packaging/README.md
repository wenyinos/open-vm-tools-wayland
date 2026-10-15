# Packaging

Three package formats are built from this tree: RPM, DEB and Pacman. All are
x86_64 only. `.github/workflows/release.yml` builds them on every `v*` tag and
attaches them to the release for that tag; running the workflow by hand builds
the same packages without publishing.

The packages are named `open-vm-tools-wayland` (and `-desktop` for the
per-session plugins). They **replace** the distribution `open-vm-tools`, which
is what makes the two unable to coexist.

## Targets and dependency baseline

| Format | Built on | Needs at build time |
| --- | --- | --- |
| RPM | Fedora 44, Fedora 45 | `wayland-protocols >= 1.39`, `libei-devel`, `gtkmm4.0-devel`, `fuse3-devel >= 3.10` |
| DEB | Debian 13 (trixie), Ubuntu 24.04 LTS, Ubuntu 26.04 LTS | `wayland-protocols >= 1.39`, `libei-dev`, `libgtkmm-4.0-dev`, `libfuse3-dev` |
| Pacman | Arch Linux (rolling) | `wayland-protocols`, `libei`, `gtkmm-4.0`, `fuse3` |

`wayland-protocols >= 1.39` is the hard floor: that release is where
`ext-data-control-v1`, the protocol the clipboard and drag-and-drop work over,
first appeared. The tested versions are Debian 13's 1.44, Ubuntu 24.04's 1.45
and Ubuntu 26.04's 1.47.

`libei` is required by all three formats so that pointer integration is present
in every build. Without it the rest of the Wayland support still compiles, but
the pointer would fall back to nothing in a Wayland session.

## How the replacement works

Each package manager needs a different mechanism. The ones below were tested
against real packages, not taken from documentation.

**RPM** — `Provides: open-vm-tools` plus `Obsoletes: open-vm-tools`, with **no**
`Conflicts`. The unversioned `Obsoletes` matches every version, so a later
distribution build cannot slip in beside this one. Adding `Conflicts` breaks it:
dnf resolves the conflict before it processes the obsoletes and refuses the
transaction with

```
installed package open-vm-tools conflicts with open-vm-tools provided by open-vm-tools-wayland-...
```

**DEB** — `Provides`, `Conflicts` and `Replaces` together. dpkg reports

```
considering removing open-vm-tools in favour of open-vm-tools-wayland ... yes, will remove
```

and the file ownership moves across.

**Pacman** — `provides` and `conflicts` in the PKGBUILD. pacman asks before
removing `open-vm-tools` and proceeds once confirmed.

The RemoteDesktop portal app id is `org.openvm.tools`, deliberately not a VMware
product name, and `/usr/share/applications/org.openvm.tools.desktop` ships in
the desktop package. The portal resolves the app id through that file, so
pointer integration does not work if the desktop package is missing.

## Building by hand

Each format has a script that produces the source tarball from the current git
tree and then runs the native tooling. Run them inside the matching
distribution; `packaging/make-tarball.sh` is shared by all three.

```sh
# Fedora
dnf builddep -y packaging/rpm/open-vm-tools-wayland.spec
packaging/rpm/build.sh

# Debian or Ubuntu (installs Build-Depends through mk-build-deps when run as root)
packaging/debian/build.sh

# Arch
packaging/arch/build.sh
```

Output lands in `dist/<format>/`. `OVT_DEB_TAG=<tag>` folds a distribution tag
into the DEB version, which is how the workflow keeps the three DEB builds from
colliding on the release page; `OVT_NO_DEPS=1` skips the automatic dependency
install.
