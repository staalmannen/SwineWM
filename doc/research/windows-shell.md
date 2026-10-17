# Notes: how alternative Windows shells work

Findings from reading [Cairo Desktop](https://github.com/cairoshell/cairoshell)
and its shell library [ManagedShell](https://github.com/cairoshell/ManagedShell)
(both Apache-2.0, C#/WPF), a mature Explorer replacement that supports
Windows 7 to 11. Cairo itself is mostly UI; nearly all the Windows plumbing
is in ManagedShell, so file references below are to ManagedShell unless
noted. SwineWM reimplements these ideas in C; the source files say where
an idea came from.

Status column: ✅ implemented in SwineWM, 🔜 planned, ❌ not planned.

For window management itself (hiding windows, crash cleanup, DWM borders,
which windows to manage), see also the notes on GlazeWM in
[`glazewm.md`](glazewm.md).

## Being the shell

| Topic | How Cairo/ManagedShell does it | SwineWM |
| --- | --- | --- |
| Am I the shell? | `GetShellWindow() == NULL` at start-up, before creating any window; `/shell` and `/noshell` override (Cairo `CairoApplicationInitializationService`) | ✅ `win32/shell.c`, `--shell` (refused while another shell runs), `--no-shell` |
| Shell window | Full virtual-screen tool window registered with the undocumented-but-exported `SetShellWindow` (`ShellWindow.cs`) | ✅ `swinewm-desktop`, looked up with `GetProcAddress` |
| Logon "shell ready" | Set the event `ShellDesktopSwitchEvent` (`ShellHelper.SetShellReadyEvent`) so Windows finishes the logon | ✅ |
| Startup programs | Only as the shell and not on a restart: HKLM/HKCU `Run` (+ `Wow6432Node`), `Policies\Explorer\Run`, HKCU `RunOnce` (deleted after use; HKLM `RunOnce` skipped because it cannot be deleted), both Startup folders; entries disabled in Task Manager are recognised by an odd first byte in `Explorer\StartupApproved\*` (`StartupRunner.cs`) | ✅ Once per logon session via a volatile registry key instead of a restart flag |
| Work area | As the shell, `SystemParametersInfo(SPI_SETWORKAREA)` = monitor minus own bars; reset on exit (`AppBarManager.SetWorkArea/ResetWorkArea`) | ✅ per monitor; announced with a posted `WM_SETTINGCHANGE` so a slow or competing window cannot block or loop |
| Exit code | Exit code 1 when quitting on purpose, so Winlogon's `AutoRestartShell` does not start Explorer (Cairo `CairoApplication.OnExit`) | ✅ differently: `exit` logs off as in sway; after a crash Explorer is started as a rescue |
| Single instance | Named mutex | ✅ already: the IPC pipe is created with `FILE_FLAG_FIRST_PIPE_INSTANCE` |
| Set as login shell | Installer writes the Winlogon `Shell` value | ✅ `swinewm --set-shell` / `--unset-shell` (HKCU, per user) |
| `TaskbarButtonCreated` | As the shell, send the registered message to each new window so `ITaskbarList3` users (progress bars, thumbnail buttons) initialise (`TasksService.addWindow`) | 🔜 needed with a taskbar-like module |
| `SetTaskmanWindow` | Claims the task-manager window role (`TasksService`) | ❌ not needed |
| Shell service objects | As the shell, loads the SSOs (e.g. `stobject.dll`) that provide the volume/network/battery tray icons (`ShellServiceObject.cs`) | ✅ loaded as the shell to provide the Windows tray icons |

## Next to Explorer

| Topic | How Cairo/ManagedShell does it | SwineWM |
| --- | --- | --- |
| Reserving space | Register each bar as an AppBar: `SHAppBarMessage(ABM_NEW / ABM_QUERYPOS / ABM_SETPOS)`, react to `ABN_POSCHANGED`, call `ABM_ACTIVATE` on `WM_ACTIVATE` and `ABM_WINDOWPOSCHANGED` (`AppBarManager.cs`, `AppBarWindow.cs`) | 🔜 SwineWM reserves bar space inside its own layout and answers selected taskbar queries as the shell; it does not register bars as AppBars, so unmanaged maximized windows can overlap them |
| Hiding Explorer's taskbar | Save the taskbar state, `ABM_SETSTATE(ABS_AUTOHIDE)`, hide `Shell_TrayWnd` and every `Shell_SecondaryTrayWnd` with `SetWindowPos(SWP_HIDEWINDOW)`, re-hide every 100 ms because Explorer shows it again; restore on exit (`ExplorerHelper.cs`) | ✅ `explorer_taskbar autohide` (re-hidden every second; original state saved for `--recover`) |
| Desktop windows | Explorer's desktop is `Progman`/`WorkerW` at the bottom of the Z order | ✅ the wallpaper window stays just above them |

## Windows and tasks

| Topic | How Cairo/ManagedShell does it | SwineWM |
| --- | --- | --- |
| Which windows count | Visible, no owner unless `WS_EX_APPWINDOW`, not `WS_EX_NOACTIVATE` unless `WS_EX_APPWINDOW`, not `WS_EX_TOOLWINDOW`, no `ITaskList_Deleted` window property (set by `ITaskbarList::DeleteTab`), not cloaked, and not an "immersive shell" window: `ApplicationFrameWindow`, `Windows.UI.Core.CoreWindow`, `StartMenuSizingFrame`, `Shell_LightDismissOverlay` without `WS_EX_WINDOWEDGE` (`ApplicationWindow.CanAddToTaskbar/getShowInTaskbar`) | ✅ added the property and immersive-window rules to `is_manageable` |
| Window events | `RegisterShellHookWindow` + the `SHELLHOOK` message: `HSHELL_WINDOWCREATED/DESTROYED/ACTIVATED`, `RUDEAPPACTIVATED` (0x8004), `FLASH` (0x8006), `REDRAW`, `WINDOWREPLACING/REPLACED`, `MONITORCHANGED` (16), `FULLSCREENENTER/EXIT` (53/54); plus a WinEvent hook for cloak/uncloak (`TasksService.cs`) | ✅ shell hook for `FLASH` (→ sway urgency) and fullscreen; WinEvents for the rest |
| Fullscreen apps | A window is fullscreen when its "effective" rectangle (client area for framed windows, else window rect) equals a monitor's; excluded: own process, invisible/minimized, cloaked, immersive shell windows, the `NonRudeHWND` property, layered windows that are transparent or no-activate. Bars hide while such a window is in front on their monitor (`FullScreenHelper.cs`) | ✅ managed windows → sway fullscreen; unmanaged ones (games) hide the bar on that output |

## The tray (notification area) — implemented

How it works (`TrayService.cs`, `NotificationArea.cs`, `NotifyIcon.cs`,
`ShellServiceObject.cs`); SwineWM's version is `win32/tray.c`, described
in [`../architecture.md`](../architecture.md):

1. Programs call `Shell_NotifyIcon`, which sends `WM_COPYDATA` (`dwData` 1,
   a `SHELLTRAYDATA`: magic `0x34753423`, the `NIM_*` message and a
   `NOTIFYICONDATA` with 32-bit handles) to the window found by
   `FindWindow("Shell_TrayWnd")`. ✅
2. The shell creates its own `Shell_TrayWnd` (with a `TrayNotifyWnd` child)
   and broadcasts the registered `TaskbarCreated` message so running
   programs re-add their icons. With Explorer running, ManagedShell raises
   its `Shell_TrayWnd` to `HWND_TOPMOST` on a 100 ms timer so `FindWindow`
   finds it first. ✅ (SwineWM uses a separate thread for the window.)
   ManagedShell can also read the icons Explorer already has from its
   toolbar (`ExplorerTrayService`, `ReadProcessMemory`). ❌ not needed:
   `TaskbarCreated` brings almost all of them back.
3. `NIM_ADD/MODIFY/DELETE/SETFOCUS/SETVERSION` maintain the icon list
   (keyed by `hWnd`+`uID` or `guidItem`); `hIcon` is a shared handle,
   copied with `CopyIcon`; `NIS_HIDDEN` hides an icon. Balloon
   notifications (`NIF_INFO`) go to the built-in Mako. ✅
4. Clicks are forwarded to the owner with `SendNotifyMessage(uCallbackMessage)`
   (down/up/double click, then `NIN_SELECT` or `WM_CONTEXTMENU`; version 4
   puts the position in `wParam` and the id in `HIWORD(lParam)`) after
   `AllowSetForegroundWindow`. Hover (`NIN_POPUPOPEN`, `WM_MOUSEMOVE`) is
   not sent yet. ✅ / 🟡
5. `dwData` 3 (`Shell_NotifyIconGetRect`) is answered with the icon's
   rectangle. ✅
6. Messages not handled (including Explorer's private messages and anything
   from `WM_USER` up) are forwarded to Explorer's taskbar. ✅ As the shell,
   SwineWM answers `ABM_GETTASKBARPOS`, `ABM_GETSTATE`,
   `ABM_GETAUTOHIDEBAR`, `ABM_GETAUTOHIDEBAREX`, `ABM_ACTIVATE` and
   `ABM_WINDOWPOSCHANGED`. It does not register AppBars or implement
   `ABM_NEW`/`ABM_QUERYPOS`/`ABM_SETPOS`; unmanaged maximized windows can
   therefore overlap the bar while running next to Explorer. 🔜
7. As the shell, the volume/network/power icons come from the SysTray shell
   service object (`CLSID 35CEC8A3-2BE6-11D2-8773-92E220524153`,
   `IOleCommandTarget::Exec(CGID_ShellServiceObject, OLECMDID_NEW)`). ✅
   ManagedShell hides the volume icon there on Windows 10+, where clicking
   it needs Explorer's flyout, and treats some Windows 11 action-centre
   icons specially (`Win11ActionCenterIcons`); SwineWM does neither yet.

Wine's `Shell_NotifyIcon` sends a different, Wine-specific message, so the
tray cannot be tested with ordinary programs under Wine; a test program
that sends the Windows format works.

## Without Explorer: what is known to be missing

- `shell:AppsFolder\<AUMID>` (how Flow Launcher and others open Store/MSIX
  apps) fails with "Class not registered"; the apps' App Execution Aliases
  (`firefox.exe` in `%LOCALAPPDATA%\Microsoft\WindowsApps`) still work.
- ManagedShell keeps the volume tray icon hidden as the shell on Windows
  10+ because its flyout needs Explorer; the same family of features
  (`CImmersiveShell`, `IShellExperienceManagerFactory`) is hosted by
  Explorer.

## Other ideas noted

- Windows 11: WPF tool windows stop receiving `WM_DPICHANGED`; ManagedShell
  hides bars from the taskbar another way there (`AppBarWindow`). swinewm's
  bar windows are plain Win32 tool windows; watch for DPI-change issues.
- Volume, network and power flyouts can be opened through the undocumented
  `IShellExperienceManagerFactory` interfaces (`UWPInterop`); useful for bar
  modules, but undocumented.
- UWP/Store apps can be listed and launched through
  `IApplicationActivationManager` (`StoreAppHelper`).
