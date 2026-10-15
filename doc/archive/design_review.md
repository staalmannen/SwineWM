# Design review: Windows shell and Wayland platform

> **Archived (October 2026).** This note belongs to SwineWM's original
> strategy: porting Wayland, wlroots and Sway to Windows. The project now
> builds a from-scratch Windows-native clone of Sway, Waybar and Mako
> instead; see [`../project_summary.md`](../project_summary.md) and
> [`../architecture.md`](../architecture.md). The sections on activation
> outcomes and on keeping shell activation separate from shell policy still
> apply and are summarised in the architecture document.

This note records additional design considerations for the goals in
[`wayland_port_summary.md`](wayland_port_summary.md).
It does not change the project's aims. It makes a few unresolved platform
choices explicit so they can be tested before a broad port is attempted.

## The main architectural question: how do HWNDs and Wayland surfaces stack?

The hybrid model gives Windows applications native HWNDs composed by DWM and
gives Wayland clients compositor-managed surfaces. It still needs to decide how
the two presentation paths can overlap and stack relative to each other.

DirectComposition visual trees are bound to a window owned by the process that
creates the composition target. This is useful for a compositor's own windows,
but is not a mechanism for attaching its visual tree to an arbitrary
application's HWND. See Microsoft's
[`CreateTargetForHwnd` documentation](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondesktopdevice-createtargetforhwnd).

Two presentation shapes are worth comparing in a small prototype:

1. **One compositor window per monitor.** The compositor renders the Wayland
   scene into a native output window. This is closer to a conventional
   compositor, but native HWNDs and the compositor scene may have limited
   relative stacking.
2. **A host HWND for each Wayland toplevel.** DWM can stack those host windows
   alongside native application windows. This may support more rootless-style
   interleaving, at the cost of making the Windows window manager part of the
   presentation path for each Wayland toplevel.

These are hypotheses to validate, not settled implementation choices. Test
whether a Wayland toplevel can appear above, below, and beside a native window,
including owned dialogs, popups, fullscreen windows, and multiple monitors.
Check input routing and resizing along with visual stacking. If arbitrary
interleaving is unavailable or unreliable, document a narrower mixed-window
contract instead of promising full stacking equivalence.

## Model operations as requests with backend-specific outcomes

Wayland surfaces and HWNDs can share desktop policy, but their operations do
not have identical guarantees. For example, Windows restricts which process
can set the foreground window, and a request can be denied. See
[`SetForegroundWindow`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow).

Keep a shared window identity and policy model, while letting each backend
report operation outcomes such as completed, denied, or no longer applicable.
Activation, visibility, geometry, minimize/restore, close, and stacking should
not silently imply stronger guarantees for HWNDs than Windows provides.
Owned dialogs and transient windows may also need to follow their owner rather
than behave like independent tiled clients.

## Keep shell policy separate from shell activation

The shell application and the mechanism Windows uses to launch it are separate
concerns. Microsoft documents Shell Launcher for Windows 10/11 Enterprise,
Education, and IoT Enterprise editions, primarily for managed and fixed-purpose
devices. It is not a universal shell-selection mechanism for every personal
Windows edition. See the
[`Shell Launcher overview`](https://learn.microsoft.com/en-us/windows/configuration/shell-launcher/).

Keep the shell runnable as an ordinary user application. Treat login-time
replacement, restart policy, and recovery to Explorer as a separate deployment
milestone, and check the target machine's Windows edition before depending on
Shell Launcher. The alternative-shell list on Wikipedia can provide UI
inspiration, but it is not evidence for the feasibility of the Wayland/HWND
presentation model; the article itself notes that it needs additional
citations: [List of alternative shells for Windows](https://en.wikipedia.org/wiki/List_of_alternative_shells_for_Windows).

## Keep the early shell independent

The native Sway-inspired shell and the Wayland platform can share policy ideas
such as tiling, workspace assignment, and focus intent without sharing a
runtime dependency. Keep the early shell useful on its own while compositor,
graphics, input, and compatibility work proceeds. Prefer small platform
interfaces and new Windows sources in the project tree; carry upstream patches
only where an upstream abstraction cannot support the required backend.

## Prove a vertical slice before broad compatibility work

Before attempting a full Sway port, aim for a minimal end-to-end Wayland path:

1. Start a native output window and present a cleared frame.
2. Connect a small Wayland client and display one surface.
3. Deliver keyboard and pointer input and handle resize/configure cycles.
4. Shut down cleanly and restart without losing access to the normal Windows
   desktop.
5. Test the mixed-window stacking cases above with one native application.

This sequence tests the difficult seams early: output presentation, Wayland
transport and event integration, input, window lifecycle, and the boundary
between DWM composition and compositor-managed content. Expand POSIX and Linux
API compatibility only when this slice or a concrete target application
demonstrates the need.
