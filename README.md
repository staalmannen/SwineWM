# SwineWM

SwineWM explores a native Windows desktop shell inspired by keyboard-driven
Wayland compositors, with a longer-term goal of running Wayland applications
and compositors on Windows. The project aims to keep upstream sources clean:
Windows-specific glue and experiments live in root-level project directories
alongside the upstream submodules.

## Aims

The work has two related goals:

1. **Build a useful native shell early.** Manage ordinary Windows application
   windows with Win32 APIs and DWM, with Sway-inspired tiling, focus, movement,
   workspaces, keybindings, and a basic desktop UI. Restore the normal Windows
   shell reliably if the experimental shell exits or fails.
2. **Develop a Windows platform for Wayland software.** Add the output,
   rendering, input, event-loop, and compatibility layers needed to run a
   genuine Wayland compositor and clients on Windows. Use Sway and wlroots to
   explore this path, while keeping platform-specific changes isolated from
   upstream code where practical.

The early native shell is useful independently of the Wayland port. It is also
a place to explore window policy and user interaction before compositor
support is mature.

## Design

The target desktop has one owner for policy: the shell or compositor decides
layout, focus, workspaces, and the overall desktop experience. Applications
can reach that desktop through two paths:

- **Wayland clients** use compositor-owned Wayland surfaces and the compositor
  rendering path.
- **Windows applications** keep their native HWNDs and continue to be composed
  by DWM. A bridge coordinates their geometry, activation, visibility, and
  lifecycle with the shared desktop policy.

This is co-management, not a general mechanism for transferring HWND ownership
to a Wayland compositor. Prefer documented Windows APIs, preserve the native
application window model, and add POSIX or Linux API compatibility only when
real dependencies require it. Notification services, D-Bus integration, and
other desktop facilities should be introduced as concrete application needs
emerge.

## Current status

The repository is an early experiment, not a usable shell or a Windows port of
Sway. The `windows/` directory contains a small MSVC-oriented HWND bridge and
a demo that lists windows, logs WinEvent notifications, and switches the
visibility group of windows found at startup. It does not yet implement
tiling, shell UI, a complete input/keybinding system, notification handling,
or reliable shell recovery. The demo is a smoke-test starting point, not a
production window manager.

The root-level `sway/`, `wlroots/`, `wayland/`, `wayland-protocols/`, `mako/`,
`waybar/`, and `dbus/` directories are upstream source checkouts. Their
presence does not mean they currently build or run on Windows as part of this
project. Additional third-party libraries may be supplied by existing Windows
ports and integrated through the Windows build as needed.

## Trying the HWND bridge

On Windows, open a Visual Studio Developer PowerShell and run:

```powershell
cmake -S windows -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Debug
.\build\windows\Debug\swine_window_bridge_demo.exe
```

Start a few ordinary applications before launching the demo. It assigns the
visible windows it finds to workspace 1. Use **Ctrl+Alt+1** through
**Ctrl+Alt+9** to switch visibility groups, and **Ctrl+C** to exit. See
[`windows/README.md`](windows/README.md) for implementation notes and current
limitations.

## Development direction

The intended progression is to make the native shell useful, bring up a minimal
Wayland compositor and client on Windows, then demonstrate a shared desktop
where Wayland clients and native Windows applications coexist. Only after
those foundations work should the project broaden compatibility and shell
session integration. The detailed rationale and milestones are in
[`doc/wayland_windows_shell_project_summary.md`](doc/wayland_windows_shell_project_summary.md).
