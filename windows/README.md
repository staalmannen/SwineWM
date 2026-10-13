# Native Windows shell bridge

This is the first native-shell building block described in
[`doc/wayland_windows_shell_project_summary.md`](../doc/wayland_windows_shell_project_summary.md).
It is an independent C++20/MSVC library in a root-level sibling directory; no
upstream submodule sources are modified.

`swine::windows::WindowBridge` enumerates ordinary top-level HWNDs, reports
WinEvent lifecycle/geometry/focus notifications, and exposes basic window
operations. The caller remains responsible for tiling, focus policy, key
bindings, and the shell UI. Workspace switching currently groups HWNDs by
visibility, while DWM retains composition of native applications.

Build from a Visual Studio Developer PowerShell:

```powershell
cmake -S windows -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Debug
```

The WinEvent hook is delivered on the thread that installed it, so construct
the bridge and call `dispatch_one()` on the shell's UI thread. Foreground
activation is subject to Windows focus-stealing rules. This initial bridge
does not attempt to embed/reparent windows or promise reliable behavior for
elevated, fullscreen, owned-dialog, or tool windows.
