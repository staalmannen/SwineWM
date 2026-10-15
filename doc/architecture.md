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
  swinemsg/             IPC client (swaymsg)
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
- **IPC client threads** pass each request with `SendMessageTimeout` and
  write the reply back.
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

### Geometry

Windows 10 and 11 windows have invisible resize borders outside the visible
frame. The layout is in terms of the visible frame (from
`DWMWA_EXTENDED_FRAME_BOUNDS`), and the rectangle passed to `SetWindowPos`
is grown by the measured borders. Maximized windows are restored first.
Outputs use the monitor's work area for tiling (so the taskbar, and later
swinebar, stay visible) and the full monitor for fullscreen.

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
starting. Event subscriptions are the next step; swinebar needs them.

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
- **Rendering** (`win32/bar.c`): one topmost, non-activating tool window per
  output, painted with GDI into a back buffer. Clicks run the item's
  command (for example `workspace "2"`); the wheel switches workspaces.
  Translucent colours are blended over black for now.

## Built-in notifications (Mako)

Mako receives notifications over D-Bus. In swinewm, `notify-send` is a
command (so it works in bindings, over `swinemsg`, and as
`exec notify-send ...` from scripts started by swinewm), and `makoctl
dismiss|restore|invoke|mode|reload` likewise. `exec makoctl ...` and
`exec mako` lines in sway configs are routed to the built-in versions.

- **Config** (`swinewm/notify/notify.c`): `mako/config` in the swinewm
  config directories, in mako's format: global options, then `[criteria]`
  sections (`app-name`, `summary`, `body`, `urgency`, `category`, `mode`,
  `expiring`) applied in order. Styles are resolved when a notification
  arrives and again when modes change or the config is reloaded.
- **Behaviour** follows mako(5): `default-timeout` (0 = never), history of
  `max-history` dismissed or expired notifications for `makoctl restore`,
  `max-visible`, `sort`, `invisible`, modes such as `do-not-disturb`, the
  `format` specifiers `%a %s %b %h %t`, and button actions.
- **Rendering** (`win32/notify.c`): one topmost, non-activating window per
  visible notification, stacked from the configured `anchor` inside the
  usable area of the focused (or configured) output. Expiry uses a one-shot
  timer set for the next deadline.
- `swinemsg -t get_notifications` / `get_notification_history` list them
  (swinewm IPC extensions 1000 and 1001).

## Things deliberately left for later

- Title bars for tabbed/stacked containers and focus borders: Windows draws
  its own frames, so these will be small layered windows owned by swinewm.
  Until then a tabbed/stacked container just shows its active child.
- Mouse: focus follows mouse, `floating_modifier` drag/resize, tiling drag.
- Criteria, `for_window`, `assign`, marks, `swap`, `sticky`.
- `output` configuration (resolution/position/scale) and workspace-to-output
  assignment.
