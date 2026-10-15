# swinewm

A Sway-compatible tiling window manager for Windows, written from scratch in
C11. It manages ordinary Windows application windows, reads Sway's config
format and speaks Sway's IPC protocol. See
[`../doc/project_summary.md`](../doc/project_summary.md) for the scope,
[`../doc/architecture.md`](../doc/architecture.md) for how it works and
[`../doc/compatibility.md`](../doc/compatibility.md) for what is supported.

**Status: early draft.** The core (tree, commands, config) is unit-tested
and the Windows build runs, but it has not yet been used on real hardware.
Expect rough edges and keep a way out: `Win+Shift+e` (exit), Ctrl+C in its
console window, or `swinewm --recover` if windows were left hidden.

## Building on Windows

With Visual Studio 2022 (Developer PowerShell):

```powershell
cmake -S swinewm -B build/swinewm -G "Visual Studio 17 2022" -A x64
cmake --build build/swinewm --config Debug
ctest --test-dir build/swinewm -C Debug
```

With MSYS2/MinGW-w64:

```sh
cmake -S swinewm -B build/swinewm -G Ninja
cmake --build build/swinewm
```

## Running

Copy [`config.in`](config.in) to `%USERPROFILE%\.swinewm\config` and edit it
(or rely on the copy placed next to the executable), start a few
applications, then run from a terminal:

```powershell
.\build\swinewm\Debug\swinewm.exe -V
```

`-V` logs what it does; `-d` logs everything. Existing windows are tiled on
workspace 1. With the default config:

| Keys | Action |
| --- | --- |
| Win+Enter | Terminal (`wt.exe`) |
| Win+d | Applications folder (until there is a launcher) |
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

Control it from scripts with `swinemsg`, as with `swaymsg`:

```powershell
.\build\swinewm\Debug\swinemsg.exe workspace 3
.\build\swinewm\Debug\swinemsg.exe -t get_tree
```

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
