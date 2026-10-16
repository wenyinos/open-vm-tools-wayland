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
| RPM | AlmaLinux 9 | `gtkmm30-devel`, `wayland-protocols`, `fuse3-devel >= 3.10`, `libmspack-devel`, `xmlsec1-openssl-devel`, `libtirpc-devel`, `rpcgen` |
| DEB | Ubuntu 22.04 LTS | `libgtkmm-3.0-dev`, `wayland-protocols`, `libfuse3-dev >= 3.10`, `libmspack-dev`, `libxmlsec1-dev`, `libtirpc-dev`, `rpcsvc-proto` |
| Pacman | Arch Linux (rolling) | `gtkmm-4.0`, `wayland-protocols`, `fuse3`, `libei` |

The baseline is the oldest release that has everything the build needs, so one
package covers that release and everything newer: the EL9 RPM installs on EL10
and on Fedora, and the 22.04 DEB installs on 24.04 and 26.04.

Two things set that floor, and both are older on EL8 and on Ubuntu 20.04:

- **`libfuse3 >= 3.10`.** The fuse3 check is a hard requirement and the code
  uses FUSE API version 35. EL8 carries fuse3 3.3 and Ubuntu 20.04 carries 3.9,
  so neither can build Shared Folders. Relaxing this would mean either falling
  back to FUSE 2 or dropping `vmhgfs-fuse` and `vmblock-fuse` altogether.
- **Gtkmm 4.** It arrives with EL10, Debian 12 and Ubuntu 24.04. The desktop
  plugins build against Gtkmm 3 or 4, and configure prefers 4 and falls back to
  3, so the older targets build against Gtkmm 3. That is why no
  `--with-gtk4` appears in the packaging.

`wayland-protocols` carries no minimum version. `ext-data-control-v1` only
appeared in 1.39, which EL9 (1.37) and Ubuntu 22.04 (1.25) predate, so its XML
is kept in the tree at `services/plugins/dndcp/waylandClipboard/`. Only
`xdg-shell` still comes from `wayland-protocols`, and that has been in its
stable directory for far longer than any release this targets.

**Pointer integration is optional, and absent from the EL9 and 22.04 builds.**
It needs `libei` to drive the EIS socket the RemoteDesktop portal returns, and
libei does not reach EL10, Debian 12 and Ubuntu 24.04 either. The build takes it
when it is installed rather than requiring it: the RPM spec detects it with
`pkg-config` and adds the build requirement and the portal desktop entry only
when it is there, and the Debian packaging installs that entry only if the build
produced it. Clipboard, drag-and-drop and file transfer work either way; what is
missing without libei is the host telling the guest where to put its pointer.
Install `libei-devel` / `libei-dev` before building to get it.

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
# EL9 and Fedora
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

Only installable packages are produced. The `-debuginfo`, `-dbgsym` and
`-debug` packages that the three toolchains generate by default are switched
off, since the release is meant to carry something you can install rather than
a set of symbols.
