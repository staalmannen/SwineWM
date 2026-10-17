# Changelog

All notable changes to SwineWM. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/) (0.x: anything may still
change). A release is made by pushing a tag (`0.1.0`); CI then publishes
the MSI and zip with the matching section below as release notes.

## [Unreleased]

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

[Unreleased]: https://github.com/staalmannen/SwineWM/compare/0.1.0...HEAD
[0.1.0]: https://github.com/staalmannen/SwineWM/releases/tag/0.1.0
[0.0.1]: https://github.com/staalmannen/SwineWM/releases/tag/0.0.1
