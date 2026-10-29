# swinewm architecture

This describes the window manager in [`../swinewm/`](../swinewm/). The scope
and roadmap are in [`project_summary.md`](project_summary.md); per-command
status is in [`compatibility.md`](compatibility.md).

## Layout of the source tree

`swinewm/` mirrors the sway repository so that a file can be compared with
its counterpart (`swinewm/swinewm/tree/container.c` ↔
`sway/sway/tree/container.c`):

```text
swinewm/
  CMakeLists.txt        builds the core everywhere, the .exe files on Windows
  config.in             default config (sway's, adapted for Windows)
  common/               list_t, logging, string helpers, JSON writer, fopen
  include/              headers; include/swinewm/ mirrors sway/include/sway/
  swinewm/              the window manager
    commands.c          command parser and handler table
    commands/           one file per command or small group of commands
    config.c            config reader: blocks, $variables, include, deferral
    tree/               root, output, workspace, container, view, arrange
    input/              seat (focus stack), key names and binding lookup
    desktop/            transaction: push layout changes to the platform
    ipc-server.c        portable IPC request handling
    ipc-json.c          get_tree / get_workspaces / ... replies
    bar/                the built-in Waybar: config, modules, CSS, formats
    notify/             the built-in Mako: notifications, config, modes
    main.c              command-line options and start-up order
    win32/              the only code that includes <windows.h>
  resources/            installed defaults: Waybar and mako configs,
                        wallpaper, icon (also compiled in as fallbacks)
  cmake/                packaging (CPack/WiX MSI) and build helpers
  swinemsg/             IPC client (swaymsg)
  swinemenu/            launcher (wofi/wmenu): lists apps, asks swinewm
                        to exec the choice, themed by get_bar_config
  tests/                unit tests with a fake platform
```

## Core and platform

The core is portable C11 with no Windows headers. It talks to the OS only
through [`platform.h`](../swinewm/include/swinewm/platform.h):

| Core asks the platform to | Platform tells the core |
| --- | --- |
| set a window's geometry, show/hide it, activate it, close it | a window appeared / disappeared / was minimized / was focused / changed title |
| start a process (`exec`) | an output (monitor) was added, removed or resized |
| list config file locations, expand `~` and `%VAR%` | a key binding fired, an IPC request arrived |

The tests link [`tests/fake_platform.c`](../swinewm/tests/fake_platform.c)
instead of `win32/`, which records the requests. Everything about layout,
focus and commands can therefore be tested on Linux in CI, and the
behaviour can be compared with sway directly.

## The tree

Same model as sway:

```text
root
 ├─ output (one per monitor) ── workspace ── container ── container (view)
 │                                  │             └── container (view)
 │                                  └─ floating: container (view)
 ├─ fallback output (holds workspaces while no monitor is connected)
 └─ scratchpad (hidden containers)
```

- A **view** is one managed native window (`HWND`). A **container** holds
  either one view or child containers with a layout (`splith`, `splitv`,
  `tabbed`, `stacking`).
- The **seat focus stack** is a most-recently-focused list of nodes. As in
  sway it answers "which child of this node was focused last", which drives
  directional focus, where new windows are inserted, which tab is visible
  and where focus goes when a window closes.
- Functions keep sway's names and semantics (`container_split`,
  `container_reap_empty`, `workspace_wrap_children`, `workspace_squash`,
  `container_move_in_direction`, ...). When in doubt, the sway source is the
  reference; deviations are commented.

### Arrange and commit

sway separates *pending* from *current* state and commits changes in
transactions that wait for Wayland clients to acknowledge new sizes. Native
windows have no such handshake, so swinewm simplifies this:

1. Commands and events change the tree.
2. `transaction_commit_dirty()` runs `arrange_root()`, which computes each
   view's desired box and visibility (`view->pending`).
3. It then diffs against what was last applied (`view->current`) and calls
   the platform: hide first, then move/resize, then show, then activate the
   focused window if focus changed.

The whole tree is arranged every time; at desktop scale this is cheap and it
avoids dirty-tracking bugs.

## The Win32 platform layer

All of it is in [`../swinewm/swinewm/win32/`](../swinewm/swinewm/win32/).

### Threads and messages

Everything that touches the tree runs on the main thread. The others only
post or send messages to the shell's hidden top-level window:

- **WinEvent hooks** (out of context) only queue events and post
  `WM_SWINE_EVENTS`; the queue is drained on the main thread.
- **The low-level keyboard hook** must return quickly or Windows removes it,
  so it only looks the key up and posts `WM_SWINE_BINDING` with a copy of
  the command.
- **IPC client threads** pass each request to the main thread and write the
  reply back. Subscribed event frames use a per-client writer queue.
- **The tray thread** owns swinewm's `Shell_TrayWnd` (see the tray section
  below) and posts `WM_SWINE_TRAY` when icons change.
- A hidden top-level window (not a message-only window) is used because only
  top-level windows receive `WM_DISPLAYCHANGE` and `WM_SETTINGCHANGE`.

### Which windows are managed

Roughly what Alt+Tab shows: visible, not cloaked, top-level, not a tool
window unless it has `WS_EX_APPWINDOW`, has a caption, not one of the shell's
own classes (taskbar, desktop). Owned windows, standard dialogs (`#32770`)
and windows without a sizing border start out **floating**, matching sway's
handling of dialogs and fixed-size windows.

### Workspaces

Windows on inactive workspaces are hidden with `ShowWindowAsync(SW_HIDE)`,
the same approach as the earlier C++ prototype (removed; see git history).
Each view counts the show/hide events it caused, so they are not mistaken for the application
hiding itself (minimizing to the tray). An application that shows a hidden
window by itself (activated from its tray icon) pulls the user to that
workspace.

Hidden windows would be lost if swinewm crashed, so:

- the handles (with process ids) of hidden windows are written to
  `%LOCALAPPDATA%\swinewm\hidden-windows.txt`;
- `swinewm --recover` and every start of swinewm show them again;
- an unhandled-exception filter and the normal exit path show every hidden
  window.
- a separate watchdog process checks the shell window's responsiveness and
  watches the manager process. After three missed heartbeats it terminates a
  hung manager before running `--recover`; a clean-shutdown event prevents
  recovery on an intentional exit. When SwineWM is the login shell, the
  watchdog starts Explorer after recovery.

### Geometry

Windows 10 and 11 windows have invisible resize borders outside the visible
frame. The layout is in terms of the visible frame (from
`DWMWA_EXTENDED_FRAME_BOUNDS`), and the rectangle passed to `SetWindowPos`
is grown by the measured borders. Maximized windows are restored first.
Outputs use the monitor's work area for tiling (so the taskbar and the bars
stay visible; swinewm's own bars are reserved separately) and the full monitor for fullscreen.

### Never block on other applications

A hung application must not freeze the window manager. Calls on other
processes' windows are asynchronous: `SetWindowPos(... SWP_ASYNCWINDOWPOS)`,
`ShowWindowAsync`, `PostMessage(WM_CLOSE)`. Nothing uses `SendMessage` on a
foreign window.

### Focus

Windows restricts `SetForegroundWindow` to the process that received the
last input. When the first attempt is refused, swinewm sends a zero-length
synthetic mouse move and retries. Activation can still be refused, so
`platform_view_activate` returns the outcome and the core treats focus as a
request (see the archived design review). Foreground changes made by the
user (clicking a window) update the seat focus; changes involving windows on
hidden workspaces are ignored, because Windows briefly activates such windows
while a workspace is being hidden.

### Key bindings

`RegisterHotKey` cannot claim most Win+key combinations, so a
`WH_KEYBOARD_LL` hook checks every key press against the current mode's
bindings and swallows matches (and their key releases). Modifier state is
read with `GetAsyncKeyState` on each press, so it cannot get stuck after
Win+L. When Win or Alt was part of a binding, an unassigned key is injected
before its release so that the Start menu or a menu bar does not open.
Key names are xkb keysym names mapped to virtual-key codes; punctuation
follows the US layout for now (layout-aware lookup with `VkKeyScanEx` is a
to-do).

### Mouse

A `WH_MOUSE_LL` hook (`win32/mouse.c`) passes pointer input to the core
(`input/cursor.c`, a port of sway's `seatop_*` files). Motion is only
recorded in the hook and handled from a posted message, at most one pending
at a time. A press is handed to the core inside the hook, because the hook
has to answer at once whether the application gets it; the core starts a
move or resize there, and posts the commands of mouse bindings instead of
running them. A swallowed press has its release swallowed too. What is under
the pointer is `WindowFromPoint`, so menus and other windows swinewm does
not manage are seen as such. While swinewm code runs on the main thread
(`win32_busy`), the hook leaves input alone: a COM call that pumps messages
must not re-enter the core.

### Decorations

The core works out each view's decoration at every commit
(`view_update_decoration`: border colour from the `client.*` class, title
bar or not, square corners for tiled windows, opacity) and hands changes to
`platform_view_set_decoration`. `win32/decor.c` applies them on a worker
thread, because changing another application's window style waits for that
application: the DWM border colour (Windows 11), the title bar colours for
`border normal`, `WS_DLGFRAME` removed for `pixel` and `none`, and
`WS_EX_LAYERED` for `opacity`. What it changed per window goes into the
recovery file and is undone on exit, by the crash handler and by
`--recover`.

### exec

`exec` uses `CreateProcessW`, which searches `PATH` and app aliases such as
`wt.exe`; if that fails the command is handed to `ShellExecuteExW`, which
opens documents, URLs and `shell:` folders. There is no implicit shell:
write `exec cmd /c ...` for shell syntax. `SWINESOCK` is set in the
environment of started programs, like `SWAYSOCK`.

### IPC

The i3/sway wire format (`"i3-ipc"`, length, type, JSON payload) over the
named pipe `\\.\pipe\swinewm-ipc.<user>`, overridable with `SWINESOCK`.
Remote clients are rejected. The first pipe instance is created with
`FILE_FLAG_FIRST_PIPE_INSTANCE`, which also prevents a second swinewm from
starting. Subscribed clients receive events through per-client writer queues,
so a slow IPC reader cannot block the window-manager thread.

The manager starts a second `swinewm.exe --watch <pid> <shell-mode>` process. It probes
the hidden shell window every two seconds and watches the manager process
handle. Three consecutive missed probes cause the watchdog to terminate the
unresponsive manager and run `--recover`; an unexpected process exit also
triggers recovery. Intentional exit and restart signal a named event before
teardown, so they do not trigger recovery (the normal shutdown path restores
the desktop). If the manager was the login shell, the watchdog also starts
Explorer after recovery.

## The built-in bar (Waybar)

Waybar is a separate program that gets workspace and window state from
Sway over IPC. In swinewm the bar is part of the window manager and reads
the tree directly; only the configuration is Waybar's.

- **Enabling.** A sway `bar { }` block (any content), `bar` lines, or
  `exec waybar` turn the bar on; `bar mode invisible` hides it. `position
  top|bottom` from the sway block applies unless the Waybar config sets
  `position`. Other swaybar options are accepted and ignored.
- **Config.** `waybar/config.jsonc` (or `waybar/config`) and
  `waybar/style.css` in the swinewm config directories, in Waybar's
  formats. Without them, built-in defaults adapted from Waybar's own are
  used. The JSONC reader accepts comments and trailing commas.
- **Modules** (`swinewm/bar/bar.c`) turn state into labels using Waybar's
  `{name}` / `{:%H:%M}` format syntax, `format-icons`, `format-alt`,
  `states`, `max-length` and `on-click`. Unsupported modules are skipped
  with a log message.
- **Styling** (`swinewm/bar/css.c`) implements the CSS subset that Waybar
  themes rely on: type/#id/.class selectors with descendant combinators,
  `@define-color`, `alpha()`, colours, fonts, padding, margin, borders,
  and `box-shadow: inset` underlines. Rules with pseudo-classes such as
  `:hover` never match. Elements mirror Waybar's widget tree: `window#waybar`,
  a `label` per module with the module id (`#clock`, `#custom-media`) and
  state classes, and `#workspaces button.focused|visible|urgent`.
- **Space.** The bar's height (scaled by the monitor's DPI) is reserved on
  each output, and tiling uses the remaining area. The bar hides while the
  output shows a fullscreen window.
- **Display changes.** Display and DPI notifications are coalesced briefly
  before outputs are re-enumerated and the layout, bars and wallpaper are
  refreshed. This lets resolution and mixed-DPI changes settle before the
  bar and wallpaper windows are placed again. Windows-only smoke checks:
  change a monitor's resolution or refresh rate while a game is fullscreen,
  exit the game, and verify the layout, bar and wallpaper recover; repeat
  with monitors at different scaling percentages and verify display settings
  are restored by normal exit and `swinewm --recover` after a configured mode
  change.
- **Rendering** (`win32/bar.c`): one topmost, non-activating tool window per
  output, painted with GDI into a back buffer. Clicks run the item's
  command (for example `workspace "2"`); the wheel switches workspaces.
  Translucent colours are blended over black for now.

## The tray (Waybar's `tray` module)

On Linux, Waybar's tray shows StatusNotifierItems that programs such as
nm-applet register over D-Bus. On Windows the equivalent is the
notification area: programs call `Shell_NotifyIcon`, which sends
`WM_COPYDATA` to the first window of class `Shell_TrayWnd` that
`FindWindow` returns. When the bar has a `tray` module, swinewm becomes
that window (`win32/tray.c`, after ManagedShell's `TrayService`):

- It creates `Shell_TrayWnd` (never shown) with a `TrayNotifyWnd` child and
  keeps it topmost, re-raising it every 100 ms, so `FindWindow` finds it
  before Explorer's. It then broadcasts the registered `TaskbarCreated`
  message, and running programs add their icons again, now to swinewm.
- The window runs on its own thread: `Shell_NotifyIcon` waits for the
  answer, selected AppBar queries are handled directly when SwineWM is the
  shell, and other messages swinewm does not handle (including Explorer's
  private ones) are passed on to Explorer's taskbar with
  `SendMessageTimeout`. Neither can block the main thread. The icon table
  (window, id or GUID, callback message, version, a copy of the icon,
  tooltip, hidden state) is shared under a lock; the main thread copies it
  into the core (`swinewm/bar/tray.c`) on `WM_SWINE_TRAY`, and the bar
  module (`swinewm/bar/bar.c`) applies Waybar's options. Icons whose window
  is gone are pruned every 2 s.
- Clicks on an icon are sent to its program with `SendNotifyMessage`,
  encoded as Explorer does: button down/up (double click when quick
  enough), then `NIN_SELECT` after a left click and `WM_CONTEXTMENU` after a
  right click; version-4 icons get the position in `wParam`.
  `AllowSetForegroundWindow` lets the program show its menu.
  `Shell_NotifyIconGetRect` is answered with where the bar drew the icon.
- Balloon tips (`NIF_INFO`) become notifications of the built-in Mako.
- As the shell, swinewm also loads the SysTray shell service object
  (`stobject.dll`), which adds the volume, network and power icons that
  Explorer would otherwise show.
- Undo: when the tray module goes away or swinewm exits, the window is
  destroyed and `TaskbarCreated` is broadcast again, so programs give their
  icons back to Explorer. After a crash the window is dropped to the bottom
  of the z-order instead (its thread may be stuck) before the broadcast;
  `swinewm --recover` broadcasts too.

Explorer keeps showing the icons it had when swinewm took over (it never
learns they moved), so its own tray looks stale until swinewm exits;
`explorer_taskbar autohide` hides it anyway.

## Wallpapers (swaybg)

sway starts `swaybg` for `output … bg …`; swinewm draws the wallpaper itself
(`win32/wallpaper.c`). Each output with a background gets a non-activating
window covering the monitor at the bottom of the stacking order. Images are
decoded and scaled with the Windows Imaging Component (Fant
interpolation) and composed once per size; the swaybg modes `stretch`,
`fill`, `fit`, `center`, `tile` and `solid_color` are supported, with the
fallback colour around the image.

While Explorer runs, its desktop windows (`Progman`, `WorkerW`) are at the
very bottom. The wallpaper window is kept directly above them instead of
moving Explorer's windows, so exiting swinewm leaves the normal desktop as
it was.

## Built-in notifications (Mako)

Mako receives notifications over D-Bus, and makoctl and notify-send talk
to it there. In swinewm the D-Bus calls are IPC messages:

- `notify-send.exe` sends its arguments (`NOTIFY_SEND`, a JSON array) and
  gets the id back. With actions or `--wait` it first subscribes to the
  `notification` event on the same connection, then waits for its
  notification's `action` (prints the key and sends `CLOSE_NOTIFICATION`,
  as libnotify's notify-send closes it) or `close` event.
- `makoctl.exe` sends its arguments (`MAKOCTL`) and prints the reply's
  `output` (`list`, `history`, `mode`). `menu` runs in makoctl.exe: it
  gets `list -j`, starts the menu program with pipes, and sends `invoke`.
- `notify-send` and `makoctl` are also swinewm commands (bindings,
  `swinemsg`), and `exec notify-send|makoctl ...` lines run them inside
  swinewm, except `makoctl menu`. `exec mako` is ignored.

- **Store and config** (`swinewm/notify/notify.c`, ported from mako's
  `criteria.c`, `notification.c` and `config.c`): `mako/config` in the
  swinewm config directories, in mako's format. Notifications are kept in
  display order (mako's `insert_notification` for `sort`); `group-by`
  gathers a group where its first member is and numbers it
  (`group_notifications`), again whenever one closes. Styles are matched
  twice, as mako does: once to find the notification's output and anchor,
  then with `output=`/`anchor=` criteria; and again whenever the list,
  the modes or the config change, and before drawing.
- **Behaviour** follows mako(5): `default-timeout` (0 = never), history of
  `max-history` dismissed or expired notifications for `makoctl restore`,
  `max-visible` per output and anchor with the "(%h more)" placeholder,
  modes such as `do-not-disturb`, and bindings. An action invoked with
  `makoctl invoke` closes the notification unless a `notify-send.exe`
  waits for it (that one closes it, as an application would).
- **Windows toasts** (`win32/toast.c`): with `windows-notifications=`
  on, a thread reads `UserNotificationListener` (WinRT, called through
  vtables, the runtime loaded from combase.dll) when Windows writes its
  notification store under `%LOCALAPPDATA%\Microsoft\Windows\Notifications`
  and every two seconds, and posts new and removed toasts to the main
  thread. New ones become notifications (icon `shell:AppsFolder\<AUMID>`);
  removed ones close theirs; `platform_notification_closed` removes a
  dismissed one from Windows' list.
- **Rendering** (`win32/notify.c`): one topmost, non-activating layered
  window per visible notification, stacked from the configured `anchor`
  inside the usable area of the focused (or configured) output, laid out
  as mako's `render_notification`: the border is a band along the rounded
  edge (anti-aliased, per-pixel alpha through `win32/canvas.c`, shared with
  the bar), the progress bar fills the inside, the icon (WIC for images,
  the shell for `.ico`/`.exe` and stock icons) sits left, right, above or
  below the text. Expiry uses a one-shot timer set for the next deadline.

## Screenshots (grim, slurp, wl-copy, grimshot)

sway configs run these through `sh`; swinewm has no shell, so `exec`
lines with them are taken apart in `commands/notify.c` (`exec_builtin`:
`&&` chains, `|`, `$(...)`) and run by `commands/grim.c`:

- `grim` captures with `platform_screenshot` (`win32/screenshot.c`: GDI
  `BitBlt`, WIC to encode; with `| wl-copy` a 24-bit `CF_DIB` and a "PNG"
  clipboard format).
- `$(slurp)`, `slurp | ...` and grimshot's area, window and anything
  first call `platform_slurp` (`win32/slurp.c`: one layered topmost window
  per output, drawn as slurp's `render.c`, pointer handling from slurp's
  `main.c`; Escape, Space and Shift come from the keyboard hook, so the
  overlay needs no focus). The capture and the rest of the `&&` chain are
  kept until `grim_slurp_done`, which the overlay calls 150 ms after it is
  gone, so that it is not in the picture.

## Urgency and fullscreen requests

- **Urgency.** Windows has no urgency hint, but an application that wants
  attention flashes its taskbar button (`FlashWindowEx`), which reaches
  shell-hook windows as `HSHELL_FLASH`. swinewm registers its window with
  `RegisterShellHookWindow` and turns that into sway's view urgency: the
  workspace gets the `urgent` class in the bar until the window is
  focused. sway's `urgent enable|disable|allow|deny` command works too.
- **Fullscreen.** An application that makes itself fullscreen (F11 in a
  browser) is detected from location changes and `HSHELL_FULLSCREENENTER`/
  `EXIT`: its "effective" rectangle (client area if it has a frame) equals
  a monitor. A managed window then becomes a sway fullscreen container, as
  an xdg-toplevel fullscreen request does in sway, and returns to the
  layout when it leaves fullscreen. An unmanaged fullscreen window in
  front (a game) only hides the bar on its monitor.

## Running as the Windows shell

`win32/shell.c`. swinewm acts as the shell when no shell window exists at
start-up, typically after `swinewm --set-shell` made it the user's logon
shell. `--shell` and `--no-shell` override the detection, but `--shell` is
refused while Explorer (or any other shell) runs: two shells would fight
over the work area. Then it:

- logs to `%LOCALAPPDATA%\swinewm\swinewm.log` instead of a console when
  started without a terminal (by Winlogon);
- creates a bottom-most desktop window and registers it with
  `SetShellWindow` (some programs look for the shell through
  `GetShellWindow`);
- sets the `ShellDesktopSwitchEvent` event so Windows completes the logon;
- starts the user's startup programs (Run/RunOnce keys and Startup folders,
  honouring entries disabled in Task Manager), once per logon session;
- owns the work area: each monitor's work area is set to the monitor minus
  the bar, so maximized windows outside the tiling respect the bar, and
  reset on exit. The change is announced with a posted `WM_SETTINGCHANGE`
  (not `SPIF_SENDCHANGE`, which waits for every window), and swinewm does
  not follow work area changes made by others;
- makes `exit` log off, as sway's does;
- after a crash, starts Explorer (with a short delay, so that Explorer finds
  no shell window and becomes the shell) so the user is never stranded.

Next to Explorer, `explorer_taskbar autohide` switches Explorer's taskbar to
auto-hide and keeps it hidden, restoring the original setting on exit,
after a crash and with `--recover`.

Next to Explorer, each exclusive bar is registered as an AppBar
(`win32/appbar.c`): `ABM_NEW`, `ABM_QUERYPOS` and `ABM_SETPOS` with the
bar's strip, again on `ABN_POSCHANGED`, and `ABM_WINDOWPOSCHANGED` when a
bar moves. `SHAppBarMessage` waits for Explorer, so a worker thread makes
the calls and the main thread only queues them; a registration is sent
only when a bar's strip changes. Explorer then takes the strips out of the
work area; `win32_appbar_work_area` gives swinewm's own strips back before
an output's usable area is computed, because swinewm reserves its bars in
its own layout (`place_bars`) and taking them out twice would shrink the
layout and move the bars in a loop. On exit the bars are removed (the
thread gets two seconds); after a crash Explorer drops the registrations
of the destroyed windows itself.

The approach follows Cairo Desktop's ManagedShell library; see
[`research/windows-shell.md`](research/windows-shell.md) for the details
and what remains.

## Known gaps

The feature-by-feature status lives in
[`compatibility.md`](compatibility.md). The larger remaining areas include
decorations, IPC subscriptions/events, script-driven
Waybar modules. These are separate projects with different
Windows dependencies; do not infer support from the upstream format alone.
