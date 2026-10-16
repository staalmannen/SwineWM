# swinewm

A Sway-compatible tiling window manager for Windows, written from scratch in
C11, with a Waybar-compatible bar and Mako-compatible notifications built
in. It manages ordinary Windows application windows, reads Sway's, Waybar's
and mako's config formats and speaks Sway's IPC protocol. See
[`../doc/project_summary.md`](../doc/project_summary.md) for the scope,
[`../doc/architecture.md`](../doc/architecture.md) for how it works and
[`../doc/compatibility.md`](../doc/compatibility.md) for what is supported.

**Status: early draft.** The core (tree, commands, config) is unit-tested
and the Windows build runs, but it has not yet been used on real hardware.
Expect rough edges and keep a way out: `Win+Shift+e` (exit), Ctrl+C in its
console window, or `swinewm --recover` if windows were left hidden.

## Installing

Every push to GitHub builds an MSI installer and a portable zip (Actions
tab → a `build` run → Artifacts). The installer puts everything in
`C:\Program Files\SwineWM`, adds a Start menu entry and puts `swinemsg` on
the PATH. See [`../doc/packaging.md`](../doc/packaging.md), which also
explains building the MSI yourself.

## Building on Windows

With Visual Studio 2022 (Developer PowerShell):

```powershell
cmake -S swinewm -B build/swinewm -G "Visual Studio 17 2022" -A x64
cmake --build build/swinewm --config Debug
ctest --test-dir build/swinewm -C Debug
```

Or open the repository folder in Visual Studio ("Open a local folder"); it
picks up `swinewm/CMakeLists.txt` by itself. The executables then end up in
`swinewm\out\build\x64-Debug\` (in Solution Explorer, switch to "CMake
Targets View", right-click `swinewm` → "Open Folder in File Explorer").
Debugging from Visual Studio is easiest: set a breakpoint and press F5 with
`swinewm.exe` selected as the startup item.

With MSYS2/MinGW-w64:

```sh
cmake -S swinewm -B build/swinewm -G Ninja
cmake --build build/swinewm
```

## Running

Copy [`config.in`](config.in) to `%USERPROFILE%\.swinewm\config` and edit it
(or rely on the copy placed next to the executable). The default config
starts [Alacritty](https://alacritty.org) and
[Flow Launcher](https://www.flowlauncher.com); install them or change `$term`
and `$menu`. Start a few applications, then run from a terminal:

```powershell
.\build\swinewm\Debug\swinewm.exe -V
```

`-V` logs what it does; `-d` logs everything. Existing windows are tiled on
workspace 1.

To use it instead of Explorer, run `swinewm --set-shell` once and log off
and on again (`swinewm --unset-shell` brings Explorer back). As the shell it
starts your Windows startup programs, logs to
`%LOCALAPPDATA%\swinewm\swinewm.log`, and `exit` (Win+Shift+e) logs you off.
Tray icons appear in the bar's `tray` module (on by default), in either
mode. Running next to Explorer, `explorer_taskbar autohide` in the config
hides Explorer's taskbar while swinewm runs.

With the default config:

| Keys | Action |
| --- | --- |
| Win+Enter | Terminal ([Alacritty](https://alacritty.org)) |
| Win+d | Launcher ([Flow Launcher](https://www.flowlauncher.com)) |
| Win+h/j/k/l, Win+arrows | Focus (Win+l locks Windows; use the arrows or change `$mod`) |
| Win+Shift+h/j/k/l, Win+Shift+arrows | Move window |
| Win+1…0 | Switch workspace |
| Win+Shift+1…0 | Move window to workspace |
| Win+b / Win+v | Split horizontally / vertically |
| Win+s / Win+w / Win+e | Stacking / tabbed / toggle split |
| Win+f | Fullscreen |
| Win+Shift+space | Toggle floating |
| Win+r | Resize mode (arrows, Enter/Escape to leave) |
| Win+Shift+q | Close window |
| Win+Shift+c | Reload config |
| Win+Shift+e | Exit (shows all windows again) |
| Win+n / Win+Shift+n | Dismiss / restore a notification |

Control it from scripts with `swinemsg`, as with `swaymsg`:

```powershell
.\build\swinewm\Debug\swinemsg.exe workspace 3
.\build\swinewm\Debug\swinemsg.exe -t get_tree
.\build\swinewm\Debug\swinemsg.exe notify-send -u critical "Build done" "All tests passed"
```

## Config files

All are looked up in `%USERPROFILE%\.swinewm\`, then
`%XDG_CONFIG_HOME%\swinewm\`, `%APPDATA%\swinewm\` and the directory of
`swinewm.exe`. Existing files from Linux can be copied over.

| File | Format | Without it |
| --- | --- | --- |
| `config` | sway | No bindings at all, so keep the copy next to the exe or your own |
| `waybar\config.jsonc` (or `waybar\config`) | Waybar | Built-in default bar |
| `waybar\style.css` | Waybar's GTK CSS (subset) | Built-in default style |
| `mako\config` | mako | mako's defaults |

The default config also sets the wallpaper with sway's
`output * bg <file> fill`; `%SWINEWM_DATA%` refers to the directory of
`swinewm.exe`.

The bar appears when the sway config has a `bar { }` block (the default
config does) or `exec waybar`. `reload` re-reads all of these.

## Developing on Linux

The core and its tests build anywhere:

```sh
cmake -S swinewm -B build/linux -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build/linux && ./build/linux/swinewm_tests
```

The Windows executables cross-compile with MinGW-w64 (`gcc-mingw-w64-x86-64`)
using a toolchain file that sets `CMAKE_SYSTEM_NAME=Windows` and
`CMAKE_C_COMPILER=x86_64-w64-mingw32-gcc`. They also start under Wine (with
Xvfb when headless), which is enough for a smoke test of window discovery,
tiling and IPC.
