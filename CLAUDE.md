# CLAUDE.md

Guidance for Claude (and other contributors) working in this repository.

## What this project is

SwineWM is a **from-scratch Windows-native clone** of Sway (tiling WM),
Waybar (bar) and Mako (notifications). It is *not* a port: there is no
Wayland, no wlroots, no compositor. Windows application windows (HWNDs) are
managed through documented Win32 APIs; DWM keeps compositing them.

All three live in **one program, `swinewm.exe`** (an explicit decision: no
one-tool-per-task split, no D-Bus). Each part still reads the config format
of the program it replaces: sway's config, Waybar's `config.jsonc` and
`style.css`, mako's config.

Sway, Waybar and Mako are the specification. Reproduce their behaviour,
config syntax, IPC protocol and defaults as faithfully as Windows allows.
This is not clean-room work: read the submodules freely and port code when
that is the most faithful option. They are MIT licensed; keep the upstream
copyright notice on substantially ported code.

Read `doc/project_summary.md` (scope, roadmap), `doc/architecture.md` (how
swinewm works), `doc/compatibility.md` (what is implemented) and
`doc/packaging.md` (installer) before larger changes, and keep `doc/compatibility.md` current when a command's
status changes.

## Repository map

- `swinewm/` — the program (`swinewm/swinewm/`), `swinemsg`, shared
  `common/`, headers in `include/`, unit tests in `tests/`. Mirrors the sway
  repository layout: `swinewm/swinewm/tree/container.c` corresponds to
  `sway/sway/tree/container.c`, and so on.
  - `swinewm/swinewm/bar/` — the built-in Waybar (config, modules, CSS
    subset, format strings); drawn by `win32/bar.c`.
  - `swinewm/swinewm/notify/` — the built-in Mako (store, config, modes);
    drawn by `win32/notify.c`. `notify-send`/`makoctl` are commands in
    `commands/notify.c`.
  - `swinewm/resources/` — installed defaults (Waybar config/style, mako
    config, wallpaper, icon). The bar's built-in fallback is generated from
    these files at build time (`cmake/embed.cmake`); edit the files, not C
    strings. Keep `swinewm/config.in` and these files sensible for a fresh
    install.
  - `swinewm/cmake/packaging.cmake` — install layout and the CPack/WiX MSI;
    see `doc/packaging.md`. Never change `CPACK_WIX_UPGRADE_GUID`.
- `.github/workflows/build.yml` — CI: Linux tests, MSVC build and tests,
  MSI/zip artifacts. MSVC problems show up here first.
- `sway/`, `waybar/`, `mako/` — upstream reference submodules. **Never edit
  files inside submodules.** Fetch them with
  `git submodule update --init --depth 1 sway waybar mako`.
- `doc/archive/` — notes from the abandoned port strategy.

## Where to look in the reference sources

| Topic | sway | waybar | mako |
| --- | --- | --- | --- |
| Command behaviour | `sway/commands/<cmd>.c`, `sway/sway.5.scd` | | |
| Layout tree | `sway/tree/*.c`, `sway/tree/arrange.c` | | |
| Focus | `sway/input/seat.c` | | |
| Config parsing | `sway/config.c`, `sway/commands.c` | `src/config.cpp` | `config.c`, `criteria.c` |
| IPC | `sway/ipc-server.c`, `sway/ipc-json.c`, `sway/sway-ipc.7.scd` | `src/modules/sway/` | |
| Defaults | `config.in` | `resources/config.jsonc`, `resources/style.css` | `doc/mako.5.scd` |
| Modules / options | | `src/modules/*`, `man/waybar-*.5.scd` | `doc/mako.5.scd`, `doc/makoctl.1.scd` |

## Code conventions

- C11, built with CMake. Must compile cleanly with MSVC (`/W4`), MinGW-w64
  GCC and Linux GCC/Clang (`-Wall -Wextra`).
- Style follows sway: tabs, `snake_case`, `struct swine_*` where sway has
  `struct sway_*`, the same function names as sway where the function does
  the same thing, `list_t` for lists, `swine_log(SWINE_DEBUG, ...)` for
  logging. Avoid GNU extensions (no `##__VA_ARGS__`, no VLAs, no
  `strcasecmp`/`strdup` — use `swine_strcasecmp`/`swine_strdup`).
- The core (`common/`, everything in `swinewm/swinewm/` except `win32/`)
  must not include `<windows.h>`. OS access goes through
  `include/swinewm/platform.h`; the Win32 implementation is in
  `swinewm/swinewm/win32/`, and `tests/fake_platform.c` implements it for
  tests.
- Tree changes happen on the main thread only. Hooks and IPC threads post
  or send messages to the shell window.
- Never block on another process's window: no `SendMessage` to foreign
  windows; use `ShowWindowAsync`, `SWP_ASYNCWINDOWPOS`, `PostMessage`.
- Anything that hides windows must be undoable: `platform_finish`, the
  crash handler and `swinewm --recover` must keep showing them again.
- Text is UTF-8 inside the program; convert at the Win32 boundary
  (`utf8_to_utf16` / `utf16_to_utf8`), open files with `swine_fopen`.

## Building and testing

Core and unit tests (Linux or Windows):

```sh
cmake -S swinewm -B build/linux -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build/linux && ./build/linux/swinewm_tests
```

Add a test in `swinewm/tests/` for every behaviour change in the core; the
fake platform makes layout, focus and command behaviour checkable without
Windows. Compare expected results with what sway does.

Cross-compiling the Windows executables on Linux (needs
`gcc-mingw-w64-x86-64`):

```sh
cat > /tmp/mingw.cmake <<'TC'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
TC
cmake -S swinewm -B build/mingw -DCMAKE_TOOLCHAIN_FILE=/tmp/mingw.cmake
cmake --build build/mingw
```

With `wine64` and `xvfb` installed, `swinewm_tests.exe` runs under Wine, and
`swinewm.exe` can be smoke-tested: start `wine notepad.exe`, start
`swinewm.exe -V -c Z:/path/to/swinewm/config.in`, then query it with
`wine swinemsg.exe -t get_tree`. Wine is not Windows; real testing happens
on the owner's Windows machine.
