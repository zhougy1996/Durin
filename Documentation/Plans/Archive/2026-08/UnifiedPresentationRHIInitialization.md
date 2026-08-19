# Unified Presentation-Aware RHI Initialization Plan

Summary: Replace the platform-split, setter-driven startup surface handoff with one explicit Windows/macOS presentation-aware RHI initialization protocol and backend-owned RAII candidate.

Last reviewed: 2026-08-18

Status: Archived
Completed: 2026-08-18

## Current Status

Windowed startup now creates and registers the hidden primary native window,
then passes an explicit presentation `FRHIInitializationContext` directly into
backend initialization on the selected RHI execution thread. Windows and macOS
both create the real startup surface before device selection and qualify the
queue family against that exact surface. Explicit headless initialization
creates no surface.

The surface is owned by a move-only Vulkan presentation candidate until the
startup viewport explicitly requests adoption. `FGenericWindow` has no added
identity or presentation token; the native handle is only a defensive wrong-
window check. Candidate, backend, and ApplicationCore rollback paths have
explicit owners and focused failure coverage.

Stage 0 is complete. GLFW required-instance-extension discovery now validates
the borrowed array before forming a pointer range, owns its failure diagnostic,
and unwinds cursors plus GLFW without publishing initialized state. A pure
query seam covers missing storage, invalid names, and successful borrowed
extensions in `NativeWindowModalLoopTests`.

The public migration uses a value-style initialization context, a Vulkan-owned
RAII candidate, and an explicit viewport adoption request. Stage 1 is complete:
`RHIInit` and backend `Init` now transport one complete explicit headless or
presentation context, the setter protocol is removed, and every production and
test caller names its initialization mode. Thread-launch failure, backend
rollback, context delivery, and capability publication pass in
`RHIInitializationTests`; the Launch module also builds with the migrated API.
Stage 2 is complete. Presentation-mode Vulkan now creates one real startup
surface on both Windows and macOS, wraps it immediately in a move-only RAII
candidate, and qualifies queue families against that exact surface through
`getSurfaceSupportKHR`. Explicit headless initialization skips surface queries
and selects a graphics/compute queue without claiming presentation support.
The Windows Vulkan integration fixture covers injected surface failure with
full backend/module rollback, shutdown before candidate consumption, retry,
candidate reuse by the startup viewport, and an independent detached surface;
all 61 Vulkan integration tests pass with validation enabled where requested.

The planned "pure candidate-state" construction test was replaced by this
backend-observable integration sequence. Constructing the owner without a live
instance and surface would violate the RAII type's invariant and could not
prove destruction ordering; injected live-surface coverage directly proves the
relevant ownership boundary.

Stage 3 is complete with the deliberately simpler protocol selected during
implementation review. `FRHIViewportCreateInfo` explicitly requests one-shot
startup candidate adoption through a boolean; `FGenericWindow` has no added
identity, and the native handle is retained only as a defensive candidate/
viewport match check. Mona carries the adoption intent only until the startup
viewport succeeds. Mismatched and duplicate requests fail without replacement-
surface fallback, while ordinary detached viewports create independent
surfaces. `RHIInitializationTests` pass 6/6, `VulkanRHIIntegrationTests` pass
61/61, and the Launch module builds.

Stage 4 and the plan are complete. Focused and aggregate native tests,
`fast-all`, the full `all` build, validation-enabled threaded and inline Windows
startup/shutdown smoke runs, failure injection, and lasting documentation
validation pass. The focused aggregate briefly exposed one unrelated
thumbnail-test flake; its focused rerun and the subsequent complete aggregate
both passed. The user additionally reported successful manual runtime
validation of the remaining presentation workflow, including the platform
startup behavior, so the interactive Windows and macOS acceptance gates are
closed.

## Goal

Make presentation-aware initialization a typed RHI protocol rather than a
macOS side channel. Windowed Windows and macOS startup both create one real
surface before Vulkan physical-device and queue-family selection, then
explicitly transfer that surface exactly once to the intended startup
viewport. Headless initialization remains explicit and creates no presentation
resource. Every intermediate owner, failure, thread, and destruction path is
observable from types, state, diagnostics, and tests.

## Scope

- An explicit `FRHIInitializationContext` distinguishing presentation-aware
  and headless startup without a nullable default argument implying policy.
- A typed presentation initialization target that keeps the required native
  handle inside the explicit context instead of a pre-init backend setter.
- Passing initialization context directly into backend initialization on the
  owning RHI execution thread; removal of the pre-init presentation setter.
- A backend-owned Vulkan presentation candidate that owns the initialization
  surface through RAII, exposes it non-owningly for admission, and supports one
  checked transfer to the matching startup viewport.
- The same real-surface device and queue-family qualification on Windows and
  macOS, with platform differences confined to native surface preparation and
  creation.
- A viewport creation descriptor that explicitly requests initialization
  candidate adoption and makes viewport creation a non-`const` state-changing
  operation.
- Complete startup rollback, candidate destruction, viewport-construction
  failure, never-consumed candidate, duplicate-consumption, and orderly
  shutdown behavior in threaded and inline RHI execution.
- Focused RHI, ApplicationCore, VulkanRHI, Launch, and window-backed viewport
  validation plus lasting runtime-contract updates.

## Non-Goals

- A render graph, swapchain rewrite, general resource-ownership framework, or
  redesign of render-command submission.
- Supporting multiple initialization candidates or selecting a device against
  every window that may be created later in the process.
- Moving `CAMetalLayer` preparation away from the macOS main thread or changing
  GLFW/AppKit window decoration and event behavior.
- Removing native `void*` handles from the final platform ABI boundary.
- Changing swapchain format, image count, present-mode policy, resize recovery,
  or detached Play-window behavior beyond using the selected device and normal
  post-startup surface path.
- Supporting presentation on a physical device or queue family that was not
  admitted for the startup presentation target.
- Treating a Windows-only build, a macOS-only build, or a synthetic pure test as
  complete cross-platform qualification.

## Design Decisions and Invariants

### Initialization contract

- `RHIInit` accepts one value-style `FRHIInitializationContext`; it does not
  accept a defaulted raw window pointer. The context represents exactly one of
  two valid alternatives: explicit headless initialization or presentation
  initialization with a valid target descriptor.
- A presentation target contains the non-owning native handle required by the
  platform surface adapter. The explicit adoption request determines candidate
  consumption; native-handle equality is only a defensive wrong-window check.
- Launch owns the startup window and its native lifetime across RHI
  initialization and startup viewport creation. The initialization context
  does not extend ApplicationCore window lifetime and documents that
  precondition.
- `FDynamicRHI::Init(const FRHIInitializationContext&)` receives the context on
  the same inline or dedicated RHI thread that performs backend initialization.
  `SetInitializationPresentationWindow()` is removed; no backend may observe a
  separately published half-initialized context.
- Tests inject the same context through `RHIInitWithBackendForTests`; test-only
  entry points do not preserve the old setter protocol.

### Platform boundary and surface creation

- Presentation-mode Vulkan initialization creates a real surface on both
  Windows and macOS before physical-device admission. Queue-family support is
  queried against that exact `VkSurfaceKHR` through the common Vulkan surface
  support operation.
- macOS retains its required ordering: AppKit-main-thread window creation
  prepares the `CAMetalLayer`, and the RHI thread creates and owns the Vulkan
  surface after instance creation.
- Windows creates the initialization surface through the same registered
  `FGenericWindow::CreateVulkanSurface()` adapter used by ordinary viewports;
  its current Win32-only admission query is not the windowed-startup authority
  after migration.
- RHI public types remain backend-neutral. Vulkan instance types, surface
  handles, and deleters remain inside VulkanRHI; ApplicationCore continues to
  expose the final type-erased native surface creation boundary.
- Headless mode creates no surface and does not pretend that presentation was
  qualified. Any future headless device-selection relaxation must be explicit
  policy rather than a null-surface fallthrough in the windowed path.

### Candidate ownership and consumption

- Vulkan owns the startup surface in one move-only RAII presentation-candidate
  object as soon as native creation succeeds. The candidate records the
  expected native window and an explicit `Available` or `Consumed` state.
- Device selection may borrow the surface but cannot release or transfer it.
  Only startup viewport creation may consume it, and only when its descriptor
  explicitly requests initialization-candidate adoption.
- `FRHIViewportCreateInfo` replaces the positional window-backed viewport
  arguments and contains an optional request to adopt the initialization
  candidate. Ordinary and later viewports omit that request and create their
  own surface normally.
- `RHICreateViewport` is non-`const`. Candidate adoption is a named state
  transition, not mutation hidden behind `mutable` or `std::exchange`.
- The Vulkan viewport becomes the surface owner only after its constructor has
  established rollback-safe ownership. If swapchain or viewport construction
  fails, exactly one live owner destroys the surface on the RHI thread.
- A mismatched native window, missing candidate, or second adoption request fails
  deterministically with an owned diagnostic. It never silently creates a
  replacement surface for a call that explicitly requested candidate adoption.
- A never-consumed candidate remains backend-owned and is destroyed before the
  Vulkan instance during backend shutdown or initialization rollback.

### Failure, threading, and ordering

- GLFW required-instance-extension discovery returns success or an owned
  diagnostic. Null extension storage, zero count when Vulkan support is
  required, or an active GLFW error stops ApplicationCore initialization and
  rolls back cursors and GLFW exactly once.
- Instance, debug messenger, candidate surface, logical device, and published
  capabilities follow a single reverse-order backend shutdown path that is
  valid after every partial initialization stage.
- Surface creation, admission queries, candidate transfer, swapchain creation,
  and Vulkan surface destruction execute on the RHI execution thread in both
  dedicated and inline modes. Main-thread-only macOS layer preparation remains
  complete before that work begins.
- RHI initialization publishes success only after device selection and
  capabilities publication complete. Viewport creation begins only after
  successful RHI initialization and rendering-command admission setup.
- Diagnostics preserve the primary failure and append rollback failure without
  replacing the owned cause.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| Launch ordering | The hidden primary `MWindow` and native window exist before `RHIInit`; Engine later adopts the same startup window. | The old API passed only a raw pointer and did not name presentation versus headless mode or startup adoption intent. |
| Backend rollback | Failed backend `Init()` calls `Shutdown()` on the RHI execution thread, then releases the backend and module. | Partial Vulkan resource stages and initialization-candidate ownership need explicit coverage under that path. |
| macOS preparation | The Cocoa window prepares its Metal layer on the main thread before RHI startup. | The prepared target enters Vulkan through a macOS-only hidden candidate protocol. |
| Windows admission | Vulkan can query Win32 queue presentation support and create a GLFW surface for a viewport. | Admission is not qualified against the real startup surface and does not reuse it. |
| Surface creation | Registered `FGenericWindow` implementations create a Vulkan surface from an instance. | Failure diagnostics are coarse, and initialization ownership is represented by raw backend fields. |
| Viewport creation | Vulkan can accept an optional already-created surface internally. | Public creation uses positional arguments, remains `const`, and silently consumes by raw-pointer equality. |
| GLFW extensions | ApplicationCore initializes GLFW before Vulkan and publishes required instance extensions. | A null/invalid extension query is inserted without validation or rollback-aware failure propagation. |
| Tests | RHI initialization rollback and Vulkan viewport failure injection already have focused fixtures. | Context transport, real-surface admission, wrong-window rejection, one-shot adoption, and cross-platform startup ownership require qualification. |

## Implementation Stages

### Stage 0: Close discovery failures and freeze the protocol

- [x] Change required GLFW Vulkan instance-extension discovery to return a
  checked result with an owned diagnostic; reject null/invalid results and
  unwind cursor/GLFW initialization without publishing ApplicationCore as
  initialized.
- [x] Add focused failure coverage for GLFW extension discovery independent of
  a working Vulkan device, using an injectable query seam rather than process-
  global driver manipulation.
- [x] Inventory every `RHIInit`, backend `Init`, `RHICreateViewport`, native
  surface creation, presentation-support query, failure-injection hook, and
  startup-window call site affected by the signature migration.
- [x] Freeze the exact public shapes and naming for initialization mode,
  presentation context, viewport descriptor, and candidate-
  adoption request; record module ownership and export requirements.
- [x] Specify the complete state table for no candidate, available candidate,
  admission use, matching adoption, mismatched adoption, duplicate adoption,
  viewport failure, initialization failure, and shutdown without adoption.
- [x] Confirm the reverse-order Vulkan shutdown operation is safe after failure
  at each instance/debug/surface/device/capability boundary; add a dedicated
  state marker only if unconditional idempotent cleanup cannot express it.

#### Acceptance Gate

- GLFW discovery failure is handled without undefined pointer arithmetic,
  leaked process state, or lost diagnostics. The new context and candidate
  state table has one selected representation, no unresolved ownership or
  thread decision, and every affected caller/test target is enumerated before
  public signatures change.

### Stage 1: Replace setter-driven initialization with a typed context

- [x] Add backend-neutral initialization-mode, presentation target, and
  `FRHIInitializationContext` types under RHI with concise
  ownership and lifetime contracts.
- [x] Change production and test `RHIInit` paths to accept the context by value
  and deliver it directly to backend `Init` on the selected RHI execution
  thread.
- [x] Remove `FDynamicRHI::SetInitializationPresentationWindow()` and all
  backend/test state used only to emulate pre-init context publication.
- [x] Update Launch to construct presentation mode from the real registered
  startup window; route any supported display-suppressed/headless entry through
  the explicit headless alternative.
- [x] Preserve launch failure diagnostics, module unloading, thread-start
  failure behavior, capability publication, and retry cleanliness in inline
  and threaded modes.
- [x] Extend RHI initialization tests to prove exact context delivery, no
  backend `Init` after thread-launch failure, rollback on the owning thread,
  and no globally retained presentation context after failure.

#### Acceptance Gate

- Every backend receives one complete immutable initialization context through
  `Init`; the setter and defaulted raw-pointer API no longer exist. Headless and
  presentation startup are distinguishable at every production and test call
  site, and existing rollback tests pass in both execution modes.

### Stage 2: Unify real-surface admission and RAII ownership

- [x] Add a move-only `FVulkanPresentationCandidate` that records the expected
  native window, owns its `VkSurfaceKHR`, exposes a non-owning admission view, and
  destroys an available surface before instance teardown.
- [x] In presentation mode, create the candidate after instance/debug setup and
  before physical-device enumeration on both Windows and macOS.
- [x] Replace platform-split presentation admission with one real-surface
  `getSurfaceSupportKHR` query for all queue families on both platforms;
  preserve platform-specific creation/preparation only below that boundary.
- [x] Make presentation-mode device rejection diagnostics distinguish surface
  creation failure, no device, unsupported required extensions/features, and
  no queue family supporting the actual startup surface.
- [x] Preserve an explicit headless device-selection route without invoking a
  native presentation query or manufacturing a false presentation capability.
- [x] Add backend-observable candidate-state coverage and Vulkan failure
  injection at surface creation, admission/logical-device boundaries, and
  shutdown without consumption; retain the existing enumeration, capability,
  and initialization rollback coverage.

#### Acceptance Gate

- Windows and macOS presentation initialization both select a device and queue
  family against the exact real startup surface. At every injected failure
  there is exactly one surface owner, cleanup occurs on the RHI thread before
  instance destruction, and headless mode creates no presentation resource.

### Stage 3: Make startup viewport adoption explicit

- [x] Introduce `FRHIViewportCreateInfo` for native handle, extent, fullscreen,
  preferred format, present policy, and explicit initialization-candidate
  adoption.
- [x] Convert `FDynamicRHI::RHICreateViewport` and all callers/tests to the
  descriptor, remove `const`, and retain existing game-thread plus synchronous
  RHI-thread execution semantics.
- [x] Replace `TakeInitializationPresentationSurface(void*) const`, mutable
  surface storage, and raw-pointer matching with the checked candidate state
  transition named by the descriptor.
- [x] Transfer candidate ownership transactionally into `FVulkanViewport` and
  prove constructor/swapchain failure destroys the surface exactly once while
  leaving no reusable consumed candidate.
- [x] Make missing, mismatched, and duplicate adoption requests deterministic
  failures; prove an ordinary later or detached viewport uses independent
  surface creation and cannot accidentally consume startup state.
- [x] Update the startup game/editor path so the first window-backed viewport
  for the primary startup window explicitly adopts the candidate. Offscreen
  editor viewports do not participate.

#### Acceptance Gate

- The intended startup viewport explicitly adopts the initialization surface
  once; no `mutable`, hidden `std::exchange`, pointer-driven consumption, or
  silent adoption fallback remains. Candidate, viewport-construction failure,
  detached window, offscreen viewport, and orderly shutdown tests demonstrate
  exact ownership.

### Stage 4: Qualify startup, rollback, and publish the contract

- [x] Run focused ApplicationCore/RHI unit tests and VulkanRHI integration tests
  through the repository build/test workflow in threaded and inline modes.
- [x] Run Windows validation-enabled windowed startup through startup viewport
  creation, stable presentation, resize, detached window creation/destruction,
  and orderly shutdown; record surface and queue-family evidence.
- [x] Run the equivalent macOS validation-enabled startup, including main-
  thread Metal-layer preparation, RHI-thread candidate creation/adoption,
  stable presentation, resize, and shutdown.
- [x] Exercise failure injection after window creation, instance creation,
  candidate creation, device admission, logical-device creation, viewport
  adoption, and swapchain creation; prove Launch/ApplicationCore/RHI cleanup is
  complete and a later process startup is clean.
- [x] Run the native aggregate and a full `all` build because the public RHI
  signature migration crosses ApplicationCore, Launch, Engine, RHI,
  VulkanRHI, MonaCore, Renderer-facing viewport callers, and native tests.
- [x] Update the lasting runtime lifecycle and viewport-rendering contracts with
  the final initialization mode, platform preparation boundary, real-surface
  admission, candidate ownership, adoption, and shutdown rules.

#### Acceptance Gate

- Focused tests, both execution modes, Windows and macOS validation-enabled
  runtime matrices, injected rollback paths, native aggregate, full build, and
  documentation validation pass. Lasting contracts describe the implemented
  protocol, and no active documentation presents the old setter/pointer-
  matching flow as authoritative.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| GLFW extension query failure | ApplicationCore reports the owned cause, rolls back cursors/GLFW once, and publishes neither extensions nor initialized state | Focused ApplicationCore or Engine test seam |
| Thread launch failure | Backend `Init` is never called; context and backend are released without surface creation | `RHITests` |
| Context delivery | Exact headless or presentation alternative reaches backend `Init` on the selected RHI execution thread | `RHITests` in threaded and inline modes |
| Windows admission | Real startup Win32/GLFW surface qualifies the chosen presentation queue | `VulkanRHITests` plus Windows runtime |
| macOS admission | Main-thread-prepared Metal layer produces the real surface used to qualify the chosen queue | macOS lifecycle/integration tests plus runtime |
| Headless initialization | No native surface or presentation candidate is created; device policy is explicit | `RHITests` and Vulkan pure/integration coverage |
| Candidate creation failure | Initialization fails with owned diagnostic and instance/debug resources roll back in reverse order | Vulkan failure injection |
| Device or queue rejection | Diagnostic identifies rejection against the actual startup surface; candidate remains owned until rollback | Vulkan selection tests |
| Matching adoption | Named startup target transfers the exact candidate surface once to the startup viewport | Vulkan viewport integration test |
| Mismatch or duplicate | Call fails deterministically without replacement-surface fallback or double destruction | Candidate state and viewport tests |
| Viewport constructor failure | Exactly one owner destroys the adopted surface on the RHI thread; no stale candidate remains | Vulkan failure injection |
| Candidate never consumed | Backend shutdown destroys it before the instance | Vulkan teardown test |
| Ordinary later viewport | Creates and owns an independent surface and cannot observe initialization candidate state | Detached/multi-window Vulkan test |
| Resize and presentation | Swapchain recreation preserves existing surface ownership and presentation behavior | Windows/macOS runtime matrix |
| Shutdown and restart boundary | Viewports retire before device/backend/application teardown; no validation error, leaked candidate, or pending global state remains | Vulkan teardown and process-boundary smoke |

## Definition of Done

- Windowed Windows and macOS startup share one presentation-aware RHI
  initialization protocol and qualify device/queue selection against the exact
  real startup surface.
- Headless startup is an explicit initialization alternative and cannot be
  confused with a missing required window.
- Initialization context is delivered atomically through backend `Init`; the
  presentation setter, backend raw window field, mutable initialization
  surface, pointer-driven consumption, and hidden `std::exchange` are
  removed.
- A move-only Vulkan candidate and the startup viewport form one checked,
  exactly-once ownership transfer; all failure and never-consumed paths destroy
  the surface once on the RHI thread before instance teardown.
- GLFW Vulkan extension discovery, backend partial initialization, viewport
  adoption, swapchain creation, and Launch failure paths preserve owned
  diagnostics and complete rollback.
- Focused, aggregate, full-build, Windows runtime, macOS runtime, and
  documentation validation gates pass, and lasting lifecycle/viewport
  documentation owns the implemented contract.

## Deferred Follow-ups

- Multiple simultaneous presentation candidates or device admission against a
  declared set of future windows.
- A backend-neutral presentation-capability token for future multi-candidate
  initialization.
- Hot replacement of the primary native window during RHI initialization.
- Cross-adapter or multi-GPU presentation selection and per-window adapter
  routing.
- Presentation recovery after device loss; this plan covers initialization,
  viewport construction, resize, and orderly teardown only.

## Related Documentation

- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Test Execution](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/ApplicationCore/Private/Misc/ApplicationCoreGlobals.cpp`
- `Engine/Source/Runtime/ApplicationCore/Private/Misc/GlfwVulkanInitialization.h`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp`
- `Engine/Source/Runtime/ApplicationCore/Public/Window/GenericWindow.h`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIGlobals.h`
- `Engine/Source/Runtime/RHI/Public/RHIInitialization.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Private/RHIGlobals.cpp`
- `Engine/Source/Runtime/RHI/Private/DynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Public/VulkanDynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanGenericPlatform.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPresentationSupport.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPresentationCandidate.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPresentationCandidate.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/Windows/VulkanWindowsPresentationSupport.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/MacOS/VulkanMacOSPresentationSupport.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
- `Engine/Tests/Native/RHITests/Private/RHIInitializationTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanSwapchainSelectionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Launch/MacOSWindowLifecycleTests.cpp`
