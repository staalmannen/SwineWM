# Notes: how alternative Windows shells work

Findings from reading [Cairo Desktop](https://github.com/cairoshell/cairoshell)
and its shell library [ManagedShell](https://github.com/cairoshell/ManagedShell)
(both Apache-2.0, C#/WPF), a mature Explorer replacement that supports
Windows 7 to 11. Cairo itself is mostly UI; nearly all the Windows plumbing
is in ManagedShell, so file references below are to ManagedShell unless
noted. SwineWM reimplements these ideas in C; the source files say where
an idea came from.

Status column: ✅ implemented in SwineWM, 🔜 planned, ❌ not planned.

## Being the shell

| Topic | How Cairo/ManagedShell does it | SwineWM |
| --- | --- | --- |
| Am I the shell? | `GetShellWindow() == NULL` at start-up, before creating any window; `/shell` and `/noshell` override (Cairo `CairoApplicationInitializationService`) | ✅ `win32/shell.c`, `--shell`, `--no-shell` |
| Shell window | Full virtual-screen tool window registered with the undocumented-but-exported `SetShellWindow` (`ShellWindow.cs`) | ✅ `swinewm-desktop`, looked up with `GetProcAddress` |
| Logon "shell ready" | Set the event `ShellDesktopSwitchEvent` (`ShellHelper.SetShellReadyEvent`) so Windows finishes the logon | ✅ |
| Startup programs | Only as the shell and not on a restart: HKLM/HKCU `Run` (+ `Wow6432Node`), `Policies\Explorer\Run`, HKCU `RunOnce` (deleted after use; HKLM `RunOnce` skipped because it cannot be deleted), both Startup folders; entries disabled in Task Manager are recognised by an odd first byte in `Explorer\StartupApproved\*` (`StartupRunner.cs`) | ✅ Once per logon session via a volatile registry key instead of a restart flag |
| Work area | As the shell, `SystemParametersInfo(SPI_SETWORKAREA)` = monitor minus own bars; reset on exit (`AppBarManager.SetWorkArea/ResetWorkArea`) | ✅ per monitor |
| Exit code | Exit code 1 when quitting on purpose, so Winlogon's `AutoRestartShell` does not start Explorer (Cairo `CairoApplication.OnExit`) | ✅ differently: `exit` logs off as in sway; after a crash Explorer is started as a rescue |
| Single instance | Named mutex | ✅ already: the IPC pipe is created with `FILE_FLAG_FIRST_PIPE_INSTANCE` |
| Set as login shell | Installer writes the Winlogon `Shell` value | ✅ `swinewm --set-shell` / `--unset-shell` (HKCU, per user) |
| `TaskbarButtonCreated` | As the shell, send the registered message to each new window so `ITaskbarList3` users (progress bars, thumbnail buttons) initialise (`TasksService.addWindow`) | 🔜 needed with a taskbar-like module |
| `SetTaskmanWindow` | Claims the task-manager window role (`TasksService`) | ❌ not needed |
| Shell service objects | As the shell, loads the SSOs (e.g. `stobject.dll`) that provide the volume/network/battery tray icons (`ShellServiceObject.cs`) | 🔜 with the tray |

## Next to Explorer

| Topic | How Cairo/ManagedShell does it | SwineWM |
| --- | --- | --- |
| Reserving space | Register each bar as an AppBar: `SHAppBarMessage(ABM_NEW / ABM_QUERYPOS / ABM_SETPOS)`, react to `ABN_POSCHANGED`, call `ABM_ACTIVATE` on `WM_ACTIVATE` and `ABM_WINDOWPOSCHANGED` (`AppBarManager.cs`, `AppBarWindow.cs`) | 🔜 For now the bar's space is reserved inside swinewm's layout only; unmanaged maximized windows can overlap it |
| Hiding Explorer's taskbar | Save the taskbar state, `ABM_SETSTATE(ABS_AUTOHIDE)`, hide `Shell_TrayWnd` and every `Shell_SecondaryTrayWnd` with `SetWindowPos(SWP_HIDEWINDOW)`, re-hide every 100 ms because Explorer shows it again; restore on exit (`ExplorerHelper.cs`) | ✅ `explorer_taskbar autohide` (re-hidden every second; original state saved for `--recover`) |
| Desktop windows | Explorer's desktop is `Progman`/`WorkerW` at the bottom of the Z order | ✅ the wallpaper window stays just above them |

## Windows and tasks

| Topic | How Cairo/ManagedShell does it | SwineWM |
| --- | --- | --- |
| Which windows count | Visible, no owner unless `WS_EX_APPWINDOW`, not `WS_EX_NOACTIVATE` unless `WS_EX_APPWINDOW`, not `WS_EX_TOOLWINDOW`, no `ITaskList_Deleted` window property (set by `ITaskbarList::DeleteTab`), not cloaked, and not an "immersive shell" window: `ApplicationFrameWindow`, `Windows.UI.Core.CoreWindow`, `StartMenuSizingFrame`, `Shell_LightDismissOverlay` without `WS_EX_WINDOWEDGE` (`ApplicationWindow.CanAddToTaskbar/getShowInTaskbar`) | ✅ added the property and immersive-window rules to `is_manageable` |
| Window events | `RegisterShellHookWindow` + the `SHELLHOOK` message: `HSHELL_WINDOWCREATED/DESTROYED/ACTIVATED`, `RUDEAPPACTIVATED` (0x8004), `FLASH` (0x8006), `REDRAW`, `WINDOWREPLACING/REPLACED`, `MONITORCHANGED` (16), `FULLSCREENENTER/EXIT` (53/54); plus a WinEvent hook for cloak/uncloak (`TasksService.cs`) | ✅ shell hook for `FLASH` (→ sway urgency) and fullscreen; WinEvents for the rest |
| Fullscreen apps | A window is fullscreen when its "effective" rectangle (client area for framed windows, else window rect) equals a monitor's; excluded: own process, invisible/minimized, cloaked, immersive shell windows, the `NonRudeHWND` property, layered windows that are transparent or no-activate. Bars hide while such a window is in front on their monitor (`FullScreenHelper.cs`) | ✅ managed windows → sway fullscreen; unmanaged ones (games) hide the bar on that output |

## The tray (notification area) — not yet implemented

How it works, for when SwineWM gets a Waybar `tray` module
(`TrayService.cs`, `NotificationArea.cs`, `NotifyIcon.cs`,
`ExplorerTrayService.cs`):

1. Programs call `Shell_NotifyIcon`, which sends `WM_COPYDATA` (`dwData` 1,
   a `SHELLTRAYDATA` holding a `NOTIFYICONDATA` with 32-bit handles) to the
   window found by `FindWindow("Shell_TrayWnd")`.
2. The shell creates its own `Shell_TrayWnd` (with a `TrayNotifyWnd` child)
   and broadcasts the registered `TaskbarCreated` message so running
   programs re-add their icons. With Explorer running, ManagedShell raises
   its `Shell_TrayWnd` to `HWND_TOPMOST` so `FindWindow` finds it first
   (Explorer's tray then stops receiving icons), and can also read the
   icons Explorer already has from its toolbar (`ExplorerTrayService`,
   `ReadProcessMemory`).
3. Messages `NIM_ADD/MODIFY/DELETE/SETFOCUS/SETVERSION` maintain the icon
   list (keyed by `hWnd`+`uID` or `guidItem`); `hIcon` is a shared handle,
   copied with `CopyIcon`. Balloon notifications (`NIF_INFO`) would map
   naturally onto the built-in Mako.
4. Clicks are forwarded to the owner with `PostMessage(uCallbackMessage)`,
   using the version-4 encoding (`NOTIFYICON_VERSION_4`: coordinates in
   `wParam`, event and id in `lParam`, `NIN_SELECT`, `WM_CONTEXTMENU`) after
   `AllowSetForegroundWindow`.
5. The same `Shell_TrayWnd` receives AppBar messages (`WM_COPYDATA`,
   `dwData` 0) from other programs when Explorer is gone, and must answer
   `ABM_GETTASKBARPOS`, `ABM_QUERYPOS/SETPOS`, `ABM_GETSTATE` and
   `ABM_GETAUTOHIDEBAR` (`AppBarManager.appBarMessageDelegate`).

## Other ideas noted

- Windows 11: WPF tool windows stop receiving `WM_DPICHANGED`; ManagedShell
  hides bars from the taskbar another way there (`AppBarWindow`). swinewm's
  bar windows are plain Win32 tool windows; watch for DPI-change issues.
- Volume, network and power flyouts can be opened through the undocumented
  `IShellExperienceManagerFactory` interfaces (`UWPInterop`); useful for bar
  modules, but undocumented.
- UWP/Store apps can be listed and launched through
  `IApplicationActivationManager` (`StoreAppHelper`).
