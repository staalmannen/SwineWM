# Changelog

All notable changes to SwineWM. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/) (0.x: anything may still
change). A release is made by pushing a tag (`0.1.0`); CI then publishes
the MSI and zip with the matching section below as release notes.

## [Unreleased]

## [0.4.0] - 2026-10-24

### Added

- IPC trees, workspaces, outputs and window/workspace events now carry all
  of sway's and i3's node fields (`urgent`, `floating`, `scratchpad_state`,
  `orientation`, `deco_rect`, `window_rect`, `geometry`, `shell`,
  `representation`, `window_properties.instance`, …) in sway's order, so
  i3 and sway IPC scripts find what they expect. `percent` is now worked
  out as in sway.

- A native config-error nag window with the complete error list and reload
  and dismiss actions.
- IPC `SUBSCRIBE` support for workspace, window, mode, output, binding,
  bar-config, shutdown and tick events; `SEND_TICK` now broadcasts its tick
  payload.
- IPC `GET_INPUTS` and `GET_SEATS` now describe SwineWM's aggregate keyboard
  and pointer hooks and its single seat.
- IPC subscriptions now report window move/fullscreen changes and workspace
  rename/urgency changes.
- A companion watchdog restores hidden windows after an unexpected exit or
  after the manager's message loop stops responding; as the login shell it
  also starts Explorer after recovery.
- `smart_gaps on|off|toggle|inverse_outer`, as in sway (until now accepted
  and ignored).
- `show_marks`: marks appear on the tabs of tabbed and stacked containers
  as `[mark]`, right-aligned; marks starting with `_` stay hidden.
- `hide_edge_borders both` hides the border of a window that fills its
  workspace (a Windows border cannot hide single edges).
- `bindcode` and `unbindcode` accept numeric Windows virtual-key values
  (decimal, 1–255), so key bindings can be specified by code.
- New-output workspace discovery recognizes `move container to workspace`
  bindings as well as `workspace` bindings.

### Fixed

- Switching workspace sent the window `focus` event before the workspace
  `focus` event, and reported the newly focused window as `"visible":false`.
  The order is now sway's (workspace first), and `visible` is worked out
  from the tree as in sway.

- `swinemsg -t subscribe -m '["window","workspace"]'` failed with
  `{"success":false}` in Windows PowerShell, which drops the inner quotes
  before 7.3. swinemsg now turns `[window,workspace]` into JSON, and also
  takes plain event names: `swinemsg -t subscribe -m window workspace`.

- IPC events reached a subscribed client only when it sent another
  message: the pipe was opened for synchronous I/O, so a waiting read
  blocked every event written to it. It now uses overlapped I/O, so
  listening clients (i3ipc scripts, Waybar's sway modules) get events as
  they happen. Events still queued when swinewm exits are written first.
- IPC events now match sway: the `mode` event's `change` is the mode's
  name (with `pango_markup` from `mode --pango_markup`); `SEND_TICK` takes
  any text and broadcasts it with `"first": false`, and a new tick
  subscriber first gets `{"first": true, "payload": ""}`; subscriptions add
  up; the `empty` workspace event reports the workspace as `current`; a new
  window's `new` event comes before its `focus` event; and nothing is sent
  after `shutdown` (no `close` events for windows swinewm lets go of).
- `swinemsg -t subscribe` prints the first event, and every event with
  `-m`/`--monitor`, as `swaymsg` does.
- Newly mapped floating windows are raised above their parent without
  activating them, so owned dialogs do not remain obscured.
- Focus-follows-mouse keeps a visible owned dialog focused rather than
  activating its owner and dropping the dialog behind it.
- Gap values are range-checked and cannot overflow layout geometry.
- Display and DPI changes are coalesced before outputs, window layout, bars
  and wallpaper are refreshed, including DPI notifications received by bars.

## [0.3.0] - 2026-10-18

Mouse and decorations (see the roadmap in
[`doc/project_summary.md`](doc/project_summary.md)): focus follows mouse,
moving and resizing windows with the mouse, window borders and title bars
in the `client.*` colours, tabbed and stacked title bars, plus a
replacement for the "Open with" dialog in swinemenu.

### Added

- Mouse support, ported from sway: `focus_follows_mouse yes|no|always`,
  `floating_modifier <mod> [normal|inverse]` ($mod + left button moves a
  floating window or drags a tiled one to another place, $mod + right
  button resizes either), `tiling_drag`, `tiling_drag_threshold`,
  `mouse_warping output|container|none`, and mouse bindings
  (`bindsym --whole-window $mod+button4 workspace prev`).
- Window decorations, as in sway: `border none|normal|pixel|toggle`,
  `default_border`, `default_floating_border`, `client.focused`,
  `client.focused_inactive`, `client.unfocused`, `client.urgent`,
  `smart_borders`, `hide_edge_borders smart|smart_no_gaps` and
  `opacity`. On Windows 11 the border is the window's own (1 px) border in
  the `child_border` colour; `border normal` keeps the window's title bar
  and colours it, `pixel` and `none` remove it. Tiled windows get square
  corners. Everything is undone when swinewm exits, after a crash and by
  `swinewm --recover`. sway's defaults apply, so title bars now follow the
  `client.*` colours (dark blue for the focused window) unless the config
  sets other colours.
- Tabbed and stacked containers get title bars, as in sway: one row of
  tabs, or one row per child, in the `client.*` colours with `font`,
  `title_align`, `titlebar_padding`, `titlebar_border_thickness` and
  `client.focused_tab_title`. A click on a tab focuses it, the wheel
  switches tabs, and title bar mouse bindings (`bindsym button2 kill`) work
  on them. `title_format` sets the tab titles.
- The drop zone of a dragged tiled window uses `client.focused`'s
  indicator colour.
- `windows_theme dark|light|auto` (swinewm extension, off by default):
  switches Windows' light/dark mode, which Windows' dialogs and most
  applications follow, while swinewm runs; `auto` follows the brightness
  of the bar's background. Your own setting comes back on exit, after a
  crash and with `swinewm --recover`.
- `swinemenu --open-with <file>`, a replacement for Windows' "Open with"
  dialog (which stays empty without Explorer): it lists the programs
  registered for the file's type and opens the file with the chosen one.
  `swinemenu --install-open-with` adds "Open with swinemenu" to every
  file's context menu and uses swinemenu for files no program is set for;
  `--uninstall-open-with` removes both.

### Fixed

- A floating window dragged by its title bar onto another monitor now
  joins the workspace shown there, as in sway.

## [0.2.0] - 2026-10-17

Sway configs that real people have: criteria and window rules, marks,
swap, sticky, resize, outputs, `include` and config error notifications,
plus the `swinemenu` launcher (see the roadmap in
[`doc/project_summary.md`](doc/project_summary.md)). The multi-monitor
features (`output … mode|position|transform`, `workspace … output`,
`move workspace to output`, hot-plug) are covered by unit tests but not
yet tested on a real multi-monitor setup; reports are welcome.

### Added

- `swinemenu`, a small launcher in the spirit of wofi/wmenu and the new
  default `$menu` (Win+d): lists the Start menu's apps and Store apps
  (`--show drun`), programs on PATH (`--show run`) or lines from standard
  input (`--dmenu`), remembers what you pick, and takes the bar's colours
  and font. swinewm starts the choice, so Store apps work as the shell.
  Flow Launcher or any other launcher can still be set as `$menu`.
- IPC `get_bar_config`, as in sway: the bar's position, height, font and
  colours, computed from the Waybar `style.css`.
- Store apps start as the shell: `exec shell:AppsFolder\<AppUserModelID>`
  uses the documented `IApplicationActivationManager` instead of the shell
  namespace, which fails with "Class not registered" without Explorer.
  `Get-StartApps` in PowerShell lists the IDs, e.g.
  `bindsym $mod+c exec shell:AppsFolder\AnthropicPBC.Claude_fnn82j28hfe8t!Claude`.
- `swinewm --set-autostart` / `--unset-autostart` (experimental): start
  swinewm at logon next to Explorer, which stays the shell behind it with
  its taskbar hidden (`explorer_taskbar autohide`, now in the default
  config; no effect as the shell). In first tests Explorer still took many
  key combinations; `--set-shell` remains the supported mode.
- Criteria: `[app_id=… class=… title=… …] <commands>`, `for_window`,
  `assign` and `no_focus`, as in sway, with a built-in regular-expression
  engine. On Windows `app_id` is the executable's file name (matched
  ignoring case) and `class` the window class; see the token table in
  `doc/compatibility.md`.
- `mark`, `unmark`, `swap container with …`, `sticky`,
  `rename workspace … to …`, `move … to mark`, `focus prev|next [sibling]`
  and the IPC `get_marks` request, as in sway.
- `resize` is now a port of sway's: `grow`/`shrink` towards an edge
  (`up`/`down`/`left`/`right`), `resize set`, and `<n> px or <m> ppt`.
- Outputs: `workspace <name> output …` and `workspace <name> gaps …`,
  `move workspace to output …`; workspaces return to their monitor when
  it is plugged back in, as in sway. Monitors can be named by their model
  (`output "DELL U2720Q" …`).
- `output … mode|position|transform` change Windows' display settings
  for as long as swinewm runs (restored on exit, after a crash and with
  `--recover`).
- `include` takes wildcards (`include config.d/*`), as in sway.
- Config errors are shown as a notification (swinewm's swaynag), on start
  and on reload.
- `doc/linux-users.md`: Windows for Linux users (where things are,
  shells, package managers, getting unstuck when swinewm is the shell).

### Fixed

- `exec "C:\path with spaces\App.lnk"` failed: sway's rule removes the
  quotes of a single quoted argument, and the path was then split at its
  first space. A command line that names an existing file is now opened as
  a whole (this also made swinemenu's Start menu entries do nothing).
- Store apps did not start from swinemenu: the activation manager is now
  created with `CLSCTX_ALL` (as PowerShell does) instead of only as a local
  server, a failed Store app `exec` reports failure (with the AppUserModelID
  in the log), and swinemenu then tries to start the app itself.
- Floating dialogs that were only partly on-screen could keep their bad
  position; new dialogs are now centered unless the whole window fits on a
  monitor.
- As the shell, swinewm now answers programs that ask where the taskbar
  is (`SHAppBarMessage(ABM_GETTASKBARPOS)`, which went unanswered without
  Explorer).
- Windows that are mostly off every monitor (less than half visible) are
  moved back on screen: a floating window that moves itself away is
  centred again, and other windows (dialogs swinewm does not tile, such as
  Windows 11's "Open with", which centres itself on the screen's corner
  without Explorer as the shell) are centred on the nearest monitor.
  Windows parked far away on purpose (at -32000) and windows being dragged
  are left alone; each such move is logged with the window's class and
  program. The "Open with" dialog still shows no app list as the shell
  (see `doc/compatibility.md`).
- `tools/list-windows.bat` and `tools/move-foreground.bat`: scripts for
  reporting windows that swinewm handles badly; `tools/launch-store-app.bat`
  tests ways of starting Store apps.
- A two-part version tag (`0.1`) was not recognised, so its build was
  versioned after the previous tag; it now means `0.1.0`.
- The release job failed when the release had already been created on
  GitHub; it now attaches the MSI and zip to it.

## [0.1.0]

Towards 0.1.0, the first preview (see the roadmap in
[`doc/project_summary.md`](doc/project_summary.md)).

### Added

- Waybar's `tray` module: swinewm hosts the Windows notification area.
  Programs' tray icons appear in the bar, clicks reach them as from
  Explorer, balloon tips become notifications, and as the shell Windows'
  volume, network and power icons are loaded. Options `icon-size`,
  `spacing`, `show-passive-items`, `reverse-direction`, `orders`,
  `ignore-list`, `icons`.
- Built-in `grim` (there is no grim on Windows): `grim`, `-o`, `-g`, `-t`,
  saved to the Pictures folder; Print is bound to it. `exec` runs
  `a && b` chains of built-in commands.
- i3's `restart` command, for updating a running swinewm: rename the
  running `swinewm.exe`, copy the new one, `swinemsg restart`.
- Versions come from the git tag: `swinewm -v`, `swinemsg -v`,
  `get_version`, the .exe version resource and the MSI file name show
  e.g. `0.1.0` or `0.1.0-5-g1234abc` for builds in between.
- Pushing a version tag publishes a GitHub Release with the MSI and zip.
- `SWINEWM_LOG=error|info|debug` sets the log level when no `-V`/`-d` can
  be passed (Windows starts the shell without options).

### Changed

- As the shell, `swinewm.log` logs at info level by default, without
  terminal colour codes, and the previous session's log is kept as
  `swinewm.old.log`.
- As the shell, the wallpaper is painted by swinewm's desktop window.
- `--shell` is refused while Explorer runs; use
  `explorer_taskbar autohide` next to Explorer instead.

### Fixed

- `--shell` next to Explorer made swinewm hang (a work-area fight with
  Explorer's taskbar).
- Black background instead of the wallpaper when swinewm is the shell.
- Dialogs that place themselves off-screen (Explorer's "Open with") are
  centered.
- Windows that were maximized when swinewm took them over were placed
  slightly too high, under the bar.
- Window titles with non-ASCII characters were garbled in the console.

## [0.0.1]

The initial draft: sway's tree, commands, config and IPC as a Windows
window manager (hide-based workspaces, keyboard-hook bindings), the
built-in Waybar (config.jsonc, CSS subset, common modules) and Mako
(config, criteria, modes, `notify-send`, `makoctl`), wallpapers,
running as the Windows shell (`--set-shell`), and an MSI installer.

[Unreleased]: https://github.com/staalmannen/SwineWM/compare/0.4.0...HEAD
[0.4.0]: https://github.com/staalmannen/SwineWM/releases/tag/0.4.0
[0.3.0]: https://github.com/staalmannen/SwineWM/releases/tag/0.3.0
[0.2.0]: https://github.com/staalmannen/SwineWM/releases/tag/0.2.0
[0.1.0]: https://github.com/staalmannen/SwineWM/releases/tag/0.1.0
[0.0.1]: https://github.com/staalmannen/SwineWM/releases/tag/0.0.1
