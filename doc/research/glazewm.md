# Notes: what GlazeWM does differently

[GlazeWM](https://github.com/glzr-io/glazewm) is a tiling window manager
for Windows (and macOS) inspired by i3, written in Rust, with its own YAML
config and a separate bar, [Zebar](https://github.com/glzr-io/zebar). It
was suggested on r/swaywm as a project with similar aims. These notes come
from reading its source (commit `5709ad0`, April 2026); paths are relative
to its `packages/` directory.

**Licence: GPL-3.0.** SwineWM is MIT, so no GlazeWM code may be copied or
ported. Only the ideas below are used, reimplemented from the documented
Windows APIs they rely on. (This differs from Cairo/ManagedShell, which are
Apache-2.0; see [`windows-shell.md`](windows-shell.md).)

GlazeWM is not a specification for SwineWM the way sway is: its commands,
config and IPC are its own. It is useful as a second opinion on the Windows
side of the problem.

Status column: ✅ SwineWM does this, 🟡 partly, 🔜 planned (milestone in
[`../project_summary.md`](../project_summary.md)), ❌ not planned, — no
change needed.

## Architecture at a glance

| Topic | GlazeWM | SwineWM |
| --- | --- | --- |
| Processes | `glazewm.exe` (WM), `zebar` (bar, web technologies), `glazewm-watcher` (crash cleanup), `glazewm` CLI | One `swinewm.exe` with built-in bar, notifications and tray, plus `swinemsg` |
| Platform layer | `wm-platform` crate with Windows and macOS back ends | `include/swinewm/platform.h`, `win32/`, `tests/fake_platform.c` |
| Window events | `SetWinEventHook` out of context for destroy/show/hide, minimize, move-size start/end, foreground, location/name change, cloak/uncloak (`wm-platform/src/platform_impl/windows/window_listener.rs`) | ✅ the same set (`win32/window.c`) |
| Keyboard | Low-level keyboard hook (`keyboard_hook.rs`) | ✅ the same |
| Mouse | Raw input (`RegisterRawInputDevices`) for focus follows mouse and drags (`mouse_listener.rs`) | 🔜 0.3.0; raw input is cheaper than a low-level mouse hook |
| IPC | WebSocket server on a TCP port (`wm/src/ipc_server.rs`) | ✅ sway's i3-ipc protocol on a named pipe |
| Single instance | Named mutex (`single_instance.rs`) | ✅ the IPC pipe (`FILE_FLAG_FIRST_PIPE_INSTANCE`) |

## Hiding windows on other workspaces

GlazeWM's `general.hide_method` (`wm-common/src/parsed_config.rs`,
`wm/src/commands/general/platform_sync.rs`) has three values:

| Method | How | GlazeWM's verdict |
| --- | --- | --- |
| `hide` | `ShowWindowAsync(SW_HIDE)` | "Legacy option … has stability issues with some apps" |
| `cloak` (default) | Undocumented: the immersive shell's `IApplicationViewCollection::GetViewForHwnd`, then `IApplicationView::SetCloak(1, 2)` to cloak and `SetCloak(1, 0)` to uncloak (`com.rs`, `native_window.rs` `set_cloaked`) | Recommended. The app keeps believing it is visible; DWM simply does not draw it |
| `place_in_corner` | Moves windows to a monitor corner, mostly off screen | Fallback (only option on macOS) |

With `cloak`, cloaked windows still have taskbar buttons, so GlazeWM removes
and re-adds them with `ITaskbarList::DeleteTab`/`AddTab` unless
`show_all_in_taskbar` is set.

**SwineWM:** uses `SW_HIDE` today. That is why `win32/window.c` counts the
hides and shows it caused itself (`expected_hides`/`expected_shows`), to
tell them apart from an app hiding itself, for example minimizing to the
tray. Cloaking would remove that ambiguity and probably some app
misbehaviour. It relies on undocumented interfaces, so it would have to be
isolated and optional, with `SW_HIDE` kept as the fallback (see the rule in
`project_summary.md` and `design_strategy.md`). 🔜 optional, not scheduled

## Crash cleanup in a second process

`wm-watcher/src/main.rs` is a small program started next to the WM. It
connects to the IPC server, queries the managed windows and subscribes to
`window_managed`, `window_unmanaged` and `application_exiting`. If the
connection drops without `application_exiting`, the WM died, and the watcher
shows every window again, restores its taskbar button, border colour and
opacity.

**SwineWM:** restores windows from an unhandled-exception handler, from
`platform_finish`, and from the state file read by `swinewm --recover` and
the next start (`win32/recover.c`). A watcher also covers what an
in-process handler cannot: being killed from Task Manager, a stack
overflow, or a hang. A `swinewm --watch` mode could reuse the recovery code;
it needs IPC events first. 🔜 0.4.0, with `SUBSCRIBE`

## Window decorations through DWM

All in `wm-platform/src/platform_impl/windows/native_window.rs`; documented
APIs, Windows 11 for the DWM attributes:

| Feature | API | Maps to (sway) | SwineWM |
| --- | --- | --- | --- |
| Border colour | `DwmSetWindowAttribute(DWMWA_BORDER_COLOR)`; `DWMWA_COLOR_NONE` to remove, default colour to reset | `client.focused` / `client.unfocused` … border colour | ✅ `win32/decor.c`, the `child_border` colour. The border stays 1 px, so `default_border pixel <n>` cannot be honoured exactly |
| Corner style | `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE)`: default, square, rounded, small rounded | none in sway; square corners for tiled windows look like sway | ✅ tiled windows |
| Title bar | Clear or set `WS_DLGFRAME`, then `SetWindowPos(SWP_FRAMECHANGED …)` | `border none`, `border pixel`, `default_border` | ✅ on a worker thread, since changing another program's styles waits for it; `border normal` also colours the title bar (`DWMWA_CAPTION_COLOR`, `DWMWA_TEXT_COLOR`) |
| Transparency | Add `WS_EX_LAYERED`, `SetLayeredWindowAttributes(LWA_ALPHA)` | `opacity <value>` | ✅ |
| Fullscreen hint | `ITaskbarList2::MarkFullscreenWindow` | `fullscreen` | — swinewm draws its own bar; useful only for Explorer's taskbar next to Explorer |

Anything changed here must be undone on exit, crash and `--recover`
(GlazeWM's watcher resets border colour and opacity for that reason).
swinewm records what it changed per window in the recovery file
(`hidden-windows.txt`, lines `<handle> <pid> d <flags>`).

## Which windows are managed

`wm/src/commands/window/manage_window.rs` (`check_is_manageable`):

- visible and not cloaked, with a process name and title;
- not `WS_CHILD`, not `WS_EX_TOOLWINDOW` or `WS_EX_NOACTIVATE`;
- **captionless windows are managed unless they also have an owner** (that
  combination is usually a menu or autocomplete popup);
- the window starts as floating if it cannot be resized, as fullscreen if
  it covers the whole workspace, else tiling.

Exceptions are handled with default `window_rules` that `ignore` windows
(`resources/assets/sample-config.yaml`): Zebar, browser
picture-in-picture, PowerToys popups, Office popups (`EXCEL` without class
`XLMAIN`, and so on), Lively wallpapers.

**SwineWM:** `is_manageable` in `win32/window.c` skips *every* captionless
window without `WS_EX_APPWINDOW`, which is stricter. Windows 11's "Open
with" dialog is probably such a window, which would explain why it is left
alone and places itself off screen. The off-screen rescue logs the class
and style of such windows; with that, either loosen the rule as GlazeWM does
(and add ignore criteria for the popups that then appear) or keep the
rescue. Open question.

New floating windows are centred on the target workspace when
`window_behavior.state_defaults.floating.centered` is set, and their size is
clamped to the workspace. SwineWM keeps a dialog's own position if it fits
on one monitor and centres it otherwise (`tree/view.c`).

## Smaller lessons

| Topic | GlazeWM | SwineWM |
| --- | --- | --- |
| Focus | Sends one synthetic mouse input (tagged with `dwExtraInfo` so its own hook ignores it), then `SetForegroundWindow` (`native_window.rs` `focus`) | ✅ the same trick (`platform_view_activate`) |
| Mixed DPI | When a window moves to a monitor with a different DPI, `SetWindowPos` is called twice: the first call often leaves the wrong size (`platform_sync.rs`, `has_pending_dpi_adjustment`) | 🔜 check on a mixed-DPI setup; cheap to add |
| Frame changes | `SWP_FRAMECHANGED` on every layout move | — only needed after changing styles |
| Display changes | Re-reads monitors on `WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE` with `SPI_SETWORKAREA`, `WM_DEVICECHANGE` with `DBT_DEVNODES_CHANGED`, and after resume (`WM_POWERBROADCAST`) (`display_listener.rs`) | 🟡 `WM_DISPLAYCHANGE` and `WM_SETTINGCHANGE`; add device and resume events when hot-plug or sleep shows gaps |
| Focus reset | When nothing should be focused, focus the shell window (`GetShellWindow`), or the desktop window without Explorer | ✅ swinewm's own desktop window is the shell window |
| Cursor jump | Optionally moves the cursor to the focused monitor or window | 🔜 0.3.0 as sway's `mouse_warping` |
| Flow Launcher | Special-cased in the manage check | — swinewm's default launcher (`config.in`); handled without a special case |

## Not relevant

- Zebar, GlazeWM's bar, is a web-technology widget host; swinewm's bar is a
  Waybar clone drawn with GDI.
- The YAML config, command syntax and WebSocket IPC: swinewm follows sway's
  config and i3-ipc instead.
- The Win key: GlazeWM's default bindings use Alt, so it offers no hint for
  Win-key combinations that Windows reserves.
