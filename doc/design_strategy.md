# Design strategy additions

This document adds recommendations to the existing native-clone strategy in
[`project_summary.md`](project_summary.md). They are intended to guide the
next design decisions; they do not expand the compatibility promise by
themselves. The live feature contract remains
[`compatibility.md`](compatibility.md).

## What is already working well

- The project chose Windows-native HWND management and DWM composition
  instead of trying to make Windows applications behave like Wayland clients.
- The portable core and Win32 platform layer have a useful boundary. Tree,
  focus, command and configuration behaviour can be reasoned about separately
  from Windows calls.
- Tree mutations are kept on the main thread, and callbacks queue events
  instead of changing layout re-entrantly.
- Shell-specific operations have explicit recovery paths, including showing
  hidden windows after a crash and restoring selected display and taskbar
  changes.
- The compatibility table makes partial support visible. Keep it as the
  source of truth instead of implying full Sway, Waybar or Mako parity.

## Recommended priorities

### 1. Make shell recovery a product feature

When SwineWM is the login shell, correctness includes getting the user back to
a usable Windows session after a failed start, crash, bad configuration or
upgrade. Preserve the current rescue paths and make them easy to discover:

- Keep a documented route back to Explorer that does not depend on SwineWM's
  normal UI or IPC being healthy.
- For each operation that changes persistent or session-wide Windows state,
  record what must be restored before applying the change. Recovery should be
  safe to run repeatedly and should tolerate windows or monitors that have
  since disappeared.
- Treat startup, normal shutdown, crash recovery, restart and uninstall as one
  lifecycle. Each path should either restore owned state or leave enough
  information for the next process to do so.
- Prefer fail-open behaviour: if a Windows integration cannot be initialized,
  keep ordinary desktop use possible and explain the degraded feature in the
  log or notification.

The existing recovery code covers important cases, but the design should
continue to account for every new shell integration rather than relying on
normal shutdown alone.

### 2. Test Windows behaviour at the boundary

The fake-platform suite is a good fit for tree, focus, command and config
logic. It cannot establish that Win32 shell behaviour matches expectations.
Add a small, repeatable Windows smoke checklist or harness for the platform
boundary, and run it on real Windows versions or disposable VMs before calling
shell-facing work complete. Prioritize:

- starting beside Explorer and as the login shell, then clean exit and
  recovery;
- window discovery, dialogs, focus, minimize/restore, fullscreen and
  application-owned popups;
- one and multiple monitors, mixed DPI, monitor removal, sleep/resume and
  display-setting restoration;
- tray ownership, taskbar/work-area behaviour and maximized unmanaged
  windows;
- elevated applications, remote sessions and Windows feature updates, with
  limitations documented where UIPI or Windows policy prevents control.

Keep scenarios short and record the Windows version, shell mode, display
layout and SwineWM build in bug reports. Use fake-platform tests for core
invariants and Windows smoke coverage for OS integration; neither replaces
the other.

### 3. State the compatibility rule for each feature

Use three explicit outcomes when adapting upstream behaviour:

1. **Compatible:** preserve Sway/Waybar/Mako semantics where the concept
   exists on Windows.
2. **Translated:** use the closest native Windows mechanism and document the
   observable difference.
3. **Unsupported:** reject unsafe or misleading behaviour, or accept a known
   no-op with a useful diagnostic when that is needed to load an existing
   config.

Do not use silent no-ops for features users are likely to depend on. Keep
Windows-only IPC and commands clearly identified as SwineWM extensions, and
avoid claiming support beyond the cases described in
[`compatibility.md`](compatibility.md). This protects the project's main
promise—familiar configs and habits—without forcing a false one-to-one mapping
onto Windows.

### 4. Make failures diagnosable without telemetry

For a shell, local logs and a simple support bundle are more useful than
remote telemetry. Continue logging lifecycle and recovery actions, and make
the important context easy to collect: build version, shell mode, active
outputs, config path and validation errors. Redact user-specific paths and
window titles from any shareable bundle by default. Do not add telemetry
unless there is a separate, explicit privacy design and opt-in.

### 5. Keep the single-process design, with service boundaries

The single executable keeps startup and recovery cohesive and avoids
recreating Linux's D-Bus arrangement. Preserve that advantage while keeping
the window manager, bar, notifications and tray behind internal interfaces.
Define what happens when an optional service fails to load or render, and
avoid letting optional UI work block window management. Split a service into
another process only if measured reliability, performance or independent
restart needs justify the added IPC and lifecycle complexity.

## A release gate for shell-facing changes

Before a shell integration is considered ready for a release, check that:

- its normal, shutdown, crash and recovery paths are described;
- failure leaves the user with a usable way to recover;
- core behaviour has a portable test where possible, and the Windows-facing
  behaviour has a reproducible smoke scenario;
- compatibility status and user-visible changes are updated in
  `compatibility.md` and `CHANGELOG.md`;
- any use of undocumented Windows APIs is isolated, optional where practical,
  and has a documented fallback.

This gate is deliberately narrower than requiring every feature to work on
every Windows configuration. Known limitations can ship when they are
visible, bounded and recoverable.
