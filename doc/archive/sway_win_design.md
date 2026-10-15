# Draft design: Sway-Win

> **Archived (October 2026).** This note belongs to SwineWM's original
> strategy: porting Wayland, wlroots and Sway to Windows. The project now
> builds a from-scratch Windows-native clone of Sway, Waybar and Mako
> instead; see [`../project_summary.md`](../project_summary.md) and
> [`../architecture.md`](../architecture.md). Its HWND bridge contract
> (asynchronous calls, request/outcome activation, events queued to the main
> thread) carried over into `swinewm/swinewm/win32/`.

This draft proposes a Windows build area at the repository root, separate from
the upstream `sway/` and `wlroots/` submodules. It aims to reuse upstream Sway
source by reference wherever its existing interfaces permit, and to put
Windows API/ABI code in `sway-win/`.

The separate `sway-hwnd/` proof of concept (since removed from the repository) now
implements the native HWND-first track described below. It is intentionally a
small, independent prototype; `sway-win/` remains a possible future build area
for the broader Wayland Sway port and upstream-source reuse work.

The central constraint is that the current Sway source is a Wayland compositor,
not a generic window manager with a replaceable window API. Much of its tree,
input, output, and rendering code directly uses wlroots and Wayland objects.
The design must account for that coupling instead of making fake wlroots
objects for HWNDs.

**Recommendation:** if maximum upstream Sway source reuse is the main
criterion, make the first serious `sway-win` target a genuine Wayland Sway
compositor running on a Windows wlroots backend, then add HWND co-management.
If the first deliverable must tile existing Windows applications as soon as
possible, build the smaller native HWND shell as a separate prototype and
accept that it can initially reuse less Sway implementation code. Do not
represent HWNDs as fabricated `wlr_surface` objects to make the source list
look larger.

## Two related targets

There are two useful meanings of “Sway-Win.” Keep them as separate targets and
milestones, even if they eventually share policy or a launcher.

### A. Native HWND shell

This target manages existing Windows applications directly. It is the quickest
way to get Sway-like tiling and workspaces on a Windows desktop, but it is not a
Wayland compositor. Native applications remain HWNDs composed by DWM.

An HWND is not a `wlr_surface`: the latter is a Wayland client surface with
buffer commits, protocol state, subsurfaces, and a client lifecycle. Sway's
`tree/view.c`, `tree/container.c`, `tree/arrange.c`, `tree/workspace.c`, and
input/seat code make direct use of those Wayland and wlroots concepts. The
native target therefore needs a window adapter and a policy integration seam;
it cannot just substitute Win32 calls at the bottom of the existing tree.

For this target, implement a `sway_win_view` backed by an HWND and keep a
separate HWND identity-to-view map. Keep the Sway-like policy tree and config
semantics distinct from the Win32 adapter. New files belong under `sway-win/`;
do not patch upstream files merely to make them include Windows headers.

### B. Wayland Sway compositor on Windows

This target runs Wayland clients and uses Sway's normal compositor model. The
reuse path is to provide a Windows backend and supporting platform interfaces
in wlroots and the lower layers, so Sway can keep using `wlr_output`,
`wlr_surface`, `wlr_seat`, `wlr_scene`, and the Wayland event loop.

This is the path with the greatest potential for compiling existing Sway
sources unchanged. It is also a larger platform project: wlroots currently
builds its Wayland server and common compositor types alongside backend
selection for DRM, libinput, and X11. A Windows backend needs to fit wlroots'
backend/renderer/output/input abstractions and remove or replace Linux-only
build requirements. See the current
[`wlroots` build definition](../../wlroots/meson.build) and
[`Sway` dependency/source definition](../../sway/sway/meson.build) as the versioned
source of truth when implementation begins.

Do not assume that this path automatically manages arbitrary Windows HWNDs.
The native HWND bridge remains a separate integration subsystem with weaker
and different guarantees than Wayland surfaces.

## Recommended repository layout

```text
sway-win/
  CMakeLists.txt                 # MSVC-facing targets and source manifests
  CMakePresets.json              # Windows developer presets when available
  include/sway-win/              # C ABI and platform-neutral adapter contracts
  src/
    app/                          # Windows target entry point / lifecycle
    win32/                        # HWND, WinEvent, monitor, input, DPI adapters
    native/                       # HWND-backed Sway-Win view implementation
    wayland/                      # later: Windows event-loop / process adapters
  cmake/                          # dependency target and feature helpers
```

The long-term source-only Wayland target can list source files from
`../sway/sway/` and `../sway/common/` directly in CMake, alongside a Windows
wlroots build. Keep explicit source manifests rather than globbing: Sway has
optional Xwayland, tray, backend, and feature-gated files. The Linux Meson
build remains upstream's build and should continue to work independently.

For the native HWND target, first build only the minimum useful application
surface: config loading, IPC or a debug console, a basic bar later, and a
small command set for focus, split/layout, move, resize, workspace, floating,
reload, and exit. Do not enable every Sway command by stubbing unsupported
Wayland features; report unsupported commands clearly.

## Upstream source reuse plan

“Reuse” should mean compiling an upstream source file from the submodule path
or carrying a small, reviewable upstream patch, rather than copying a source
file into `sway-win/` and letting it drift. Maintain a source manifest and
record any Windows-only compile definitions or patches beside it.

| Upstream area | Native HWND shell | Wayland Sway on Windows |
| --- | --- | --- |
| `common/list.c`, `common/stringop.c` | Good first candidates; check POSIX string/path helpers and provide narrow compatibility functions where needed. | Reuse directly once the dependencies build. |
| `sway/config.c`, command parser/handlers | High value, but currently coupled to Sway globals, Wayland input/output state, reload, and command implementations. Start with a constrained config subset; extract or adapt only when a real seam exists. | Reuse broadly after required Windows runtime calls and unavailable features are isolated. |
| `sway/tree/*` | Not directly reusable as a group: nodes own WLR scene/output/view objects and transactions. Reuse the algorithms after introducing an upstream-supported policy/platform boundary, or keep this first native manager small. | Reuse broadly because real WLR surfaces and outputs remain the model. |
| `sway/input/*` | Replace the seat/device layer for HWND shell input; Sway keybinding/config concepts may inform the adapter. | Reuse Sway seat logic; implement Windows keyboard/pointer devices and event integration in wlroots. |
| `sway/desktop/*`, `server.c`, `main.c` | Mostly not applicable to a native HWND-only shell. | Reuse where APIs remain available; replace startup, process, event-loop, and backend assumptions through wlroots/platform integration. |
| `swaybar`, `swaynag`, `swaymsg` | Later targets. They have their own Wayland, renderer, and process assumptions. `swaymsg`/IPC semantics may be useful earlier. | Separate later Windows build targets once clients and renderer support exist. |

The first source-reuse spike should be a tiny MSVC target that compiles the
selected upstream common files from their existing paths. This will establish
the actual POSIX compatibility needs before a larger source list is promised.
Do not copy/fork `tree/arrange.c` blindly: although its layout algorithm is
valuable, it directly works with Sway container state and wlroots geometry
types.

## HWND bridge contract

Expose the Win32 bridge through an opaque C ABI to the Sway C code, even if its
implementation uses C++. Keep Windows headers out of shared policy headers.
The bridge should deliver stable IDs, metadata, events, and requested
operations; it should not decide tiling or workspace policy.

Initial Win32 responsibilities:

- Enumerate and observe top-level application windows using documented Win32
  APIs and WinEvent hooks.
- Track HWND lifetime, owner/owned relationships, visibility, title, process,
  style, monitor, DPI, and current geometry. Treat transient dialogs and popups
  as related windows rather than ordinary independent clients.
- Apply batches of window placement changes and report whether requests were
  accepted. Use `BeginDeferWindowPos`/`DeferWindowPos`/`EndDeferWindowPos` for
  coordinated geometry changes where practical.
- Request activation while respecting Windows foreground restrictions. Do
  not assume `SetForegroundWindow` always succeeds; return the actual outcome.
- Provide monitor bounds and DPI conversion so Sway's logical layout geometry
  maps deliberately to physical HWND coordinates.
- Offer a small, documented input-binding mechanism for the first prototype.
  Keep it separate from Sway's Wayland seat/device implementation.

All WinEvent callbacks should enqueue events for the shell's main thread.
Update the policy tree after callback return, not reentrantly from inside the
Win32 hook. Keep HWND calls on a controlled UI thread and make the bridge's
message-pump integration explicit.

Windows can deny foreground activation and impose owned-window/Z-order rules.
The shared policy API should distinguish a request from its result instead of
promising Wayland-equivalent focus or stacking. Consult Microsoft's
[`SetForegroundWindow`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)
and [`SetWindowPos`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
contracts during implementation.

## Configuration and IPC compatibility

Treat Sway config compatibility as incremental. A command can be reused only
when its behavior can be supplied by the native model. Start with a documented
subset of familiar commands, e.g. `bindsym`, `focus`, `split`, `layout`,
`move`, `resize`, `workspace`, `floating`, `exec`, and `exit`, and reject
unsupported commands with a line-specific diagnostic. Input bindings will
need a Windows key-name/keycode mapping and may not have the exact semantics
of a Wayland seat.

Keeping `swaymsg` compatibility is valuable for scripts, but the current IPC
implementation uses Unix-domain sockets. Define a transport seam so Windows
can use a named pipe or loopback socket while preserving the message framing
and command/event JSON where possible. Avoid claiming full i3/Sway IPC
compatibility until event ordering and error behavior are matched.

## Build and ABI policy

Use CMake for the MSVC-facing `sway-win` targets, with explicit imported
dependency targets and feature flags. Keep language boundaries deliberate:
the current upstream Sway core is C, so expose new C++ platform code with
`extern "C"` interfaces and opaque handles. Do not pass STL objects, Windows
types, or CRT-owned allocations across that boundary.

Decide static versus DLL linkage per dependency after the first Windows build;
it is independent of whether dependency source is a submodule. Ensure every
linked MSVC object uses the same runtime-library setting. Build architecture,
configuration, CRT choice, Unicode mode, and feature set should be recorded in
the Windows presets and dependency manifest.

## Milestones and decision gates

1. **Build inventory:** create an MSVC source/dependency inventory and compile
   the small, genuinely portable upstream common files from their submodule
   paths.
2. **HWND vertical slice:** observe, tile, focus-request, restore, and close a
   few ordinary top-level HWNDs; exercise DPI, dialogs, and monitor changes.
3. **Native Sway-like policy:** load a constrained Sway-style config and support
   core tiling/workspace/focus operations with honest error reporting.
4. **Wayland platform slice:** independently present a Wayland output, connect a
   simple client, deliver input, and handle resize/shutdown on Windows.
5. **Reuse Sway compositor:** build increasing sets of upstream Sway files
   against the Windows wlroots backend; track each source that needs a patch
   and why.
6. **Mixed-window proof:** coordinate Wayland toplevels and native HWNDs under
   one policy, and validate the stacking limitations documented above.
7. **Shell deployment:** select a supported login/shell mechanism for the
   target Windows edition and provide a tested path back to Explorer.

The critical decision gate is the end of milestone 2/4: decide whether the
first useful product should remain the independent HWND shell or whether the
project should prioritize the genuine Wayland compositor. The latter has a
better upstream source-reuse story; the former gets native Windows apps under
tiling policy sooner.
