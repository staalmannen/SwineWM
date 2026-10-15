# SwineWM

SwineWM is an alternative shell for Microsoft Windows that works like a
[Sway](https://swaywm.org/) desktop: a keyboard-driven tiling window manager
with workspaces, a [Waybar](https://github.com/Alexays/Waybar)-style bar and
[Mako](https://github.com/emersion/mako)-style notifications. The aim is that
Sway users can bring their config and habits to Windows.

## Approach

SwineWM is a **from-scratch, Windows-native clone**, not a port. It manages
ordinary Windows application windows through Win32 APIs and leaves
compositing to Windows (DWM). Sway, Waybar and Mako are the specification:
their sources are included as submodules and used as reference for
behaviour, config syntax, IPC protocol and defaults, which SwineWM
reproduces as faithfully as Windows allows. The earlier plan of porting
Wayland, wlroots and Sway to Windows is archived in
[`doc/archive/`](doc/archive/).

| Linux | SwineWM | Status |
| --- | --- | --- |
| sway | `swinewm` — tiling window manager | Early draft, runs |
| swaymsg | `swinemsg` — IPC client | Early draft, runs |
| waybar | `swinebar` — status bar | Planned |
| mako | `swinenotify` — notifications | Planned |

## Repository layout

| Path | What |
| --- | --- |
| [`swinewm/`](swinewm/) | The window manager and `swinemsg`, in C11 ([README](swinewm/README.md)) |
| [`doc/`](doc/) | [Project summary](doc/project_summary.md), [architecture](doc/architecture.md), [Sway compatibility](doc/compatibility.md) |
| [`sway-hwnd/`](sway-hwnd/), [`windows/`](windows/) | Earlier C++ prototype of an HWND tiling shell, kept for testing and comparison |
| `sway/`, `waybar/`, `mako/` | Upstream sources, used as reference |
| `wlroots/`, `wayland/`, `wayland-protocols/`, `cairo/`, `pango/`, `dbus/`, `json-c/`, `pcre2/`, `gdk-pixbuf/` | Dependencies imported for the abandoned port; no longer used |

Clone with `git clone --recurse-submodules`, or run
`git submodule update --init sway waybar mako` to get just the reference
sources.

## Status

`swinewm` implements Sway's layout tree, focus model and most everyday
commands (focus, move, split, layout, workspaces, floating, fullscreen,
scratchpad, resize, gaps, modes, exec), loads Sway config files and answers
i3/Sway IPC requests over a named pipe. Its core is unit-tested on Linux and
Windows, and it has been smoke-tested under Wine; it has not yet been used
on a real Windows desktop. Not yet done: title bars and borders, mouse
bindings, criteria rules, output configuration, IPC events, the bar and the
notification daemon. See [`doc/compatibility.md`](doc/compatibility.md).

To try it, see [`swinewm/README.md`](swinewm/README.md). It runs as an
ordinary program next to Explorer and shows all windows again when it exits.

## License

MIT; see [LICENSE](LICENSE). Code ported from Sway, Waybar or Mako (also MIT)
keeps the original copyright notice.
