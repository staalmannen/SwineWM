# sway-hwnd

Minimal native Windows shell proof of concept. It applies the project's
platform-independent split-tree layout to ordinary visible top-level HWNDs,
then uses the Win32 bridge to request geometry, visibility, and activation.
It borrows Sway's interaction ideas but does not yet compile Sway source files:
Sway's tree and command code currently depend directly on Wayland/wlroots
objects, while this program manages HWNDs.

## Build and run

From a Visual Studio Developer PowerShell:

```powershell
cmake -S sway-hwnd -B build/sway-hwnd -G "Visual Studio 17 2022" -A x64
cmake --build build/sway-hwnd --config Debug
.\build\sway-hwnd\Debug\sway-hwnd.exe
```

Start a few ordinary applications first. The program tiles visible top-level
windows on the primary monitor's work area. Its global shortcuts are:

| Shortcut | Action |
| --- | --- |
| Ctrl+Alt+1…9 | Switch workspace |
| Ctrl+Alt+Shift+1…9 | Move the focused window to a workspace |
| Ctrl+Alt+Arrow | Focus the neighboring tiled window |
| Ctrl+Alt+R | Rescan visible top-level windows |
| Ctrl+Alt+Q | Exit |

Unavailable shortcuts are reported at startup. Exit with Ctrl+Alt+Q to return
to the desktop. This is not a login shell and does not modify Windows shell
configuration.

## Current boundaries

- Workspace changes are visibility groups built with `ShowWindowAsync`; they
  are not Windows virtual desktops.
- Only visible ordinary top-level windows are discovered. Tool windows and
  owned windows are filtered by the shared bridge, so dialogs/popups are not
  fully handled yet.
- New windows opened after startup are picked up when Ctrl+Alt+R rescans; full
  automatic lifecycle tracking is not implemented in this prototype.
- The demo uses the primary monitor work area and fixed equal splits. It does
  not handle monitor hot-plug, per-monitor layouts, borders, floating rules,
  app-specific minimum sizes, or activation denial recovery.
- Global hotkeys are fixed for this proof of concept, not configurable Sway
  `bindsym` input handling.
- Windows focus-stealing rules can prevent activation even when a layout
  change succeeds.

Use the generic [`windows/README.md`](../windows/README.md) for details on the
Win32 bridge and tile-layout library.
