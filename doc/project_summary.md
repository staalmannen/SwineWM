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

The next strategic emphasis should be reliability and Windows-boundary
validation before broadening feature parity. A shell must be recoverable
after a bad start or crash, and each Windows integration needs a repeatable
native smoke scenario in addition to portable core tests. See
[`design_strategy.md`](design_strategy.md) for proposed release gates,
compatibility rules and diagnostics priorities.

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
| `notify-send`, `makoctl` | `notify-send.exe`, `makoctl.exe` (IPC clients), also swinewm commands | — | Draft working |
| `swaymsg` | `swinemsg.exe` | — | Draft working |
| `swaynag` | Native config-error nag window | — | Persistent error details with reload and dismiss actions |
| `wofi`/`wmenu` | `swinemenu.exe`, a separate small program, the default `$menu` | — (colours and font from the bar, over `get_bar_config`) | Draft working: drun (Start menu and Store apps), run (PATH) and dmenu modes; the choice is started by swinewm over IPC |

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

SwineWM runs as the Windows shell (`--set-shell`). Some of Windows 11's
own dialogs, search and Store app launching rely on Explorer's
undocumented shell services, which only run when Explorer is the shell.
Rather than reimplementing those, SwineWM fills the gaps one operation at a
time with documented APIs (for example `IApplicationActivationManager` for
Store apps) and accepts the rest as known limitations. Running next to a
hidden Explorer (`--set-autostart`) was tried and is experimental: Explorer
kept many key combinations and the combination broke easily.

Where things stand: the core, the Win32 layer, the bar, notifications,
the tray, grim and shell mode work as drafts and have been used on a real
Windows 11 machine as the shell. `compatibility.md` is the current feature
matrix; roadmap items are goals, not a promise that every upstream detail is
implemented. The milestones below lead to a 1.0 that
can be maintained as a long-term release; each milestone is a usable
release with an (unsigned) MSI on GitHub Releases. Versions always have
three parts (`0.2.0`, tagged `0.2.0`); fixes in between are patch
releases (`0.1.1`). Details per command are in
[`compatibility.md`](compatibility.md).

### 0.1.0 — first preview (released)

Make what exists dependable before adding more.

- Release engineering: version from the git tag (every CI build is
  "0.0.1" today), a release workflow that attaches the MSI and zip to a
  GitHub Release, a `CHANGELOG.md`.
- Shell-mode fixes from real use: wallpaper, dialogs placed off-screen,
  Win+S/W/E, the "restart" update path.
- A `swinewm.log` that is useful by default (warnings and lifecycle
  events), and `-d` for the shell via an environment variable or config.

### 0.2.0 — sway configs that real people have (released)

What an existing sway config needs to load and behave.

- Criteria: `[app_id=… class=… title=… con_mark=… floating tiling]`,
  `for_window`, `assign`, `no_focus`; app_id/class/title mapped to the
  executable, window class and title.
- `mark`/`unmark`, `swap`, `sticky`, `rename workspace`, `focus prev|next`,
  `resize set`, `resize grow|shrink` in all directions.
- Outputs: `output <name> position|resolution|scale|transform` (through
  `ChangeDisplaySettingsEx`, where Windows allows), `workspace … output …`,
  `move workspace to output`, `focus output`, monitor hot-plug.
- `include` with glob patterns; a native swaynag-style config-error window.

### 0.3.0 — mouse and decorations (released)

- `focus_follows_mouse`, `mouse_warping`, `floating_modifier` drag and
  resize, mouse `bindsym` (button and wheel), and `bindcode` with numeric
  Windows virtual-key values.
- Title bars for tabbed/stacked containers (clickable tabs), focus
  borders and `client.*` colours, `default_border`, `title_format`,
  `hide_edge_borders`, `smart_borders`. Borders through DWM's own border
  colour (`DWMWA_BORDER_COLOR`, 1 px) rather than extra layered windows,
  square corners for tiled windows, `border none/pixel` by removing the
  title bar, and `opacity` with layered-window alpha; all undone on exit,
  crash and `--recover` (see [`research/glazewm.md`](research/glazewm.md)).

### 0.4.0 — IPC parity (released)

- `SUBSCRIBE` with workspace, window, mode, output, binding, shutdown and
  tick events, matching sway-ipc(7), so i3/sway IPC libraries and scripts
  (autotiling, workspace renamers) work against the pipe.
- The remaining IPC replies.
- A watchdog: a second process monitors the manager's message-loop
  responsiveness and shows every window again if swinewm is killed or hangs,
  which an in-process crash handler cannot cover (as GlazeWM's watcher does).

### 0.5.0 — Waybar beyond the basics (released)

- `custom/*` running scripts (`exec`, `interval`, `return-type json`,
  `signal` via `pkill -RTMIN+n` equivalents), which most Waybar setups use.
- `network`, `pulseaudio` (Core Audio), `disk`, `temperature`,
  `idle_inhibitor`, `keyboard-state`, `sway/language` (keyboard layout),
  `power-profiles-daemon` → Windows power mode.
- Tooltips, `:hover`, real transparency (layered windows), multiple bars,
  `left`/`right` positions, `rotate`.
- Tray: tooltips and hover messages. (Scrolling has no counterpart:
  Windows' notification area sends programs no scroll messages.)

### 0.6.0 — Mako, grim and the rest of the desktop

- Mako: icons, actions (`makoctl invoke`/`menu`), `border-radius`,
  progress bars, grouping, `makoctl list|history` output. Done:
  `notify-send.exe` and `makoctl.exe`, all of mako's criteria and
  bindings. Still open: markup inside a line, freedesktop icon themes.
- Notifications from Windows applications: mirror toasts
  (`UserNotificationListener`), or at least show balloon/toast fallbacks
  as the shell. Done without packaging: the listener is read (its change
  event needs package identity), `windows-notifications=` in the mako
  config. Still open: toast buttons and opening the app on a click.
- grim: `slurp` (drag an area), clipboard output (the `wl-copy` habit),
  `grim -` once `exec` supports pipes. Done: slurp, `| wl-copy`,
  `$(slurp)` and grimshot as built-ins.
- Store apps as the shell: done for `exec shell:AppsFolder\<AUMID>`
  (`IApplicationActivationManager`) and swinemenu; still open for launchers
  such as Flow Launcher, which start them their own way.
- swinemenu: icons, wofi's `config` and `style.css`, more modes (windows,
  like `rofi -show window`). Done.
- Next to Explorer (`--set-autostart`, experimental): register the bar as an
  AppBar so that Explorer reserves its space for every window, and find out
  which keys Explorer still takes.

### 1.0.0 — long-term release

Feature work stops; reliability and documentation.

- Tested matrix: Windows 10 22H2 and Windows 11 (current and previous
  feature update), single and multiple monitors, mixed DPI, as the shell
  and next to Explorer, sleep/resume, fast user switching, elevated
  windows (documented behaviour), RDP.
- Compatibility promise: config and IPC behaviour of 1.0 stay supported
  through 1.x; `compatibility.md` is the contract.
- Man pages / reference docs (`swinewm(5)`, `swinemsg(1)`, built-in
  Waybar/Mako/grim differences), a migration guide from a Linux sway
  setup.
- Signed MSI (Authenticode; free signing for open-source projects is
  offered by e.g. SignPath, or Azure Trusted Signing), and packages for
  winget, Scoop and Chocolatey.
- 1.x patch releases for bugs and Windows updates only; new features go
  to 2.0.

## Open design questions

- **Where notifications come from.** Mako receives notifications over
  D-Bus. On Windows, applications send toast notifications to the system.
  swinewm shows what is sent with `notify-send`, tray balloons, and
  mirrors system toasts read through the WinRT `UserNotificationListener`
  (user consent; reading works without packaging, only its change event
  needs package identity, so swinewm watches Windows' notification store
  and polls). Whether Windows still shows its own toast banners when
  Explorer is not the shell needs to be tested; `auto` assumes it does not.
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
  PowerShell `Start-Process`, and with `explorer.exe shell:AppsFolder\…`),
  so launchers that use it (Flow Launcher) cannot start them. Most likely a
  COM class that Explorer registers while it is the shell is missing. The
  documented `IApplicationActivationManager::ActivateApplication` does work
  without Explorer (tested on Windows 11), so swinewm's `exec` uses it for
  `shell:AppsFolder\<AUMID>`; launchers could be asked to do the same.
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
