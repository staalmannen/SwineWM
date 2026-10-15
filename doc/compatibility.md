# Compatibility with sway

What swinewm does with each sway config/runtime command. The goal is that an
unmodified sway config loads: commands with no meaning on Windows (or not
implemented yet but harmless to skip) are **accepted and ignored** with an
info message. Anything else unknown is an error with a line number, as in
sway.

Legend: ✅ implemented as in sway · 🟡 partly implemented · ⚪ accepted and
ignored · ❌ not implemented (error)

## Config file

| Feature | Status | Notes |
| --- | --- | --- |
| Location | 🟡 | `-c <file>`, else `%USERPROFILE%\.swinewm\config`, `%XDG_CONFIG_HOME%\swinewm\config`, `%APPDATA%\swinewm\config`, then `config` next to `swinewm.exe` |
| Comments, `\` line continuation, `{` on the next line | ✅ | |
| Blocks (`mode "x" { }`, `bar { }`, `input … { }`) | ✅ | Lines are prefixed with the block header, as in sway |
| `set $var value`, `$var` replacement | ✅ | Longest variable name wins; `$$` escapes |
| `include <path>` | 🟡 | Single files; `~` and `%VAR%` expanded; no glob patterns yet |
| `exec` / `exec_always` deferral | ✅ | Run after the config is read; `exec` is skipped on reload |
| `swinewm -C` (validate config) | ✅ | |
| UTF-8 paths and BOM | ✅ | |

## Commands

| Command | Status | Notes |
| --- | --- | --- |
| `bindsym [--release] [--no-repeat] [--locked] <combo> <command>` | 🟡 | Keyboard only. `--to-code`, `--no-warn`, `--inhibited`, … accepted. Mouse buttons not yet |
| `unbindsym` | ✅ | |
| `bindcode`, `bindswitch`, `bindgesture` | ⚪ | |
| `mode <name>`, `mode <name> { … }` | ✅ | `--pango_markup` accepted |
| `set` | ✅ | |
| `exec`, `exec_always` | ✅ | See architecture: CreateProcess, then ShellExecute; no implicit shell |
| `exit` | ✅ | Shows all hidden windows and quits |
| `reload` | ✅ | |
| `kill` | ✅ | Sends `WM_CLOSE` (the application may ask to save) |
| `nop` | ✅ | |
| `focus left/right/up/down` | ✅ | Tree-based like sway, crosses outputs, honours `focus_wrapping` |
| `focus parent/child`, `focus mode_toggle/floating/tiling` | ✅ | |
| `focus output <name\|direction>` | ✅ | |
| `focus prev/next [sibling]` | ❌ | |
| `move left/right/up/down [<n> px]` | ✅ | Port of sway's `container_move_in_direction`; floating windows move by n px |
| `move [container\|window] [to] workspace <name\|number n\|next\|prev\|…\|back_and_forth>` | ✅ | Focus stays on the current workspace, like sway |
| `move [container\|window] [to] output <name\|direction>` | ✅ | |
| `move [to] scratchpad`, `scratchpad show` | ✅ | |
| `move [absolute] position center\|<x> <y>` | 🟡 | Floating windows; coordinates relative to the output's work area |
| `move workspace to output …` | ❌ | |
| `split h/v/t/none`, `splith`, `splitv`, `splitt` | ✅ | |
| `layout splith/splitv/stacking/tabbed/default`, `layout toggle [split\|all\|…]` | ✅ | Tabbed/stacked show only the active child; no title bars yet |
| `workspace [number] <name>`, `next`, `prev`, `next_on_output`, `prev_on_output`, `back_and_forth`, `--no-auto-back-and-forth` | ✅ | |
| `workspace <name> output …`, `workspace <name> gaps …` | ⚪ | |
| `workspace_auto_back_and_forth` | ✅ | |
| `floating enable/disable/toggle` | ✅ | |
| `fullscreen [enable/disable/toggle] [global]` | 🟡 | `global` behaves like per-output fullscreen |
| `resize grow/shrink width/height [<n> [px\|ppt]]` | ✅ | |
| `resize grow/shrink up/down/left/right`, `resize set` | ❌ | |
| `gaps inner/outer/horizontal/vertical/top/right/bottom/left <n>` | ✅ | Config default |
| `gaps <type> all\|current set\|plus\|minus\|toggle <n>` | ✅ | Runtime |
| `default_orientation`, `workspace_layout` | ✅ | |
| `focus_wrapping` | ✅ | |
| `focus_follows_mouse`, `floating_modifier` | 🟡 | Parsed and stored; mouse handling not implemented yet |
| `smart_gaps`, `smart_borders`, `hide_edge_borders` | ⚪ | |
| `border`, `default_border`, `default_floating_border`, `client.*`, `font`, `title_align`, `titlebar_*`, `show_marks` | ⚪ | Decorations not drawn yet |
| `for_window`, `assign`, `no_focus`, criteria `[…]` | ⚪ / ❌ | Config lines ignored; runtime criteria are an error |
| `output`, `input`, `seat` | ⚪ | Windows settings apply |
| `bar` | ⚪ | swinebar will be a separate Waybar-like program |
| `xwayland`, `swaybg_command`, `swaynag_command`, `max_render_time`, `allow_tearing`, `primary_selection`, `inhibit_idle`, `shortcuts_inhibitor`, `tiling_drag*`, `mouse_warping`, `popup_during_fullscreen`, `focus_on_window_activation`, `force_display_urgency_hint`, `new_window`, `new_float`, `floating_minimum_size`, `floating_maximum_size` | ⚪ | |
| `mark`, `unmark`, `swap`, `sticky`, `rename`, `title_format`, `opacity`, `urgent`, `create_output` | ❌ | |

## Keys

- Modifiers: `Shift`, `Control`/`Ctrl`, `Mod1`/`Alt`, `Mod4`/`Super`/`Logo`
  (the Windows key). `Mod2` (Num Lock) and Caps Lock never block a match.
- Key names are xkb keysym names (`Return`, `space`, `minus`, `Left`, `F1`,
  `Print`, `XF86AudioMute`, …), case-insensitive for letters.
- Punctuation keys follow the US layout for now.
- Windows reserves **Win+L** and **Ctrl+Alt+Del**; they cannot be bound.

## IPC (`swinemsg`, or any i3/sway IPC library pointed at the pipe)

| Message | Status |
| --- | --- |
| `RUN_COMMAND` | ✅ |
| `GET_WORKSPACES`, `GET_OUTPUTS`, `GET_TREE`, `GET_VERSION` | ✅ (subset of sway's fields) |
| `GET_BINDING_MODES`, `GET_BINDING_STATE`, `GET_CONFIG` | ✅ |
| `GET_MARKS`, `GET_BAR_CONFIG`, `GET_INPUTS`, `GET_SEATS` | Empty lists |
| `SEND_TICK` | 🟡 replies success, no event |
| `SUBSCRIBE` and events | ❌ next on the list (needed by swinebar) |

`get_tree` reports native window properties as `app_id` (the executable
name, e.g. `notepad.exe`), `pid`, `window` (the HWND) and
`window_properties.class`/`title`.
