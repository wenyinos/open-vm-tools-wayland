# Wayland Support

**English** | [简体中文](WAYLAND.zh-CN.md)

Native Wayland backend for open-vm-tools 13.1.0. Copy and paste and drag and drop
work in both directions in a Wayland session, without falling back to an X11 session.

> Upstream open-vm-tools runs its desktop integration on XWayland by force
> (`gdk_set_allowed_backends("x11")` is called unconditionally in `copyPasteDnDX11.cpp`).
> In a Wayland session it therefore reaches only Xwayland's clipboard, and most
> compositors refuse clipboard reads from an unfocused Xwayland client. This branch
> adds the native backend.

- [What works](#what-works)
- [Pointer integration](#pointer-integration)
- [Compositor support](#compositor-support)
- [Building](#building)
- [Runtime requirements](#runtime-requirements)
- [Environment variables](#environment-variables)
- [What this adds over the community patch set](#what-this-adds-over-the-community-patch-set)
- [Known limits](#known-limits)

## What works

| Feature | Detail |
|---|---|
| Clipboard (host ↔ guest) | Text, RTF, PNG images and file lists, both directions |
| Drag host → guest | A native Wayland drag where the compositor has layer shell, XDND bridged by Mutter otherwise |
| Drag guest → host | A native drop target, or XDND on X11 sources |
| File copy and paste | Guest to host transfers directly; host to guest reads lazily through vmblock |
| Automatic resolution / multi-monitor | Uses `resolutionKMS`, with the Wayland role conflict fixed |
| Pointer integration | Through the RemoteDesktop portal and libei; see below |
| X11 sessions | Upstream behaviour is preserved in full |

The clipboard backend offers every format the host sent at once, so one copy
carries RTF and plain text together: pasting rich text into a plain text editor
yields text, and into a rich text editor keeps the formatting.

## Pointer integration

The host has to be able to move the guest pointer: when it grabs the pointer it
puts it where the host says. On X11 that is `XWarpPointer`. A Wayland client may
not move the pointer itself, so this goes through the desktop's
**RemoteDesktop portal**, which returns an EIS socket that libei drives. That
gives an absolute pointer device to move, and the size of the area it moves in.

Note that the position can be **written but not read**: libei's sender context
sends events and does not receive them, and the portal has no position query. So
the guest position cannot be reported back to the host on Wayland. On X11 that
report is what lets the host place the host pointer where the guest pointer was
when it is released; on Wayland it is absent, and nothing else depends on it.

Three things have to be in place, each of them found by running the flow:

1. **libei.** Detected at configure time. When it is missing only the pointer
   integration is left out, with a warning, and the rest of the Wayland support
   still builds. Install `libei-devel` or the distribution's equivalent.

2. **A .desktop file.** The portal remembers a grant against an application id,
   and that id has to name an installed `.desktop` file. This package installs
   `org.openvm.tools.desktop` and the plugin registers `org.openvm.tools` before
   its first portal call. **The id is this package's own, not a VMware product
   name**, so the permissions belong to this package and do not affect the
   official VMware applications.

3. **One consent dialog, once.** The grant is requested with
   `persist_mode=PERSISTENT`, so the desktop stores it and does not ask again on
   later logins. It can be reviewed and revoked in the desktop's permission
   settings; on KDE it is the `kde-authorized` / `remote-desktop` entry.

> **Packaging note:** the `org.openvm.tools.desktop` file this package installs
> has a different name from the one official open-vm-tools provides, so the two
> do not overwrite each other. Both do install the same plugin files such as
> `libdndcp.so`, though, so the packages **must declare a conflict and a
> replacement (`Conflicts`/`Replaces`/`Provides`) and only one may be installed
> at a time**.

## Compositor support

What you get depends on the protocols the compositor implements, not on this
build.

| What the compositor offers | Clipboard | Drag host → guest | Drag guest → host |
|---|---|---|---|
| `ext-data-control-v1` + `zwlr_layer_shell_v1` | native, all formats | native | native |
| `ext-data-control-v1` only | native, all formats | XDND, bridged | XDND, bridged |
| `zwlr-data-control-unstable-v1` only | native, all formats | XDND, bridged | XDND, bridged |
| GNOME (Mutter), no data control | through Xwayland's X selection | XDND, bridged | XDND, bridged |
| neither | off | X11 applications only | X11 sources only |

Verified environments:

| Environment | Result |
|---|---|
| KDE Plasma 6.7.5 (KWin), Wayland | clipboard and drag and drop, both directions, all formats |
| niri 26.04, nested session | clipboard both directions, including non-ASCII text |
| GNOME (Mutter) | text and files verified by the upstream patch set |

## Building

### Dependencies

Beyond the usual open-vm-tools dependencies you need:

- `wayland-client` (libwayland-client)
- `wayland-scanner`
- `wayland-protocols` **>= 1.39** (`ext-data-control-v1` first appeared there)
- A GTK 4 or GTK 3 development package (the Wayland backend is built with the
  desktop plugins)
- `libei` (optional; without it only the pointer integration is left out)

Fedora / RHEL:

```bash
sudo dnf install gtkmm4.0-devel libsigc++30-devel glibmm2.68-devel \
    wayland-devel wayland-protocols-devel libei-devel
```

Debian / Ubuntu:

```bash
sudo apt install libgtkmm-4.0-dev libsigc++-3.0-dev libglibmm-2.68-dev \
    libwayland-dev wayland-protocols
```

### Compiling

```bash
autoreconf -i
./configure --with-gtk4 --with-wayland --enable-resolutionkms
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

Wayland support defaults to `auto`: when the libraries above are missing,
configure warns and builds without it. Asking for it explicitly turns a missing
dependency into an error:

```bash
./configure --with-wayland     # error if a dependency is missing
./configure --without-wayland  # build without the Wayland backend, as upstream
```

Without `--with-gtk4` or `--with-gtk3`, configure probes for either and prefers GTK4.

> **Build note:** 13.1.0 does not compile as-is with GCC 16 and libsigc++ 3.8,
> which removed the `signal<R, T...>` syntax and made it `final`. This branch
> carries the compatibility fixes, so no extra patch is needed.

## Runtime requirements

1. **The vmblock service is mounted**, which host to guest file paste and file
   drags need:

   ```bash
   sudo systemctl enable --now run-vmblock\\x2dfuse.mount
   ```

2. **vmusr must be started through `vmware-user-suid-wrapper`**, not by running
   `vmtoolsd -n vmusr` directly. The wrapper opens the vmblock and `/dev/uinput`
   descriptors as root and passes them on; drag and drop under Wayland needs
   uinput to inject pointer events.

   The distribution's XDG autostart entry
   (`/etc/xdg/autostart/vmware-user.desktop`) already does this.

3. **Xwayland must be running.** vmusr loads upstream's desktopEvents plugin,
   which cannot start without an X display.

4. The guest needs the `open-vm-tools-desktop` package, which carries the desktop
   plugins.

**To check it took effect.** The backend logs the protocol it bound to at debug
level (`g_debug`), which the default log level does not include. Turn it on in
`/etc/vmware-tools/tools.conf`:

```ini
[logging]
vmusr.level = debug
```

Then restart vmusr in the session (log out and back in, or
`systemctl --user restart vmtoolsd` depending on the distribution). One of these
should appear in the log:

```
WaylandClipboard_Init: using ext-data-control-v1
WaylandClipboard_Init: using zwlr_data_control_manager_v1
```

On Linux vmusr logs to `/var/log/vmware-vmusr-<user>.log` by default. The message
`no data control protocol is available` means the compositor implements neither
protocol, and the clipboard falls back to the X selection path, which only works
on GNOME.

> A quicker check: copy **text containing non-ASCII characters** in the guest and
> paste it to the host. Older builds always failed here (see issue #800 below),
> so a successful paste means the backend is in use.

## Environment variables

| Variable | Effect |
|---|---|
| `VMTOOLS_DATA_CONTROL_PROTOCOL` | Pins the data control protocol the clipboard uses, either `ext_data_control_manager_v1` or `zwlr_data_control_manager_v1`. Use it when a compositor's implementation of one protocol is broken and you need the other, or to test a specific path on a compositor that offers both. |

It is not needed normally. Where both protocols are offered,
`ext-data-control-v1` is preferred.

## What this adds over the community patch set

The Wayland backend here comes from the community patch set
[goproslowyo/ovt-wayland](https://github.com/goproslowyo/ovt-wayland), with three
additions:

1. **`zwlr-data-control-v1` support for older compositors.**
   The community patch set binds only `ext-data-control-v1`, which first appeared
   in wayland-protocols 1.39 (2024). Long-term-support releases such as Plasma 5.27
   and sway 1.9 offer only `zwlr-data-control`, and the clipboard stayed off
   entirely there. This branch adds `wayland-data-control.c`, which drives both
   protocols from one implementation: the two have the same interfaces, the same
   requests in the same order and the same events in the same order, and differ
   only in the interface each names when creating an object, so no code is
   duplicated. Where both are offered, the standard `ext-data-control-v1` is
   still preferred.

2. **Non-ASCII clipboard text fixed (upstream issue #800).**
   `copyPasteUIX11GTK4.cpp` passed `Glib::ustring::size()`, a character count, to
   `CPClipboard_SetItem()`, which wants bytes. Copying text containing anything
   non-ASCII to the host failed; when the cut landed inside a multibyte sequence
   the whole copy was dropped. The GTK3 backend already used `bytes()`.

3. **Multi-monitor fixed under Wayland (upstream issue #805).**
   In a Wayland session `resolutionSet` reaches only Xwayland, whose RandR outputs
   cannot be set, yet it claimed the `resolution_server` role from `resolutionKMS`
   when it started. The host's `DisplayTopology_Set` then landed on a backend that
   could not apply it, and "Cycle Multiple Monitors" never worked. It now backs off
   in a Wayland session.

It also carries the three build compatibility patches Fedora's RPM uses
(GCC 16, libsigc++ 3.8, OpenSSL 4); without them 13.1.0 does not compile on a
current Fedora.

## Known limits

These come from the inherited patch set and are not resolved:

- **A drag released too soon is lost.** Letting go within about 100 ms of entering
  the VM makes the host cancel the drag before the guest learns the pointer position.
- **Drags lag on GNOME.** A host to guest drag starts about half a second after the
  pointer enters the VM; an earlier drop waits. On GNOME, after a guest to host drag,
  move the pointer over the guest once before dragging from the host, or the host
  holds the guest mouse button down and the drag fails.
- **Display scaling offsets a drag.** If the VMware window scales the guest display,
  a host to guest drag lands beside the host pointer. Use a guest resolution that
  fits the window (View → Autosize → Autofit Guest).
- **Dropped files expire after two minutes.** An application that reads a dropped
  file more than two minutes after the drop, such as a browser upload form, finds
  it gone. Drop into a file manager first.
- **Tiling compositors stretch the detection window.** Float the window titled
  `vmware-user`, for example in sway: `for_window [title="vmware-user"] floating enable`.
- **Format details.** A clipboard carrying only RTF pastes only into applications
  that accept RTF, and `:` in a host-supplied file name is rewritten to `^%`
  (a Windows host limitation).
- **Pasted files keep the permissions the host reports**, so a read-only file
  arrives read-only (upstream behaviour).
- **A kernel-side resolution issue remains.** When the host logs
  `guestScreens not congruent with hostScreens`, it may be `vmwgfx` not updating
  active screen positions after `DRM_VMW_UPDATE_LAYOUT`, which is a kernel driver
  matter.
- **The drag timings are measured values.** If a compositor needs different
  ones a drag can start late or be refused; they can be adjusted in the `[dnd]`
  section of `tools.conf` (see below) without a rebuild.

## Drag timing adjustments

Each step of a drag waits for the step before it, and the waits were measured on
the compositors the feature was tested with. What a compositor needs varies, so
they can be adjusted in the `[dnd]` section of `/etc/vmware-tools/tools.conf`:

```ini
[dnd]
# How long the pointer stays on the detection window. Mutter takes the
# selection while it is there, so leaving earlier loses the drag.
#xdndLeaveDelayMs=300
# How long to wait for GTK to start an XDND drag.
#xdndDragBeginTimeoutMs=500
# How long a native drag target has to accept before the button goes up.
#waylandDragAcceptTimeoutMs=500
# How long a bridged XDND drop waits for the target to accept.
#xdndDropAcceptTimeoutMs=1500
```

A value that is not a positive number is refused and the default is used, so a
typo cannot turn a wait into a busy loop. With nothing set the behaviour is
unchanged.
