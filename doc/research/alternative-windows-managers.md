# Notes: alternative Windows window managers and shells

Research notes from 2026-10-19 on four projects that provide useful
comparisons for SwineWM's Windows integration. Sway, Waybar and Mako remain
the behavioral and configuration specifications; these projects are
secondary references for Windows-specific design choices and user
workflows.

## Summary

| Project | Role | Useful perspective |
| --- | --- | --- |
| [workspacer](https://github.com/workspacer/workspacer) | Scriptable tiling WM for Windows 10+ | Separate layout engines, composable extensions, and configurable routing/filters |
| [dwm-win32](https://github.com/martanne/dwm-win32) | Historical Win32 port of dwm | Practical limits of shell-hook window discovery and a compact master/stack/tag model |
| [bbLean](https://bb4win.sourceforge.net/bblean/) | Blackbox-style Windows shell | Plugin ecosystem, shell command/message integration, and workspace-aware shell UI |
| [bug.n](https://github.com/fuhsjr00/bug.n) | Tiling WM add-on for Explorer | Rule-driven window management, multi-tag views, state persistence, and delayed event handling |

## workspacer

workspacer's configuration is executable C# and is explicitly aimed at
power users. Its core distinguishes workspace/window management from
layout calculation: an `ILayoutEngine` receives a window collection and
available dimensions, then returns desired locations. Workspaces can have
different layout-engine instances, and layout proxies can wrap all engines
to add behavior such as gaps. The configuration context centralizes
keybindings, plugins, workspaces, monitors, system tray, window routing and
layout customization.

The window router has separate filters and routes. Filters decide whether
a window is managed; routes can assign an eligible window to a workspace,
and can defer to later routes. This gives users a direct way to avoid
problematic fullscreen applications or direct application windows to
specific workspaces. The documented built-ins also include alternative
layout engines, title-bar customization, a focus indicator, an action menu,
and a widget-based bar. Workspace operations preserve ordering and
last-focused-window state, and expose operations such as swapping the
focused window with the primary window.

**Possible lessons for SwineWM:** this is a useful model for keeping layout
algorithms independent from platform window operations, even if SwineWM
continues to follow sway's tree and command semantics rather than adopting
workspacer's configuration API. Its explicit route/filter distinction may
help when expanding criteria and assignment behavior. Treat the C# plugin
model as an architectural comparison, not a reason to introduce a plugin
system before the sway-compatible core is complete.

## dwm-win32

dwm-win32 is a small, historical port of X11 dwm. It has master/stack,
monocle and floating layouts, per-window tags, and a compact status bar.
Its configuration is compiled into the program through `config.h`, rather
than loaded from a runtime configuration file. Dialogs are always floating.
The README says the project is no longer actively developed.

The most useful Windows-specific note is its account of window discovery:
it uses `RegisterShellHookWindow` for creation and destruction notifications,
but those notifications only cover unowned top-level windows. The project
then scans child windows associated with the active top-level window, and
acknowledges that this can miss some cases. It suggests a `WH_CBT` hook as a
more comprehensive alternative, but notes that this requires a DLL loaded
into other processes.

**Possible lessons for SwineWM:** regard shell-hook notifications as a
partial signal, not a complete inventory of all HWNDs; combine notifications
with explicit filtering and reconciliation where needed. The stated CBT
trade-off reinforces avoiding cross-process hook DLLs for ordinary
window-management coverage. The tag model is a useful contrast, but sway's
workspace/tree model remains SwineWM's target.

## bbLean

bbLean is a compact Blackbox-derived shell with a desktop, toolbar, menus,
keyboard bindings and optional plugins. Its documentation emphasizes
composable shell extensions: plugins can be loaded or unloaded from the
configuration UI, and plugins can dock into a shared slit. The bbLeanBar
offers task interactions beyond simple activation, including moving tasks
between workspaces with gestures and showing only current-workspace tasks.
The shell's menu is data-driven, can include submenus and folder contents,
and can invoke shell commands or internal actions.

The documentation also surfaces several integration patterns: broadcast
messages ("broams") let menus, hotkeys and external clients invoke the same
shell actions; desktop margins can be automatically derived from bars or
set manually per monitor; sticky-window rules make selected applications
visible on every workspace; and the shell can run either on top of Explorer
or as the default shell. It warns that the tray may not work reliably when
running on top of Explorer.

**Possible lessons for SwineWM:** keep shell-specific facilities explicit
and test both "next to Explorer" and replacement-shell modes. Shared
commands callable from bindings, menus and external clients are a useful
consistency principle, already aligned with SwineWM's command and IPC
design. Per-monitor work-area behavior and workspace-persistent focus are
worth considering when extending the bar and shell integration.

## bug.n

bug.n is an AutoHotkey tiling add-on for Explorer, not a shell replacement.
It provides tile, monocle and floating layouts, multiple monitors and
tagging: a window can have multiple tags and therefore appear in multiple
views. The configuration is an INI file with hotkey-to-function mappings
and ordered rules based on window class, title and style. Its defaults
illustrate why rule ordering and exceptions matter: generic rules skip
child and popup windows, followed by exceptions for applications whose
main windows use those styles.

Several details are relevant to reliability and user expectations:

- When a hidden window is activated, a configurable policy can switch to
  its view, retag it into the current view, or hide it again.
- A shell-message delay is configurable to let window state settle before
  processing notifications.
- Session auto-save can be off, automatic or ask-first; periodic saves
  preserve monitor/view/layout and window state for recovery after an
  unintentional exit.
- View activation can be synchronized across monitors or remain independent
  per active monitor.
- Debug hotkeys can dump managed-window or current-view details, including
  class, process, style, geometry, tags and floating state.

**Possible lessons for SwineWM:** the hidden-window activation policy is a
useful edge case to compare with sway's focus/workspace semantics. A clear
per-window diagnostic dump would help investigate Windows filtering issues.
The persistence and monitor-sync options are useful product comparisons,
but should not be adopted without deciding whether they fit SwineWM's
compatibility goals and existing recovery model.

## Licensing and reuse

The projects are not all equally suitable as code references. workspacer
and dwm-win32 declare MIT licenses. bbLean documents GPL-2.0; bug.n
documents GPL-3.0. This note records design observations only. Do not copy
or port bbLean or bug.n code into SwineWM; independently implement any
useful behavior against SwineWM's specifications and documented Windows
APIs. Continue to preserve upstream notices for any permitted substantial
port from MIT-licensed references, per the repository's project guidance.

## References

- workspacer: [README](https://github.com/workspacer/workspacer),
  [configuration and layout engines](https://workspacer.org/config/),
  [keybindings](https://workspacer.org/keybindings/),
  [`ILayoutEngine`](https://github.com/workspacer/workspacer/blob/master/src/workspacer.Shared/Layout/ILayoutEngine.cs),
  [`IConfigContext`](https://github.com/workspacer/workspacer/blob/master/src/workspacer.Shared/IConfigContext.cs),
  [`Workspace`](https://github.com/workspacer/workspacer/blob/master/src/workspacer.Shared/Workspace/Workspace.cs),
  [MIT license](https://github.com/workspacer/workspacer/blob/master/LICENSE).
- dwm-win32: [README and Windows discovery notes](https://github.com/martanne/dwm-win32),
  [MIT/X11 license](https://raw.githubusercontent.com/martanne/dwm-win32/master/LICENSE.txt).
- bbLean: [project overview](https://bb4win.sourceforge.net/bblean/),
  [user interface and plugins](https://bb4win.sourceforge.net/bblean/docs/bblean_overview.htm),
  [desktop margins, shell mode and broadcast messages](https://bb4win.sourceforge.net/bblean/docs/bblean_details.htm),
  [license and release history](https://bb4win.sourceforge.net/bblean/docs/bblean_info.htm).
- bug.n: [README](https://github.com/fuhsjr00/bug.n),
  [default configuration](https://github.com/fuhsjr00/bug.n/blob/master/doc/Default_configuration.md),
  [usage](https://github.com/fuhsjr00/bug.n/blob/master/doc/Usage.md),
  [default hotkeys](https://github.com/fuhsjr00/bug.n/blob/master/doc/Default_hotkeys.md),
  [GPL-3.0 license](https://github.com/fuhsjr00/bug.n/blob/master/LICENSE.md).
