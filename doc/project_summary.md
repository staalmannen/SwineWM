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

On Linux these are separate programs talking over IPC and D-Bus. SwineWM
deliberately does **not** follow the one-tool-per-task split: the window
manager, the bar and the notification daemon are one program, `swinewm.exe`.
That avoids reimplementing D-Bus, keeps a single process to start as the
shell, lets the bar read the layout tree directly instead of over IPC, and
means one crash handler restores everything. The *configuration* still
follows the Linux programs, so existing files keep working:

| Linux | In SwineWM | Configured by | Status |
| --- | --- | --- | --- |
| `sway` | `swinewm.exe` window manager | sway's `config` | Draft working: see [`compatibility.md`](compatibility.md) |
| `waybar` | built-in bar | Waybar's `config.jsonc` + `style.css` | Draft working |
| `mako` | built-in notifications | mako's `config` | Draft working |
| `notify-send`, `makoctl` | swinewm commands (also via `exec` and `swinemsg`) | — | Draft working |
| `swaymsg` | `swinemsg.exe` | — | Draft working |
| `swaynag` | — | — | Later, likely built in as well |
| `wmenu`/`fuzzel` | — | — | Not planned for now: the default config uses [Flow Launcher](https://www.flowlauncher.com) |

The code still keeps the parts apart: [`../swinewm/swinewm/bar/`](../swinewm/swinewm/bar/)
and [`../swinewm/swinewm/notify/`](../swinewm/swinewm/notify/) are separate
modules next to the window-manager code, each with a portable core and a
Win32 renderer.

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
   Windows machines.
3. **Integrated bar and notifications** *(draft)* — Waybar's config and a
   CSS subset of its `style.css`; modules `sway/workspaces`, `sway/mode`,
   `sway/window`, `sway/scratchpad`, `clock`, `cpu`, `memory`, `battery`,
   static `custom/*`. Mako's config with criteria, modes, history,
   timeouts; `notify-send` and `makoctl` as commands.
4. **Daily-drivable WM** — focus-follows-mouse, mouse bindings and
   `floating_modifier` drag/resize, tab/stack title bars and focus borders,
   criteria (`[app_id=…]`, `for_window`, `assign`), `output` configuration,
   IPC event subscriptions, a swaynag equivalent, `slurp` (area selection
   for the built-in `grim`).
5. **Richer bar and notifications** — `custom/*` modules running scripts,
   network, audio (pulseaudio module on Windows' audio API), tray
   tooltips and hover, translucent backgrounds; notification icons,
   actions, grouping, and receiving Windows' own toast notifications.
6. **Shell replacement** *(draft)* — `swinewm --set-shell` runs SwineWM
   instead of Explorer at logon: shell window, logon "ready" event,
   startup programs, work area, `exit` logs off, Explorer as a rescue after
   a crash, and the tray (notification area icons, including Windows'
   volume/network/power icons). Next: AppBar support.
   See [`research/windows-shell.md`](research/windows-shell.md).

## Open design questions

- **Where notifications come from.** Mako receives notifications over
  D-Bus. On Windows, applications send toast notifications to the system.
  Today swinewm shows what is sent with `notify-send` (a command, also
  reachable through `exec` and `swinemsg`). Mirroring system toasts through
  the WinRT `UserNotificationListener` (needs packaging and user consent)
  is the candidate for the rest. When Explorer is not the shell, whether
  system toasts are still displayed needs to be tested.
- **The bar and Explorer's taskbar.** The bar reserves its space inside
  swinewm's own layout. Windows that swinewm does not manage (maximized
  unmanaged windows, Explorer's taskbar) do not know about it; registering
  as an AppBar (`SHAppBarMessage`) would fix that while Explorer runs.
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
  Education and IoT editions. SwineWM uses the per-user `Shell` value under
  `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon`
  (`--set-shell`), which needs no administrator rights; this has to be
  confirmed on Home editions.
- **Packaged apps as the shell.** As the shell, Microsoft Store/MSIX apps
  start fine through their App Execution Alias (`start firefox`,
  `exec firefox.exe`), but opening them as `shell:AppsFolder\<AUMID>`
  fails with "Class not registered" for every caller (tested with
  PowerShell `Start-Process`), so launchers that use it (Flow Launcher)
  cannot start them. Most likely a COM class that Explorer registers while
  it runs (its immersive shell) is missing. Open: whether
  `IApplicationActivationManager::ActivateApplication` works without
  Explorer; if so swinewm's `exec` could use it for `shell:AppsFolder`
  paths, and launchers could be asked to do the same.
- **Tray icons next to Explorer.** The `tray` module takes the tray icons
  over from Explorer (see [`architecture.md`](architecture.md)). Explorer's
  own tray then shows stale icons until swinewm exits. Hiding its taskbar
  (`explorer_taskbar autohide`) avoids the confusion; whether the tray
  should do that by itself is open.

## Using the reference sources

The submodules are documentation. Notes on how other Windows shells solve
Windows-specific problems (from Cairo Desktop / ManagedShell) are in
[`research/windows-shell.md`](research/windows-shell.md). The submodules:

- [`../sway/`](../sway/) — `sway/tree/` (layout tree), `sway/commands/`
  (one file per command), `sway/config.c`, `sway/input/seat.c` (focus
  stack), `sway/ipc-*.c`, `config.in`, and the man pages
  (`sway/sway.5.scd`, `sway-ipc.7.scd`).
- [`../waybar/`](../waybar/) — module list and config format
  (`man/*.scd`, `resources/config.jsonc`, `resources/style.css`,
  `src/modules/sway/`).
- [`../mako/`](../mako/) — config format (`mako.5.scd`), criteria, styling
  and `makoctl`.

Other alternative Windows shells and tiling WMs are useful to see how
Windows-specific problems were solved, e.g. komorebi, GlazeWM and Zebar,
workspacer, bug.n and FancyWM. Their licences differ (GPL-3.0 for GlazeWM,
a source-available non-OSI licence for recent komorebi releases, MIT for
workspacer), so read them for ideas and check the licence before copying
any code into this MIT-licensed project.
