# macOS Platform Runtime Plan

Summary: Implement and qualify the Apple Silicon macOS process, crash, shell, window, input, ownership, and Editor lifecycle required before MoltenVK rendering qualification.

Last reviewed: 2026-08-18

Status: Completed
Completed: 2026-08-18

## Current Status

Stages 0-2 are implemented. Launch owns a real macOS signal/terminate adapter
with reporter-thread publication and system-managed `.ips` semantics. Process
execution, return codes, open-path diagnostics, executable-adjacent dylib
loading, project ownership, native-dialog handling, and cross-image shutdown
ordering have focused coverage. Crash characterization, ordinary native tests,
DevTool/DHT Python tests, and CMake metadata tests pass with clean processes.

Stage 3 has qualified standalone main-thread Cocoa/GLFW construction, logical
and Retina framebuffer sizing, resize and close state, worker-thread rejection,
and repeated teardown. If GLFW reports no monitors at session startup, MonaImGui
truthfully disables multi-viewport support for that session instead of
publishing fabricated monitor geometry; a transient empty result preserves the
last real snapshot. Keyboard, text, mouse, focus, minimize, restoration, and
full multi-monitor injection remain an open native qualification boundary.
Targets that require this lifecycle now declare the shared native-test
application host rather than using temporary bundles. LaunchServices owns the
Host process; CTest retains discovery, scheduling, timeout, locks, and exact
result policy. The complete Vulkan RHI qualification passes through that path.
The current session still reports no GLFW monitors, and the window lifecycle
qualification preserves that condition as a bounded failure instead of
fabricating display state or skipping the case.

The complete Debug Editor and Launch closure link as Mach-O arm64 with valid
runtime `@rpath` dependencies. Launch now creates the final hidden primary
Cocoa window before `RHIInit()`. ApplicationCore installs the window's
`CAMetalLayer` while still on the AppKit main thread; the RHI thread then uses
that prepared layer to create the real MoltenVK surface without mutating the
Cocoa view hierarchy. MoltenVK selects a surface-qualified queue family,
requires and enables
`VK_KHR_portability_subset`, and lets the first viewport consume that same
surface. Runtime smoke on the Apple M4 host creates an sRGB BGRA swapchain
through this path; Windows keeps its existing Win32 presentation-support query.

Editor startup now loads `AssetImportCore` before the atomic asset-registry
scan, so authored import-record classes are available and the Sandbox catalog
publishes all 28 packages instead of retaining a stale revision. MonaImGui
builds its render-pass layout and pipeline from the actual swapchain format.
Both Sandbox and Project Browser complete three hidden ticks and the ordinary
shutdown path with exit code 0. Final operator-driven macOS qualification on
2026-08-18 exercised focus, keyboard, text, mouse, cursor, monitor/Retina,
minimize/restore, close, and visible normal shutdown without an observed
regression. The same-day Windows ordinary native aggregate passed all 74
targets and the full `all` build remained clean. Stages 3 and 4 and this plan
are complete; completion does not claim the full M3 rendering vertical slice.

This plan is M2 of the
[macOS Platform Enablement roadmap](../Roadmaps/MacOSPlatformEnablement.md) and
consumes the archived M1
[native toolchain handoff](Archive/2026-08/MacOSNativeToolchainBootstrap.md#m2-entry-diagnostics-and-rollback-boundary).

## Goal

Provide real macOS process/crash/module/filesystem and shell services, qualify
the Cocoa/GLFW window and input lifecycle, and make the Editor shell link,
launch, enforce project ownership, handle relaunch/open-path requests, and shut
down cleanly on the declared Apple Silicon baseline. The result must preserve
required common contracts and expose the real M3 rendering entry rather than
bypassing or weakening it.

## Scope

- Characterize the post-test teardown crash and the complete Launch linker and
  runtime boundary with LLDB, sanitizer-assisted diagnostics where supported,
  native logs, and focused deterministic fixtures.
- Implement the required macOS process-crash service with install/uninstall,
  crash-root publication, test option configuration, and native fixture
  behavior matching the common contract.
- Complete the `FMacOSPlatformProcess` operations needed by module loading,
  process launch/wait/return-code behavior, executable and library discovery,
  relaunch/open-path flows, and clean shutdown.
- Qualify filesystem/path behavior, dynamic module lifetime, logging, saved
  directories, and project ownership on the native host.
- Implement or select the macOS native-dialog contract without a generic
  success/failure no-op.
- Qualify GLFW's Cocoa window creation, destruction, focus, keyboard, mouse,
  monitor, DPI/Retina sizing, and event-pump behavior that belongs before
  rendering admission.
- Link and launch the Editor shell through its normal startup path, preserve
  rendering requirements as explicit M3 diagnostics, and shut down without
  leaked modules, dangling callbacks, or teardown crashes.
- Preserve Windows behavior with focused cross-host contract coverage and the
  repository's required regression suites.

## Non-Goals

- Completing MoltenVK graphics/compute correctness, shader parity,
  resize/recreate presentation behavior, or the full rendering qualification
  matrix; those belong to M3. M2 includes only the bounded real-surface device
  admission required to enter the ordinary Editor shell.
- Deciding cooked asset sharing or recooking, derived-data keys, or MacOS cook
  output; those belong to M4.
- Building an `.app` bundle, signing, notarization, packaging, installation,
  CI, Intel, universal binaries, Release/Game/Shipping qualification, or a
  supported-machine matrix; those belong to M5.
- Adding process, crash, dialog, window, or rendering no-ops solely to make a
  target link or appear to launch.

## Design Decisions and Invariants

- Common Launch retains a required platform crash-service contract. MacOS must
  supply a real adapter; common startup must not special-case the platform away.
- Crash handlers perform only async-signal-safe work in signal context. Rich
  reporting, filesystem publication, and test orchestration remain outside the
  signal handler unless a separately verified platform primitive permits them.
- Platform process APIs report explicit failure and native diagnostics. They do
  not return fabricated handles, process identifiers, exit codes, or paths.
- Module load/unload ownership is balanced and testable. Shutdown order must not
  leave global destructors calling code from an unloaded dylib.
- Cocoa work stays on the main thread. GLFW remains the window/event abstraction
  unless a focused native bridge is required for an existing common contract.
- A successful Editor process that bypasses RHI initialization is not M2 exit
  evidence. M2 may stop at a stable, actionable M3 rendering diagnostic, but it
  must reach that boundary through the ordinary startup and shutdown lifecycle.

## Implementation Stages

### Stage 0: Characterize native runtime and shutdown failures

- [x] Capture the complete Launch unresolved-symbol set and prove its ownership
  is limited to the missing macOS crash-service adapter.
- [x] Reproduce both post-test teardown crashes with native crash reports and
  LLDB where the host permits it; record the first
  invalid frame, owning dylib/module, thread, shutdown phase, and relevant
  loader state.
- [x] Inventory every process, module, filesystem, dialog, ownership, Cocoa,
  GLFW, and Editor-shell platform operation reachable before M3.
- [x] Select or add focused regression fixtures before repairing any
  reproducible common or platform lifecycle defect.

#### Acceptance Gate

- Every initial failure has a stable command, native stack or linker evidence,
  an owning stage, and a test boundary; no runtime defect is hidden behind a
  no-op, disabled test, or skipped startup path.

### Stage 1: Implement the macOS process-crash service

- [x] Add the MacOS Launch adapter for handler install/uninstall, crash-root
  publication, test configuration, and native crash fixtures.
- [x] Preserve the common crash-context schema and deterministic fixture
  semantics while using macOS-appropriate signals and filesystem publication.
- [x] Validate handler restoration, repeated install/uninstall, normal exit,
  intentional crash subprocesses, and malformed/unsupported fixture requests.
- [x] Link Launch without weakening the common process-crash contract.

#### Acceptance Gate

- Launch links on arm64; focused normal and crashing subprocess fixtures publish
  the expected diagnostics; repeated handler lifecycle is balanced; and Windows
  crash-service contract coverage remains green.

### Stage 2: Complete process, module, filesystem, dialog, and ownership services

- [x] Implement and test the remaining `FMacOSPlatformProcess` operations needed
  by Launch and the Editor shell, including executable/module discovery,
  subprocess lifetime, return codes, and relaunch/open-path behavior.
- [x] Qualify dylib load, symbol lookup, balanced unload, failure diagnostics,
  and shutdown order across representative engine modules.
- [x] Qualify saved/config/project path creation, normalization, case behavior,
  permissions, migration, logging, and project ownership.
- [x] Implement the required native-dialog behavior and deterministic
  non-interactive/test handling without fabricating a user response.
- [x] Repair the native-test global teardown crash and require clean process
  exit for the previously affected suites.

#### Acceptance Gate

- Focused process/module/filesystem/dialog/ownership tests pass with clean exit;
the affected SkeletalAsset and Spline suites pass as processes, not only as test
bodies; and failure paths provide actionable native diagnostics.

### Stage 3: Qualify Cocoa/GLFW window and input lifecycle

- [x] Create and destroy a Cocoa-backed GLFW window on the main thread through
  the normal ApplicationCore path.
- [x] Qualify event pumping, close requests, focus, keyboard, text, mouse,
  cursor, monitor, DPI/Retina framebuffer sizing, minimize, and restoration.
- [x] Validate repeated window/application construction and teardown without
  dangling callbacks, late events, or module lifetime violations.
- [x] Preserve the explicit Vulkan-surface handoff and qualify the bounded
  real-surface device admission needed by ordinary startup while leaving full
  swapchain, presentation, and rendering correctness to M3.
- [x] Prepare the Cocoa `CAMetalLayer` on the main thread before RHI startup and
  keep Vulkan surface creation on the RHI thread without later AppKit mutation.

#### Acceptance Gate

- Platform-focused window/input fixtures and a repeatable diagnostic-window
  smoke pass on the main thread, terminate cleanly, and expose a stable surface
  handoff without claiming rendering qualification.

### Stage 4: Qualify the Editor shell and publish the M3 handoff

- [x] Build the complete Debug Editor target and audit its arm64 dylib closure,
  install names, rpaths, framework dependencies, and runtime deployment.
- [x] Launch through the normal Editor startup path, exercise project selection
  and ownership plus relaunch/open-path behavior, and reach the real rendering
  boundary without bypasses.
- [x] Validate startup failure plus repeated hidden Sandbox/Project Browser
  launch and automated normal shutdown with clean process exit and preserved
  logs/crash diagnostics.
- [x] Validate visible operator close with clean process exit and preserved
  logs/crash diagnostics.
- [x] Run the selected native, DevTool, DHT, CMake metadata, and Windows contract
  regression suites.
- [x] Update lasting platform/runtime documentation and publish exact M3 entry
  diagnostics without claiming rendering, asset, or product support.

#### Acceptance Gate

- The Editor shell links, launches, owns or opens the selected project, creates
  and services its Cocoa window, reaches the explicit MoltenVK rendering entry,
  and shuts down cleanly on the declared host. The runtime evidence is
  repeatable, Windows contracts remain qualified, and M3 receives a bounded
  rendering handoff.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Crash service | Link closure, handler lifecycle tests, normal subprocess exit, intentional native crash fixtures, published crash context, and Windows parity. |
| Process services | Launch/wait/return-code fixtures, executable and path discovery, relaunch/open behavior, native error diagnostics, and handle cleanup. |
| Module lifecycle | Representative dylib load/symbol/unload tests, failure cases, repeated cycles, shutdown ordering, and clean global teardown. |
| Filesystem and ownership | Saved/config/project directory fixtures, normalization and permission failures, logging, migration, and single-owner behavior. |
| Dialog contract | Native selection/cancel/error behavior plus deterministic non-interactive handling. |
| Window and input | Main-thread Cocoa window lifecycle, focus, keyboard/text/mouse, monitor and Retina sizing, close/minimize/restore, and repeated teardown. |
| Editor shell | Complete Debug Editor build, dependency audit, normal startup path, project/relaunch flows, M3 boundary, repeated clean shutdown, and logs. |
| Regression | Focused native suites, DevTool and DHT Python suites, CMake metadata checks, and synthetic/hosted Windows contract coverage. |

Build, native-test selection, and documentation validation follow the
repository [agent build workflow](../Agents/BuildAndRun.md),
[agent testing workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md).

## Definition of Done

- All Stage 0-4 acceptance gates pass on the declared Apple Silicon baseline.
- Launch has a real macOS crash service and every required process/module/
  filesystem/dialog/ownership contract used by the Editor shell is implemented
  and covered by native evidence.
- Previously crashing native suites exit successfully after global teardown.
- Cocoa/GLFW window and input lifecycle plus the ordinary Editor shell startup
  and shutdown path are repeatable without bypassing rendering requirements.
- Windows shared behavior remains qualified and lasting documentation matches
  the implemented runtime contract.
- Completion makes no MoltenVK rendering, cooked asset, packaging, signing,
  distribution, Intel, universal, or product-support claim.

## Deferred Follow-ups

- M3 owns full MoltenVK device/surface/swapchain qualification beyond the
  bounded startup-admission slice, plus shaders, rendering, synchronization,
  presentation, resize, and GPU diagnostics.
- M4 owns cooked payload compatibility, platform cook outputs, derived-data
  identities, migrations, and representative content.
- M5 owns configuration expansion, `.app` assembly, CI, signing, notarization,
  packaging, installation, and the supported-machine matrix.

## Related Documentation

- [macOS Platform Enablement roadmap](../Roadmaps/MacOSPlatformEnablement.md)
- [macOS Native Toolchain Bootstrap](Archive/2026-08/MacOSNativeToolchainBootstrap.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Agent Build And Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Documentation Workflow](../Agents/Documentation.md)

## Related Code

- `Engine/Source/Runtime/Core/Private/MacOS/`
- `Engine/Source/Runtime/Core/Public/MacOS/`
- `Engine/Source/Runtime/Launch/Private/ProcessCrashServices.h`
- `Engine/Source/Runtime/Launch/Private/Windows/WindowsProcessCrashServices.cpp`
- `Engine/Source/Runtime/ApplicationCore/`
- `Engine/Source/Programs/DurinLauncher/`
- `Engine/Tests/Native/CoreTests/`
- `Engine/Tests/Native/EngineTests/`
