# SwineWM — project summary

## Vision

SwineWM is an alternative shell for Microsoft Windows that works like a
[Sway](https://swaywm.org/) desktop: a keyboard-driven tiling window manager
with numbered workspaces, a [Waybar](https://github.com/Alexays/Waybar)-style
status bar and [Mako](https://github.com/emersion/mako)-style notifications.
Someone who lives in Sway on Linux should be able to bring their config and
their muscle memory to Windows and feel at home.

## Strategy: a native clone, not a port

The project started as an attempt to port Wayland, wlroots and Sway to
Windows (see [`archive/`](archive/)). That path needs a Windows backend for
wlroots, a Wayland transport and event loop, buffer sharing, input devices,
and a bridge for native windows. Native Windows applications would still not
be Wayland clients. The amount of platform work is very large compared to
what the user eventually sees.

SwineWM is now a **from-scratch Windows program that copies Sway's
behaviour**, not its platform layer:

- The window manager manages ordinary Windows application windows (HWNDs)
  through documented Win32 APIs. DWM keeps compositing them; there is no
  Wayland and no compositor of our own.
- Sway, Waybar and Mako are **the specification**. Their source is checked
  out as submodules and read freely: the tree algorithms, command syntax,
  config format, IPC protocol and defaults are reproduced as faithfully as
  Windows allows. This is not a clean-room project; code may be ported from
  them where that is the most faithful option (all three are MIT licensed,
  so keep their copyright notice on ported code).
- Code is written in C11, mirrors Sway's structure and naming where it can,
  and keeps the window-manager core portable and unit-tested. Only a thin
  platform layer touches Win32.

Fidelity goals, in priority order:

1. **Behaviour.** The same command does the same thing to the layout tree:
   where new windows go, how `split`, `layout`, `focus`, `move`,
   `workspace`, `floating`, `fullscreen` and `scratchpad` behave.
2. **Configuration.** An existing `~/.config/sway/config` loads. Commands
   that make no sense on Windows are accepted and ignored rather than
   rejected, and unsupported ones are reported with line numbers.
3. **IPC.** The i3/Sway IPC protocol (same message numbers, same JSON shape)
   over a named pipe, so `swaymsg`-style scripts and IPC libraries work
   with little change. Waybar's Sway modules rely on it too.
4. **Look and feel** of the bar and notifications, configured with Waybar's
   JSON + CSS and Mako's ini-style config.

## Components

Each Linux program gets a Windows counterpart with the same role. Names are
working titles.

| Linux | SwineWM | Role | Status |
| --- | --- | --- | --- |
| `sway` | `swinewm` | Tiling window manager, config, key bindings, IPC server | Draft working: see [`compatibility.md`](compatibility.md) |
| `swaymsg` | `swinemsg` | IPC command-line client | Draft working |
| `waybar` | `swinebar` | Status bar: workspaces, window title, clock, tray, … | Planned |
| `mako` / `makoctl` | `swinenotify` / `swinenotifyctl` | Notification daemon and control tool | Planned; design question below |
| `swaynag` | `swinenag` | Warning/confirmation bar (e.g. exit, config errors) | Later |
| `wmenu`/`fuzzel` | — | Launcher | Later; `explorer shell:AppsFolder` meanwhile |

`swinewm` and `swinemsg` live in [`../swinewm/`](../swinewm/), which mirrors
the layout of the sway repository. The bar and notification daemon will be
separate programs in sibling directories (`swinebar/`, `swinenotify/`) that
talk to `swinewm` over IPC, just as Waybar talks to Sway.

## Non-goals

- Running Wayland or X11 applications. (WSLg already does that.)
- Replacing DWM or compositing windows ourselves.
- Undocumented shell internals (the virtual-desktop COM interfaces,
  injecting into Explorer). Prefer public, stable APIs; if something can
  only be done through undocumented interfaces, isolate it and make it
  optional.
- Pixel-identical rendering of GTK widgets. The bar should honour Waybar's
  config and the commonly used parts of its CSS, not embed GTK.

## Roadmap

1. **Window-manager core** *(done as a draft)* — Sway's tree model, seat
   focus stack, arrange, the common commands, config parser with `set`,
   blocks, `mode`, `include` and deferred `exec`, unit tests.
2. **Win32 platform layer** *(draft)* — window discovery, hide-based
   workspaces, low-level keyboard hook bindings, monitor/work-area outputs,
   crash recovery of hidden windows, named-pipe IPC. Needs testing on real
   Windows machines; `sway-hwnd/` is the earlier prototype for comparison.
3. **Daily-drivable WM** — focus-follows-mouse, mouse bindings and
   `floating_modifier` drag/resize, tab/stack title bars and focus borders
   drawn by swinewm, criteria (`[app_id=…]`, `for_window`, `assign`),
   `output` configuration, IPC event subscriptions, `swinenag`.
4. **swinebar** — Waybar config format, Sway modules over IPC (workspaces,
   mode, window), clock, CPU/memory, battery, network, audio, tray. Uses
   the AppBar API (`SHAppBarMessage`) so tiled windows avoid it.
5. **swinenotify** — Mako config format and look, `swinenotifyctl`, plus a
   way for applications to send notifications (see below).
6. **Shell replacement** — run SwineWM instead of Explorer at login on
   editions that support it, with a tested way back to Explorer.

## Open design questions

- **Where notifications come from.** Mako receives notifications over
  D-Bus. On Windows, applications send toast notifications to the system.
  Options: (a) our own `notify-send`-style CLI/pipe API, (b) mirroring
  system toasts through the WinRT `UserNotificationListener` (needs
  packaging and user consent), (c) both. When Explorer is not the shell,
  whether system toasts are still displayed needs to be tested.
- **Hiding versus cloaking.** Workspaces currently hide windows
  (`SW_HIDE`), which also removes them from the taskbar and Alt+Tab. That
  matches Sway, but some applications react to being hidden. Moving them
  off-screen or minimizing are fallbacks to evaluate per application.
- **Win+L and other reserved shortcuts.** No program can intercept Win+L or
  Ctrl+Alt+Del. Sway's default `$mod+l` therefore locks the machine unless
  `$mod` is changed or the `DisableLockWorkstation` policy is set.
- **Elevated windows.** A non-elevated process cannot move or hide windows
  of elevated ones (UIPI). Running elevated, or skipping such windows, are
  the options.
- **Shell activation.** Shell Launcher is only available on Enterprise,
  Education and IoT editions; other editions need the `Shell` registry
  value under `Winlogon`. Treat this as deployment, separate from the WM.

## Using the reference sources

The submodules are documentation. The ones that matter now:

- [`../sway/`](../sway/) — `sway/tree/` (layout tree), `sway/commands/`
  (one file per command), `sway/config.c`, `sway/input/seat.c` (focus
  stack), `sway/ipc-*.c`, `config.in`, and the man pages
  (`sway/sway.5.scd`, `sway-ipc.7.scd`).
- [`../waybar/`](../waybar/) — module list and config format
  (`man/*.scd`, `resources/config.jsonc`, `resources/style.css`,
  `src/modules/sway/`).
- [`../mako/`](../mako/) — config format (`mako.5.scd`), criteria, styling
  and `makoctl`.

`wlroots`, `wayland`, `wayland-protocols`, `cairo`, `pango`, `dbus`,
`json-c`, `pcre2` and `gdk-pixbuf` were imported as build dependencies for
the port. They are no longer needed and can be removed when convenient.

Other alternative Windows shells and tiling WMs are useful to see how
Windows-specific problems were solved, e.g. komorebi, GlazeWM and Zebar,
workspacer, bug.n and FancyWM. Their licences differ (GPL-3.0 for GlazeWM,
a source-available non-OSI licence for recent komorebi releases, MIT for
workspacer), so read them for ideas and check the licence before copying
any code into this MIT-licensed project.
