# Wayland Desktop Shells on Windows — Project Summary

> **Archived (October 2026).** This note belongs to SwineWM's original
> strategy: porting Wayland, wlroots and Sway to Windows. The project now
> builds a from-scratch Windows-native clone of Sway, Waybar and Mako
> instead; see [`../project_summary.md`](../project_summary.md) and
> [`../architecture.md`](../architecture.md). The parts about the
> native-window bridge, focus restrictions and a reliable way back to Explorer
> still apply.

## Vision

Build a platform that lets Wayland compositors and desktop environments run on Windows as **true shell replacements**, not merely as Linux desktops inside a fullscreen window.

The long-term goal is to make it practical to build or port multiple Wayland desktops—such as Sway, KDE Plasma, and potentially GNOME—with minimal upstream changes. Windows remains the underlying operating system, providing its kernel, drivers, security model, and native application infrastructure.

## Two complementary deliverables

### 1. Early product: a Sway-like native Windows shell

Create a useful keyboard-driven tiling shell early, using native Windows APIs to manage ordinary Windows application windows.

Initial features:
- Tiling layouts and directional focus
- Keyboard-driven window movement
- Workspaces
- Configurable keybindings
- A basic status bar or desktop UI
- Reliable restoration of windows if the shell exits or crashes

For this first product, let Windows' Desktop Window Manager (DWM) continue composing native Windows windows. The shell manages policy—layout, focus and workspace behavior—through Win32 APIs.

### 2. Long-term platform: Wayland on Windows

Build a reusable Windows platform layer so genuine Wayland compositors and clients can run natively on Windows.

Aim to preserve upstream components where practical:
- Wayland protocols and protocol definitions
- `libwayland`, where feasible
- Portable compositor policy and configuration code
- Portable client applications and protocol implementations

Provide Windows backends or replacements for OS-specific functionality as needed:
- Display and output management
- Rendering, buffer allocation and presentation, potentially using Direct3D/DXGI
- Keyboard, pointer and other input
- Event-loop integration, timers and shared memory
- Session management and other Linux-oriented facilities required by target software

Avoid emulating all of Linux by default. Add POSIX compatibility and Linux API implementations selectively, based on real dependency requirements.

## Core architecture: one desktop, two window paths

The Wayland compositor should be the **desktop policy owner**: it decides layout, workspaces, focus and the visible desktop experience.

However, not every application needs to become a Wayland client:

- **Wayland applications:** use native Wayland surfaces and the compositor's normal rendering path.
- **Windows applications:** retain native HWNDs and DWM composition, while an integration bridge coordinates their behavior with the Wayland desktop.

DWM already composes normal Windows windows, so the project generally would not “hand windows to DWM.” Instead, it would coordinate native windows that are already managed by Windows.

This hybrid model is analogous in spirit to rootless XWayland, where X11 applications appear as individual windows in a Wayland desktop. It is not a direct implementation of XWayland: Windows does not provide an equivalent general-purpose public API for handing arbitrary HWND ownership to an independent compositor.

## The native-window bridge

Treat native Windows windows as **foreign clients** in the desktop's shared window model.

The bridge should coordinate:
- Window creation and destruction
- Position and size
- Activation and focus
- Stacking order
- Minimize, restore and close
- Visibility and workspace behavior
- Overview or task-switcher representation

Use documented Windows APIs where possible—for example, `SetWindowPos` for geometry and appropriate Win32 event mechanisms for lifecycle tracking.

Expect behavioral differences and edge cases involving owned dialogs, menus, pop-ups, fullscreen applications, elevated applications, DPI, multiple monitors, focus restrictions and apps that react badly to being hidden.

A shared window abstraction could expose operations such as:
- `map` / `unmap`
- `configure` (position and size)
- `activate` / `deactivate`
- `minimize` / `restore`
- `close`
- `set_workspace`

Wayland surfaces and Win32 HWNDs would implement these operations through different adapters. Keep compositor policy separate from platform-specific window handling.

## Sway as the proof-of-concept

Sway is a strong initial target because it exercises compositor behavior and has a clear keyboard-driven tiling model. wlroots offers reusable compositor abstractions, but Sway is coupled to wlroots and Linux-oriented dependencies; do not assume it can be ported simply by changing the graphics backend.

Use Sway to validate the platform, not as proof that every desktop will work automatically. Keep the platform modular so that other wlroots compositors—and later compositors with different architectures—can be evaluated.

## Recommended milestones

1. **Native shell prototype:** a Sway-inspired tiling manager for ordinary Windows windows.
2. **Minimal genuine Wayland compositor:** start on Windows and present an output.
3. **Wayland client proof:** connect and display a simple client; verify input and a second client.
4. **Mixed-window proof:** tile genuine Wayland clients and ordinary Windows applications together on the same desktop.
5. **Shell/session integration:** allow the desktop to be selected at login, with a reliable way back to Explorer.
6. **Broaden compatibility:** add supporting libraries and services only as needed by real target applications.
7. **Validate portability:** run another compositor or desktop component to check that the platform is not Sway-specific.

The most valuable early platform milestone is not merely “Sway compiles”; it is that a minimally modified Wayland client works on a Windows-hosted compositor, and that Wayland and Windows windows can coexist under one desktop policy.

## Design principles

1. **One policy owner:** the Wayland compositor owns desktop policy; avoid two independent authorities fighting over focus, stacking and geometry.
2. **Two presentation paths:** Wayland surfaces use the Wayland path; native HWNDs remain DWM-composed and are coordinated by the bridge.
3. **Hybrid integration:** support co-management first and use embedding only where it proves reliable. Do not assume every Windows application can be embedded.
4. **Keep upstream changes small:** prefer modular platform backends and targeted compatibility implementations over large permanent forks.
5. **Keep the early shell independent:** it should be useful even before the Wayland platform is mature.
6. **Use public, stable Windows interfaces:** avoid making core functionality depend on undocumented shell or virtual-desktop APIs.
7. **Add services on demand:** notifications, portals, D-Bus equivalents, file integration and other desktop services should follow concrete application needs.

## Key technical risk

The hardest part is not drawing the desktop; it is coordinating two different window models. Wayland compositors expect control over their clients' surfaces, while Windows applications create HWNDs managed by Windows. Geometry, focus, stacking, visibility, pop-ups, fullscreen behavior and workspace transitions must be reconciled.

DWM and the Wayland compositor should not be treated as interchangeable compositors that can freely take turns owning arbitrary windows. The practical design is for the Wayland compositor to own desktop policy while DWM continues composing native Windows windows.

## Bottom line

Build a useful native Sway-like shell first, while designing toward a genuine Wayland platform. Use Sway as a vertical proof-of-concept, and make the mixed-window bridge a first-class subsystem. The defining demonstration is a single Wayland desktop that can tile and manage both Wayland-native clients and ordinary Windows applications, then start as the user's selected shell with a reliable fallback to Explorer.
