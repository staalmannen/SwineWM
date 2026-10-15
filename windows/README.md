# Native Windows shell bridge

This is the first native-shell building block described in
[`doc/archive/wayland_port_summary.md`](../doc/archive/wayland_port_summary.md),
and the library behind the [`sway-hwnd/`](../sway-hwnd/) prototype. New
development happens in [`swinewm/`](../swinewm/).
It is an independent C++20/MSVC library in a root-level sibling directory; no
upstream submodule sources are modified.

`swine::windows::WindowBridge` enumerates ordinary top-level HWNDs, reports
WinEvent lifecycle/geometry/focus notifications, and exposes basic window
operations. The caller remains responsible for tiling, focus policy, key
bindings, and the shell UI. Workspace switching currently groups HWNDs by
visibility, while DWM retains composition of native applications.

`swine::windows::TileLayout` is a platform-independent binary split-tree
policy that calculates window placements and directional neighbors. It is
compiled into the same library, but the demo does not yet connect its
placements to HWND geometry or provide directional-focus keybindings.

Build from a Visual Studio Developer PowerShell:

```powershell
cmake -S windows -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Debug
./build/windows/Debug/swine_window_bridge_demo.exe
```

The demo prints its initial HWND/title list and logs events. It assigns the
windows present at startup to workspace 1; use Ctrl+Alt+1 through Ctrl+Alt+9
to switch their visibility group, and Ctrl+C to exit. Run a few ordinary apps
first so the initial list is useful. Switching away hides the grouped windows;
switching back shows them without activation.

For an embedding application, the WinEvent hook is delivered on the thread
that installed it, so construct the bridge and pump that thread's messages
(with `dispatch_one()` or the application's own message loop). Foreground
activation is subject to Windows focus-stealing rules. This initial bridge
does not attempt to embed/reparent windows or promise reliable behavior for
elevated, fullscreen, owned-dialog, or tool windows. It is still only a library
and smoke-test demo, not a Sway port or complete shell.
