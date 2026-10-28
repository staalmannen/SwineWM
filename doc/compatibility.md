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
| `include <path>` | ✅ | Relative to the including file; `~` and `%VAR%` expanded; wildcards (`*`, `?`, `[…]`) in the file name, matched in sorted order and ignoring case; each file at most once; a missing file is not an error (as in sway) |
| Config errors (swaynag) | ✅ | Shown in a persistent native window with the full error list and Reload config/Dismiss buttons; it goes away after a reload without errors. `swaynag_command` is ignored |
| `exec` / `exec_always` deferral | ✅ | Run after the config is read; `exec` is skipped on reload |
| `swinewm -C` (validate config) | ✅ | |
| UTF-8 paths and BOM | ✅ | |

## Commands

| Command | Status | Notes |
| --- | --- | --- |
| `bindsym [--release] [--no-repeat] [--locked] <combo> <command>` | 🟡 | `--to-code`, `--no-warn`, `--inhibited`, … accepted |
| `bindsym [--whole-window] [--border] [--exclude-titlebar] <mod>+button<n> <command>` | 🟡 | Mouse bindings: `button1`–`button9` (4–7 are the wheel) and `BTN_*` names. Without a flag a binding is for title bars, as in sway: swinewm draws title bars only for tabbed and stacked containers (a window's own title bar belongs to the window), so elsewhere only `--whole-window` bindings fire on windows (and on the desktop). `--border` never fires: Windows windows have no border swinewm can tell apart. |
| `bindcode [flags] <mod>+<keycode> <command>`, `unbindcode` | 🟡 | Numeric Windows virtual-key values (decimal, 1–255), not Linux evdev keycodes |
| `unbindsym` | ✅ | |
| `bindswitch`, `bindgesture` | ⚪ | |
| `mode <name>`, `mode <name> { … }` | ✅ | `--pango_markup` accepted |
| `set` | ✅ | |
| `exec`, `exec_always` | ✅ | See architecture: CreateProcess, then ShellExecute; `%VAR%` expanded; no implicit shell. `waybar`, `mako`, `makoctl`, `notify-send`, `grim` run the built-in versions; `a && b` chains of those run like `sh` would. Store apps: `exec shell:AppsFolder\<AppUserModelID> [args]` starts them through `IApplicationActivationManager`, also as the shell (PowerShell's `Get-StartApps` lists the IDs) |
| `exit` | ✅ | Shows all hidden windows and quits |
| `reload` | ✅ | |
| `restart` | ✅ | i3's command (sway has none): exits cleanly without logging off and starts `swinewm.exe` again with the same arguments; hidden workspaces are shown and re-tiled. For updating a running swinewm |
| `kill` | ✅ | Sends `WM_CLOSE` (the application may ask to save) |
| `nop` | ✅ | |
| `focus left/right/up/down` | ✅ | Tree-based like sway, crosses outputs, honours `focus_wrapping` |
| `focus parent/child`, `focus mode_toggle/floating/tiling` | ✅ | |
| `focus output <name\|direction>` | ✅ | |
| `focus prev/next [sibling]` | ✅ | |
| `move left/right/up/down [<n> px]` | ✅ | Port of sway's `container_move_in_direction`; floating windows move by n px |
| `move [container\|window] [to] workspace <name\|number n\|next\|prev\|…\|back_and_forth>` | ✅ | Focus stays on the current workspace, like sway |
| `move [container\|window] [to] output <name\|direction>` | ✅ | |
| `move [container\|window] [to] mark <mark>` | ✅ | |
| `move [to] scratchpad`, `scratchpad show` | ✅ | |
| `move [absolute] position center\|<x> <y>` | 🟡 | Floating windows; coordinates relative to the output's work area |
| `move workspace [to] [output] <name\|direction\|current>` | ✅ | The workspace remembers the output for when monitors come and go |
| `split h/v/t/none`, `splith`, `splitv`, `splitt` | ✅ | |
| `layout splith/splitv/stacking/tabbed/default`, `layout toggle [split\|all\|…]` | ✅ | Tabbed and stacked containers get title bars drawn by swinewm (tabs side by side, or one row per child), clickable and scrollable as in sway; the windows in them lose their own title bar (`border normal`) |
| `workspace [number] <name>`, `next`, `prev`, `next_on_output`, `prev_on_output`, `back_and_forth`, `--no-auto-back-and-forth` | ✅ | |
| `workspace <name> output <outputs…>`, `workspace <name> gaps <type> <n>` | ✅ | The first connected output in the list; workspaces return to it when the monitor is plugged back in |
| `workspace_auto_back_and_forth` | ✅ | |
| `floating enable/disable/toggle` | ✅ | |
| `fullscreen [enable/disable/toggle] [global]` | 🟡 | `global` behaves like per-output fullscreen |
| `resize grow/shrink width/height/up/down/left/right [<n> [px\|ppt]] [or <n> [px\|ppt]]` | ✅ | Port of sway's `resize.c`; floating windows are at least 75×50 and at most the output's work area |
| `resize set [width] <w> [height] <h>` | ✅ | |
| `gaps inner/outer/horizontal/vertical/top/right/bottom/left <n>` | ✅ | Config default |
| `gaps <type> all\|current set\|plus\|minus\|toggle <n>` | ✅ | Runtime |
| `default_orientation`, `workspace_layout` | ✅ | |
| `focus_wrapping` | ✅ | |
| `focus_follows_mouse yes\|no\|always` | ✅ | Port of sway's `seatop_default.c`. Not while a mouse button is held, and never away from a window swinewm does not manage that has the focus (a menu or launcher); it also keeps a visible owned dialog focused instead of activating its owner underneath it |
| `floating_modifier <mod> [normal\|inverse]` | ✅ | $mod + left button moves floating windows and drags tiled ones (`tiling_drag`), $mod + right button resizes either, from the nearest corner; Shift keeps the aspect ratio of a floating window. A click with the modifier never reaches the window, and Win released after it does not open the Start menu |
| `tiling_drag enable\|disable\|toggle`, `tiling_drag_threshold <px>` | ✅ | Drop zones as in sway, shown with a translucent rectangle; dropped on a window's middle the two swap. Dragging by title bar waits for the threshold |
| `mouse_warping output\|container\|none` | ✅ | After `focus` commands and `move workspace to output`, as in sway |
| `border none\|normal\|pixel [<n>]\|toggle`, `default_border`, `default_floating_border` | 🟡 | The border is the window's own DWM border (Windows 11), always 1 px whatever `<n>` says. `normal` keeps the window's own title bar; `pixel` and `none` remove it (for windows that have a standard one). `csd` is refused |
| `client.focused\|focused_inactive\|unfocused\|urgent <border> <background> <text> [<indicator> [<child_border>]]` | 🟡 | `child_border` colours the window border; with `border normal`, `background` and `text` colour the window's own title bar (standard title bars only, Windows 11). `indicator` colours the drop zone of `tiling_drag`. `client.placeholder` and `client.background` are accepted and ignored, as in sway |
| `smart_borders on\|off\|no_gaps`, `hide_edge_borders smart\|smart_no_gaps` | ✅ | |
| `hide_edge_borders none\|vertical\|horizontal\|both` | 🟡 | A DWM border cannot hide single edges, so it goes only when every edge would: with `both`, for a window that fills its workspace |
| `smart_gaps on\|off\|toggle\|inverse_outer` | ✅ | As in sway: no gaps around a lone visible window (`on`), or outer gaps only around one (`inverse_outer`); the other tabs of a tabbed or stacked container do not count |
| `font`, `title_align`, `titlebar_padding`, `titlebar_border_thickness`, `client.focused_tab_title` | ✅ | For the tabs of tabbed and stacked containers. `font` takes a Pango description (`pango:DejaVu Sans Mono 10`); generic families map to Segoe UI, Times New Roman and Consolas |
| `title_format <format>` | 🟡 | Tab titles: `%title`, `%app_id`, `%class`, `%instance` (the class) and `%shell` (`win32`); no Pango markup |
| `hide_edge_borders --i3` | ✅ | No tab for a lone child |
| `show_marks yes\|no\|toggle` | ✅ | On the tabs swinewm draws (tabbed and stacked containers), right-aligned as `[mark]`; marks starting with `_` are hidden, as in sway. A window's own title bar cannot show them |
| `for_window`, `assign` (`workspace`, `number`, `output`, `→`), `no_focus` | ✅ | As in sway: rules run when a window appears and again when its title changes (once per rule and window); `assign` picks the workspace, `no_focus` keeps the focus where it was |
| Criteria `[…] <commands>` | ✅ | All tokens; what they match on Windows is listed under "Criteria" below. Regular expressions use a built-in PCRE-like engine (no look-around or back-references) |
| `output <name\|*> bg <file> <stretch\|fill\|fit\|center\|tile> [<#color>]`, `output … bg <#color> solid_color` | ✅ | swaybg's options; PNG, JPEG, BMP, GIF, TIFF (anything the Windows Imaging Component reads). `~`, `%VAR%` and paths relative to the config file |
| `output … mode\|resolution\|res <w>x<h>[@<r>Hz]`, `position\|pos <x>,<y>`, `transform <0\|90\|180\|270> [clockwise\|anticlockwise]` | 🟡 | Through Windows' display settings, as a temporary change: Windows does not save it, and swinewm restores the saved settings when it exits or crashes and with `--recover`. Only modes the driver lists; positions are relative to where the config puts the primary monitor (Windows keeps it at 0,0); `flipped-*` rotates without mirroring |
| `output <identifier>` | ✅ | The monitor name shown in Windows' display settings (e.g. `"DELL U2720Q"`); also in `workspace … output` and `get_outputs` (`model`) |
| `input <identifier> xkb_switch_layout next\|prev\|<index>` | ✅ | Switches between the keyboard layouts installed in Windows' language settings (Settings → Time & language), in Windows' order. `*`, `type:keyboard` and `swinewm:keyboard` name the keyboard. A layout switched with Windows' own hotkey (Alt+Shift, Win+Space) is noticed within a second |
| `output … scale`, `enable`, `disable`, `dpms`, `power`, `adaptive_sync`, `subpixel`, …, other `input` options (`xkb_layout`, `repeat_rate`, …), `seat` | ⚪ | Windows settings apply (scaling is per monitor in Settings → Display; keyboard layouts in Time & language) |
| `bar { … }`, `bar <option>` | 🟡 | Enables the built-in Waybar-compatible bar; `position`, `mode invisible\|dock\|toggle` honoured, other swaybar options ignored (see below) |
| `xwayland`, `swaybg_command`, `swaynag_command`, `max_render_time`, `allow_tearing`, `primary_selection`, `inhibit_idle`, `shortcuts_inhibitor`, `popup_during_fullscreen`, `focus_on_window_activation`, `force_display_urgency_hint`, `new_window`, `new_float`, `floating_minimum_size`, `floating_maximum_size` | ⚪ | |
| `urgent enable\|disable\|allow\|deny` | ✅ | Windows' own "urgency" is a flashing taskbar button (`FlashWindowEx`); it marks the view and its workspace urgent until focused |
| Application-requested fullscreen | ✅ | A window that makes itself fullscreen (F11) becomes a sway fullscreen container and returns when it leaves fullscreen |
| `mark [--add\|--replace] [--toggle] <id>`, `unmark [<id>]` | ✅ | Drawn on tabs (`show_marks`) |
| `swap container with id\|con_id\|mark <arg>` | ✅ | `id` is the window handle |
| `sticky enable\|disable\|toggle` | ✅ | Floating windows follow workspace switches |
| `rename workspace [<old>\|number <n>] to <new>` | ✅ | |
| `opacity [set\|plus\|minus] <0..1>` | ✅ | Makes the window layered (`WS_EX_LAYERED`); undone on exit, after a crash and by `--recover` |
| `create_output` | ❌ | |
| `explorer_taskbar autohide\|show\|toggle` | ✅ | swinewm extension: while running next to Explorer, keep Explorer's taskbar auto-hidden (restored on exit) |
| `windows_theme dark\|light\|auto\|none` | ✅ | swinewm extension (config only): sets Windows' light/dark mode for apps and the system while swinewm runs; `auto` picks dark if the bar's background is dark. The user's own mode comes back on exit, after a crash and with `--recover`. Default `none`: Windows' setting is left alone |

## Criteria

sway's tokens and what they match here. Values are regular expressions
(searched anywhere in the value, as with PCRE; anchor with `^…$`), or
`__focused__` for the focused window's value.

| Token | Matches | Example |
| --- | --- | --- |
| `app_id` | The executable's file name, **ignoring case** (Windows file names do) | `[app_id="^firefox"]` matches `firefox.exe` |
| `instance` | The executable's name without `.exe`, ignoring case | `[instance="^Discord$"]` |
| `class` | The window class (case-sensitive) | `[class="MozillaWindowClass"]` |
| `title` | The window title (case-sensitive; `(?i)` for ignoring case) | `[title="(?i)picture-in-picture"]` |
| `shell` | Always `native` | |
| `window_type` | `normal` (tiled by default) or `dialog` (dialogs, fixed-size windows); other types never match | Newly mapped floating windows are raised without stealing focus, keeping owned dialogs above their parent |
| `id` | The window handle (HWND) as a decimal number | |
| `pid`, `con_id`, `con_mark`, `workspace`, `floating`, `tiling`, `urgent=first\|latest…`, `all` | As in sway | |
| `window_role`, `sandbox_engine`, `sandbox_app_id`, `sandbox_instance_id`, `tag` | Never match (Windows has none of these) | |

Regular expressions: literals, `.`, `[…]` classes (with ranges, `\d \w \s`
and `[:alpha:]`-style names), `^ $ \b`, `* + ? {n,m}` (and lazy `*?`),
groups, `|`, `(?i)`. Not supported: look-around, back-references,
Unicode properties (`\p{…}`).

## Bar (Waybar config)

| Feature | Status | Notes |
| --- | --- | --- |
| `waybar/config.jsonc` / `waybar/config`, JSONC | ✅ | One bar, or an array of bars. Bars on the same edge stack in config order, and a later bar takes its edge of the space earlier bars left (as Waybar's layer-shell surfaces do) |
| `height`, `width`, `spacing`, `position` (top/bottom/left/right), `output`, `margin`, `margin-top/right/bottom/left`, `exclusive`, `name` | ✅ | `height` is the thickness of a top or bottom bar and `width` of a left or right one (without it a vertical bar is twice the line height); bars always span the monitor. Left and right bars lay their modules out top to bottom. `window#waybar` has the position and `name` as classes. No `layer`, `mode`, `start_hidden`, `ipc` |
| `modules-left/center/right` | ✅ | |
| `sway/workspaces` | 🟡 | `format` with `{name} {value} {index} {icon} {output}`, `format-icons`, `all-outputs`, `disable-scroll`, `disable-click`; no persistent workspaces |
| `sway/mode`, `sway/window`, `sway/scratchpad` | ✅ | window: `{title} {app_id}`, `max-length` |
| `clock` | 🟡 | `format`, `format-alt` (click), strftime specs; `tooltip-format` with `{calendar}` (the month, weeks from Monday, in a fixed-width font for `<tt>`); no `calendar` options, time zones or `actions` |
| `cpu`, `memory`, `battery` | 🟡 | Main fields, `states`, `format-icons`, battery `format-charging/plugged/full` |
| `sway/language` | ✅ | The active Windows keyboard layout: `{}`/`{short}` (the region, `us`), `{shortDescription}` (the language, `en`), `{long}` (`English (United States)`), `{variant}` (a non-default layout such as `Dvorak`), `{flag}` (emoji; Windows' fonts draw two letters); `hide-single-layout`; `#language.<short>` styling. The default bar shows it with more than one layout, and a click switches |
| `custom/*` | ✅ | `exec` every `interval` (or `"once"`), once and on a `signal`, or continuously (one update per line, `restart-interval`); `exec-if`, `exec-on-event`, `return-type: json` (`text`, `alt`, `tooltip`, `class`, `percentage`) or i3blocks lines, `format` with `{} {text} {alt} {icon} {percentage}`, `format-icons` (string, array, object by `alt`), `escape`, `hide-empty-text`, `on-update`, `max-length`. Scripts run through `cmd.exe /c` without a window (pipes, `%VARIABLES%`, `.bat` files; PowerShell as `powershell -NoProfile -File x.ps1`), with `%WAYBAR_OUTPUT_NAME%` set to the first monitor (one script per module, not per bar); output in UTF-8 or the console code page. Signals: `pkill -RTMIN+<n> waybar` (also `-SIGRTMIN+n`, `-s`, `killall`) in `exec`, an `on-click` or `swinemsg exec …`. No tooltips or `image-path` yet |
| `tray` | 🟡 | Windows notification area icons. `icon-size`, `spacing`, `show-passive-items` (icons a program hides), `reverse-direction`, `orders`, `ignore-list`, `icons` (`false`, or an `.ico` file; keys are the program, `"Discord.exe"` or `"discord"`); `#tray > .active\|.passive` styling. Left/middle/right clicks go to the program as Explorer sends them; balloon tips become notifications. Tooltips show the program's text, or for icons that draw their own (version 4 without `NIF_SHOWTIP`) the program gets `NIN_POPUPOPEN`/`NIN_POPUPCLOSE`; the pointer is reported with `WM_MOUSEMOVE` as Explorer does. No scroll (Windows' notification area has no scroll message), no `-gtk-icon-effect` |
| `network` | ✅ | The interface of the default route, or `interface` (wildcards match the name, e.g. `"Wi-Fi"`); states `wifi`, `ethernet`, `linked`, `disconnected`, `disabled` (Wi-Fi radio off) with `format-<state>[-<threshold>]` and classes; `{ifname} {ipaddr} {gwaddr} {netmask} {netmask6} {cidr} {cidr6} {essid} {bssid} {signalStrength} {signaldBm} {signalStrengthApp} {frequency} {icon}`, the `bandwidth*` values over each `interval`, `{rxBitrate} {txBitrate} {linkSpeed}` (Waybar's unit specs such as `{bandwidthDownBits:>}` work), `family`. Windows 11 24H2 and later hide the Wi-Fi name (`{essid}`, `{bssid}`) unless location access is allowed for desktop apps; the interface still counts as Wi-Fi. Link state is read at least every 5 s |
| `pulseaudio`, `wireplumber` | ✅ | The default Windows output and input: `{volume} {desc} {icon} {format_source} {source_volume} {source_desc}` (wireplumber: `{node_name}`), `format-muted`, `format-bluetooth[-muted]`, `format-source[-muted]`, `states`, `format-icons` by port (`speaker`, `headphone`, `headset`, `hdmi`, `hands-free`, `phone`, `hifi`, `-muted`); scrolling changes the volume (`scroll-step`, `max-volume`, `reverse-scrolling`, `target`). No `ignored-sinks`/`sink-mapping` |
| `disk` | ✅ | `path` (`/` means the Windows drive, `%SystemDrive%`; any folder works, e.g. `D:\\`) or `paths` with `header` and `separator`; `{percentage_used} {percentage_free} {total} {used} {free} {path} {specific_*}` with `unit`, `states` |
| `temperature` | 🟡 | ACPI thermal zones from Windows' "Thermal Zone Information" counters (`thermal-zone` is their index); hidden when Windows reports none, as on many desktops. `{temperatureC} {temperatureF} {temperatureK} {icon}`, `warning-threshold`, `critical-threshold`, `format-warning`, `format-critical`. `hwmon-*` options do not apply |
| `idle_inhibitor` | ✅ | A click keeps the display on and the computer awake (`SetThreadExecutionState`, undone on exit); `{status} {icon} {timeout} {timeleft}`, `timeout` in minutes, `start-activated`. No `wait-for-activity` |
| `keyboard-state` | ✅ | `numlock`, `capslock`, `scrolllock` labels (`#keyboard-state label.locked`), `format` (string or per key), `format-icons` (`locked`/`unlocked`, or per key) |
| `power-profiles-daemon` | ✅ | Windows' power mode (Best power efficiency, Balanced, Best performance) as `power-saver`, `balanced`, `performance`; with a power plan other than Balanced, the plans Power saver, Balanced and High performance. Left click: the next profile, right click: the previous one. `{profile} {icon} {driver}`; without `format-icons` the icon is `SAVE`, `BAL` or `PERF` (Waybar's default icons need Font Awesome) |
| Others (`backlight`, `bluetooth`, `mpd`, …) | ❌ | Skipped with a log message |
| `on-click`, `on-click-middle`, `on-click-right`, `on-scroll-up/down` | ✅ | Run through `exec`; `swaymsg <command>`, `pkill -<signal> waybar` and the volume commands `pactl set-sink-volume\|set-sink-mute\|set-source-volume\|set-source-mute`, `wpctl set-volume\|set-mute` and `pamixer -i\|-d\|--set-volume\|-t\|-m\|-u` (on the default Windows devices) run inside swinewm. A Waybar signal through `pkill`: `RTMIN+n` (custom modules), `USR1` (show or hide the bar), `USR2` (reload the bar) |
| Tooltips | ✅ | After the hover delay, below a top bar (above a bottom one). `tooltip` (`false` turns it off), `tooltip-format`, `tooltip-format-<state>` and each module's Waybar default (window: the whole title; battery: `{timeTo}`; disk: `{used} used out of {total} on {path} ({percentage_used}%)`; scratchpad: one `{app}: {title}` line per window; custom: the script's `tooltip`, lines split by `\r`). Styled by `tooltip` and `tooltip label` in style.css; markup is stripped |
| `rotate` | ✅ | 90, 180, 270 (counter-clockwise, as GTK) for text modules |
| Markup beyond stripping | ❌ | |
| `style.css` | 🟡 | Selectors by type/#id/.class with descendants; `@define-color`, `alpha()`; colours, fonts, padding, margin, borders, `box-shadow: inset`; `:hover` (colours and borders; it does not change sizes) and `:not(.class)`; no other pseudo-classes (`:first-child`, …), animations, transparency (blended over black) |

## Notifications (mako config)

| Feature | Status | Notes |
| --- | --- | --- |
| `mako/config`, global options, `[criteria]` | ✅ | Criteria: `app-name`, `summary`, `body`, `urgency`, `category`, `mode`, `expiring`. Regex (`summary~`), grouping and `hidden` criteria not yet |
| Style options | 🟡 | `font`, colours, `width`, `height`, `margin`, `padding`, `outer-margin`, `border-size`, `format`, `text-alignment`, `anchor`, `output`; `border-radius`, icons, progress not drawn |
| Behaviour options | ✅ | `default-timeout`, `ignore-timeout`, `max-visible`, `max-history`, `history`, `sort` (time), `invisible`, `on-button-*` |
| `notify-send` | ✅ | As a command; `-u -t -a -c -r`, other options accepted |
| `makoctl dismiss [-a] [-n id] [-h]`, `restore`, `invoke`, `mode [-a\|-r\|-t\|-s]`, `reload` | ✅ | `invoke` dismisses (no actions yet) |
| `makoctl list` / `history` | 🟡 | Use `swinemsg -t get_notifications` / `get_notification_history` |
| Notifications from Windows applications (toasts) | ❌ | See the project summary |

## Screenshots (grim)

There is no grim on Windows, so it is built in: `grim` is a command, and
`exec grim …` in a sway config runs it. The default config binds Print.

| Feature | Status | Notes |
| --- | --- | --- |
| `grim [file]` | ✅ | Whole desktop; default file `<date>_grim.png` in `%GRIM_DEFAULT_DIR%` or the Pictures folder |
| `-o <output>`, `-g "x,y wxh"`, `-t png\|jpeg` | ✅ | Desktop coordinates in physical pixels |
| `-c`, `-q`, `-l`, `-s` | ⚪ | Accepted; the cursor is never captured |
| `grim -` (stdout), `-T`, `slurp` | ❌ | No pipes yet; no interactive area selection |

## Launcher (wofi / wmenu)

`swinemenu.exe` is the default `$menu`. It is a separate program, as wofi
is; any other launcher can be set in the config instead.

| Feature | Status | Notes |
| --- | --- | --- |
| `--show drun` (default) | ✅ | Start menu shortcuts (all users and current user; uninstallers and document shortcuts left out) and Store apps |
| `--show run` | ✅ | Programs on PATH (`.exe`, `.bat`, `.cmd`, `.com`) |
| `--show dmenu`, `-d`/`--dmenu` | ✅ | Choices from standard input, the chosen (or typed) line on standard output, exit status 1 if cancelled |
| `-p`/`--prompt`, `-L`/`--lines`, `-W`/`--width` | ✅ | |
| Matching | ✅ | Every typed word must appear, ignoring case; often chosen entries first (history in `%LOCALAPPDATA%\swinewm\swinemenu-*.txt`) |
| Keys | ✅ | Type to filter, Up/Down, Tab, Ctrl+n/p/j/k, Page Up/Down, Enter, Esc, Ctrl+u, Ctrl+w/Ctrl+Backspace; a click starts an entry; clicking elsewhere closes it; starting it again closes it |
| Starting the choice | ✅ | Sent to swinewm as `exec` over IPC, so Store apps start as the shell too; without swinewm it opens the choice itself |
| Theme | 🟡 | Colours and font from the bar (`get_bar_config`); wofi's own `config`/`style.css` and icons are not read yet |
| `--open-with <file>` (swinewm extension) | ✅ | Replaces Windows' "Open with" dialog: the programs registered for the file's type (`SHAssocEnumHandlers`, else the type's ProgIDs, `OpenWithProgids` and `OpenWithList` in the registry), the default marked; Enter opens the file with the choice, a typed program name opens it with that |
| `--install-open-with`, `--uninstall-open-with` | ✅ | Adds (or removes) "Open with swinemenu" in every file's context menu and makes swinemenu the program Windows asks for files that have none (`HKCU\Software\Classes`, the `Unknown` type's `openas` verb) |

## Session

| sway | swinewm |
| --- | --- |
| Started by a display manager or a TTY | `swinewm --set-shell` makes it the Windows logon shell for the current user (`--unset-shell` undoes it). 🧪 `swinewm --set-autostart` starts it at logon next to Explorer instead, with Explorer's taskbar hidden; in first tests Explorer still took many key combinations, so this is experimental |
| `exit` ends the Wayland session | As the shell, `exit` logs off; next to Explorer it just quits |
| `exec` in the config for autostart | Works the same; as the shell, the Windows startup programs (Run keys, Startup folder) are started too, as Explorer would |

Known limitation as the shell (`--set-shell`, not `--set-autostart`): some
of Windows 11's own dialogs and flyouts rely on parts of Explorer that only
run when Explorer is the shell.
The "Open with" dialog (`OpenWith.exe`) then centres itself on the
screen's top-left corner and shows only its buttons, without the app list.
swinewm moves it to the middle of the screen, but it stays empty and does
not react to clicks (Esc closes it). Use `swinemenu --open-with <file>`
instead; `swinemenu --install-open-with` puts it in file managers' context
menus. Defaults per file type are still changed in Settings → Apps →
Default apps (Windows protects them from other programs). Win+S (search) and Win+W (widgets) likely have the
same cause.

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
| `GET_WORKSPACES`, `GET_OUTPUTS`, `GET_TREE`, `GET_VERSION` | ✅ Every node has sway's and i3's fields (`urgent`, `floating`, `scratchpad_state`, `orientation`, `deco_rect`, `window_rect`, `geometry`, `representation`, …), in sway's order; window and workspace events carry the same nodes. Windows-specific values: `window` is the HWND, `shell` is `win32`, `window_properties.instance` is the window class, `deco_rect` covers only the tabs of tabbed and stacked containers (Windows draws a window's own title bar inside the window), and Wayland-only fields (`sandbox_*`, `tag`, `foreign_toplevel_identifier`) are `null` |
| `GET_BINDING_MODES`, `GET_BINDING_STATE`, `GET_CONFIG` | ✅ |
| `GET_MARKS` | ✅ |
| `GET_BAR_CONFIG` | ✅ one bar, `bar-0`: position, height, and `colors`/`font` computed from the Waybar `style.css` (window, workspace buttons) |
| `GET_INPUTS`, `GET_SEATS` | ✅ Reports each successfully installed aggregate keyboard/pointer hook and one seat (`seat0`) with the corresponding capabilities. The keyboard lists the Windows keyboard layouts as sway's `xkb_layout_names`, `xkb_active_layout_index` and `xkb_active_layout_name` (English names such as `"Swedish (Sweden)"`). Physical devices are not exposed |
| `GET_NOTIFICATIONS` (1000), `GET_NOTIFICATION_HISTORY` (1001) | ✅ swinewm extensions |
| `IPC_SYNC` | ✅ Returns the unsupported response, as sway does |
| `SEND_TICK` | ✅ As in sway: the payload is any text, sent to tick subscribers as `{"first": false, "payload": …}`; a new tick subscriber first gets `{"first": true, "payload": ""}` |
| `SUBSCRIBE` and events | 🟡 Validates subscriptions (they add up, as in sway) and streams events asynchronously; workspace focus/init/empty/move/rename/urgent, window new/close/focus/title/move/fullscreen_mode, mode (`change` is the mode's name), output, binding, barconfig, shutdown, tick and input (`xkb_layout`, when the keyboard layout changes) are covered; nothing is sent after shutdown. `bar_state_update` and some state changes remain unimplemented. `swinemsg -t subscribe [-m] '["window"]'` prints events as `swaymsg` does; it also accepts plain names (`-m window workspace`) and the `[window,workspace]` that Windows PowerShell before 7.3 makes of the quoted array |
| Watchdog | ✅ A companion process probes the manager's hidden window and process. After three missed probes it terminates an unresponsive manager and runs `--recover`; an unexpected exit also runs recovery. If SwineWM is the login shell, it starts Explorer afterward |

`get_tree` reports native window properties as `app_id` (the executable
name, e.g. `notepad.exe`), `pid`, `window` (the HWND) and
`window_properties.class`/`title`, plus `marks` and `sticky`.
