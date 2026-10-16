# Changelog

All notable changes to SwineWM. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/) (0.x: anything may still
change). A release is made by pushing a tag (`0.1.0`); CI then publishes
the MSI and zip with the matching section below as release notes.

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

[0.1.0]: https://github.com/staalmannen/SwineWM/compare/0.1.0
[0.0.1]: https://github.com/staalmannen/SwineWM/releases/tag/0.0.1
