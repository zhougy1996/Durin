# RDG Single-Use Builder Plan

Summary: Consolidate RDG declaration, compilation, and recording into one single-use builder while preserving compiler diagnostics and renderer failure transactions.

Last reviewed: 2026-09-07

Status: Completed
Completed: 2026-09-07

## Current Status

Stages 0–3 are complete. Explicitly authorized application execution resolved
LaunchServices denial. Vulkan instance requirements now include the base surface
extension required by the existing swapchain-enabled device and prebuilt
presentation pipelines, including headless initialization. Surface maintenance
activation also checks that its base surface dependency is enabled. Focused
Vulkan regression and editor output tests pass without validation-layer errors.
Default application-test capability is restored to OFF.

The builder now owns compilation records and all parameter/value/callback
storage. Execute seals every graph shape and reports compile/preparation failure,
recording success, or invalid reuse. Execute reentry from parameter/value constructors consumes the
builder with CompileFailed before recording incomplete storage. Release-enabled declaration gates reject
late or reentrant authoring. Tests cover owning captures, unchanged reports on
reuse, exact parameter identity, allocator/callback reentry, supported unwinding,
exact-once storage destruction, failure transactions, and explicit external
state handoff. Compiler algorithms and renderer publication policy are retained.

### Stage 0 decisions and inventory

- Production consumers: `RDG.h/.cpp`, `SceneRenderPipeline.cpp`, and
  `SceneRendererProfiling.h/.cpp`. Test consumers: `RDGTests.cpp`,
  `VulkanResourceTransitionTests.cpp`, `EditorGridVulkanTests.cpp`, and
  `RendererSceneContractTests.cpp`, including source-scanning assertions.
- Public `ERDGExecutionStatus` and `FRDGExecutionResult` report CompileFailed,
  PreparationFailed, Recorded, or InvalidState. `ERDGBuilderState` exposes the
  lifecycle for diagnostics. Execute takes the command list and an explicit
  optional execution-context pointer; absent allocators are valid only when
  no retained logical RHI resources need allocation.
- Illegal declaration calls use `requiref` before touching declarations or
  consuming parameter references, matching existing fatal callback-authority
  validation and the all-configuration authoring invariant convention. This
  applies to void and handle-returning methods alike. Declaration validation
  errors while Building still use the existing deferred compiler errors.
- Compiled records are private builder-owned storage. Parameters, values and
  callback owners remain in declaration storage until builder destruction;
  runtime pass records borrow callbacks by declaration identity.
- A friend `FRDGBuilderTestAccessor`, defined only in the native RDG test
  translation unit, invokes private compilation and returns success/error
  evidence. Successful compile-only inspection leaves it sealed in Preparing; it cannot produce an executable object.
- Pre-execution and failed-compile captures have `bCompiled = false`, empty
  compiled records, and the builder's budget. Successful compilation sets
  `bCompiled = true`, including after preparation failure. The execution report
  and state are separate from pointer-free compiler evidence. Duplicate Execute
  never changes either evidence or the original report.
- Preparation failure retains compiler diagnostics and resource storage, runs
  no callback or graph command, and publishes no extraction. Renderer maps
  compilation failure separately, publishes requested captures after successful
  compilation even if preparation fails, and commits frame outputs only after
  Recorded and existing feature-result checks.
- Mutation audit: resource registration/creation, token/value creation,
  extraction queues, all AddPass forms, parameter allocation, dependency/root/
  culling/budget edits, all manual uses and erased typed-value uses need the
  common Building gate. Constructor callbacks in typed allocation, allocator
  reentry, and pass callbacks need execution-state coverage; supported C++
  unwinding leaves execution terminally Failed without swallowing exceptions.

## Goal

Expose one non-const `FRDGBuilder::Execute` operation that privately compiles,
prepares retained resources, records passes, and publishes successful outputs.
The builder owns graph storage throughout this lifecycle. The public API must
not expose an independently owned executable compilation result or imply
replay, retry, concurrent execution, or GPU completion.

## Selected Decisions

- Keep existing pass declaration and typed-parameter APIs wherever their
  semantics remain valid. Keep dependency analysis, culling, stable ordering,
  resource lifetime analysis, and barrier generation algorithms unchanged.
- Remove public `FRDGCompileResult` and `FRDGCompiledGraph`. Keep compilation
  records in private builder-owned state; declaration and compiled records may
  remain separate internal structures. A forwarding wrapper around the old
  public ownership model is not the completed design.
- Parameter allocations, destructor records, typed values, and callbacks have
  one graph owner. Remove storage-transfer flags and cross-owner moves.
  Submitted parameters remain immutable; typed values remain writable only
  through declared callback capabilities. Storage survives recording success
  or failure until builder destruction. Existing allocator borrowing,
  extraction detachment, and RHI retirement contracts remain authoritative.
- The public entry point remains explicit about its command list and execution
  allocator. Return a structured `FRDGExecutionResult` with status and error,
  distinguishing `CompileFailed`, `PreparationFailed`, `Recorded`, and
  `InvalidState`. Do not replace recoverable failures with UE-style void-only
  execution. `Recorded` describes CPU command recording, not GPU completion.
- Use the lifecycle `Building -> Compiling -> Preparing -> Recording ->
  Recorded`, with terminal `Failed` on supported failure paths. Every call to
  Execute consumes the building phase, including compile/preparation failure.
  A second execution returns `InvalidState` before allocation, commands,
  callbacks, extraction, or changes to the original execution report.
- Every declaration/mutation entry point requires `Building`. Reentrant calls
  during compilation, allocation, or callbacks are rejected by the same
  lifecycle gate. Guards remain enabled in release builds. The builder is
  thread-confined; this guard does not promise thread-safe concurrent calls.
- Reuse the existing authoring-error mechanism for illegal declaration calls,
  without mutating finalized declarations or diagnostics. Stage 0 must settle
  the exact mechanism for void and handle-returning APIs against repository
  conventions before implementation.
- Retrying requires a new builder and freshly authored graph. Preparation
  failure records no graph commands or extraction and runs no pass callback;
  allocator side effects still follow its existing contract. Once recording
  starts, provide no rollback or replay promise. If the supported failure
  mechanism unwinds, leave the builder terminally failed; do not introduce
  exception recovery for fatal authoring assertions.
- Keep `Capture`, `Dump`, statistics, and budget inspection available on the
  builder after execution. Define pre-execution and failed-compilation queries
  explicitly: never expose partial compiler output as a successful plan.
  Successful compiled diagnostics remain available after preparation failure.
  Owning, pointer-free `FRDGCapture` remains valid after builder destruction.
- Preserve the renderer's compile/preparation failure distinction, output
  publication transactions, capture demand checks, and observational budget
  behavior. Budget observations cannot authorize or prevent graph execution.
- Tests may access a private compile stage through a narrow test accessor.
  It returns diagnostic evidence, never an executable graph. A compile-only
  test consumes/seals its builder and cannot subsequently call public Execute;
  tests requiring both observations and execution inspect after Execute or
  author a second builder. Production has no alternate compile-only API.
- External resources retain the existing initial/final access contract. A new
  graph must declare its correct entrance state and obey timeline ordering;
  creating a fresh builder does not discover unknown external uses. Do not
  replay old barriers or silently substitute the previous final access.

## Non-Goals

Reusable compiled templates, payload cloning, cross-frame graph replay,
automatic retry, new RHI state tracking, allocator redesign, scheduling
optimization, new parallel execution, and copying all UE RDG APIs are excluded.
Binding the command list in the builder constructor is not required for this
lifecycle change. Existing feature authoring should not be rewritten solely
to imitate UE naming.

## Implementation Stages

### Stage 0: Freeze lifecycle and migration contracts

- [x] Inventory all production, test, and diagnostic consumers, including
  inferred compile-result types and source-scanning contract tests.
- [x] Finalize result/status definitions, mutation rejection behavior,
  compile-only test accessor visibility, and pre-execution/failed capture
  semantics; record decisions here before source changes.
- [x] Identify all graph mutation entry points and callback escape paths;
  map each to lifecycle guards and ownership cleanup.
- [x] Review the current failure and extraction tests and define the exact
  compatibility expectations for renderer status and output publication.

Completion: no unresolved public ownership, state, error, or diagnostic
decision remains; the affected call sites and validation targets are recorded.

### Stage 1: Consolidate ownership and migrate the public API

Depends on Stage 0.

- [x] Move compiled/runtime state under the builder, retaining private
  compiler separation and removing conditional storage transfers.
- [x] Implement non-const Execute, structured results, and release-enabled
  lifecycle gates on execution and declaration entry points.
- [x] Rebind pass-resource views and parameter resolvers to builder-owned
  execution state without weakening pass identity or declared capabilities.
- [x] Move diagnostic access to the builder and implement the private
  compile-only test accessor without a second compiler implementation.
- [x] Migrate scene orchestration and profiling/capture plumbing; preserve
  failure mapping, observational budgets, and output transactions.
- [x] Migrate existing compiler/execution tests and Vulkan fixtures so the
  public compiled-graph types can be removed in the same coherent change.
  Replace tests that intentionally outlive the builder with the new ownership
  expectations rather than retaining the old API for compatibility.

Completion: affected targets build, migrated existing tests pass, production
has one execution entry point, and searches find no obsolete public executable
result ownership or transferred-storage flags in active code.

### Stage 2: Prove single-use and failure semantics

Depends on Stage 1.

- [x] Cover empty graphs, manual graphs, parameterized graphs, and typed-value
  graphs: execution consumes the builder uniformly, including failure.
- [x] Reject duplicate Execute and representative late declaration calls;
  audit every mutation entry point for the common guard.
- [x] Exercise reentry from allocator and pass callback paths; verify rejected
  execution does not allocate, record, publish, or overwrite prior diagnostics.
- [x] Verify exact-once destruction on unused-builder, compile failure,
  preparation failure, normal execution, and supported unwinding paths;
  preserve submitted parameter identity and typed-value access enforcement.
- [x] Verify failed preparation runs no pass and leaves extraction destinations
  unchanged, including missing and incompatible allocator results.
- [x] Verify capture survives builder destruction and retains compiler evidence
  after preparation failure; repeated execution must not alter that evidence.
- [x] Test an external resource whose initial/final accesses differ: second
  execution is rejected before barriers, and a fresh graph using the correct
  entrance state generates the expected new transition sequence.

Completion: lifecycle regressions are covered by meaningful behavioral tests;
compiler ordering, culling, range tracking, and barrier expectations remain
unchanged outside the intentionally revised ownership contract.

### Stage 3: Qualify renderer integration and publish the contract

Depends on Stage 2.

- [x] Run renderer contract coverage for graph authoring, failure status,
  diagnostic publication, and frame output transactions.
- [x] Run Vulkan transition qualification for the migrated graph path and
  cross-graph external state handoff; inspect validation failures before
  attributing them to the lifecycle refactor.
- [x] Update Render Graph and affected frame-preparation/recovery contracts
  with implemented ownership, single-use execution, retry, and capture rules.
- [x] Record exact validation evidence and any unavailable platform gates;
  do not mark completion while a required gate remains unfulfilled.
- [x] Close stage checklists, validate documentation and plan lifecycle, and
  mark this plan completed only after all acceptance conditions pass.

Completion: renderer and backend gates pass, lasting rules live in their
owning runtime documents, and no public documentation promises an independent
reusable executable graph.

## Stage 3 final validation evidence

- Final extension negotiation, device selection, and RDG transition selection
  passed (20 cases); EditorGridVulkanTests passed (8 cases). Detailed outputs
  contain no Vulkan validation errors or VUID diagnostics. Evidence:
  `Build/.agent-state/logs/20260907-040858-506182-35112-ctest.log`,
  `Build/.agent-state/logs/20260907-040858-vulkan-extension-detail.log`,
  `Build/.agent-state/logs/20260907-040914-431584-35178-ctest.log`, and
  `Build/.agent-state/logs/20260907-040914-editor-extension-detail.log`.
- The fix retains swapchain support for eagerly created presentation-compatible
  renderer pipelines in headless mode and requests its required base instance
  extension without requiring native window extensions. Regression coverage
  verifies headless base requirements and rejects optional surface maintenance
  activation when the base extension is only available, not enabled.
- This focused plan acceptance gate supersedes the unbounded `test affected`
  mapping for qualification-only Vulkan source ownership. Earlier renderer
  contract and full-build evidence remains below. No performance claim is made.

## Earlier validation and diagnostic history

- After explicit user authorization, sandbox-exempt
  `./DevTool test VulkanRHIIntegrationTests 'FVulkanResourceTransitionTests.*'
  --mode qualification` passed, followed by
  `./DevTool test EditorGridVulkanTests --mode qualification`. Evidence:
  `Build/.agent-state/logs/20260907-040143-061357-33046-ctest.log` and
  `Build/.agent-state/logs/20260907-040220-435603-33182-ctest.log`.
  This establishes that LaunchServices access was the execution blocker; the
  separately observed bundle-signature verification issue did not prevent these
  launches. No signing or machine authorization settings were changed.
  Editor output nevertheless reports `VUID-vkCreateInstance-ppEnabledExtensionNames-01388`
  and `VUID-vkCreateDevice-ppEnabledExtensionNames-01387`: initialization enables
  surface-capability/maintenance or swapchain extensions without the required
  `VK_KHR_surface` instance extension in some initialization paths. Retained detail:
  `Build/.agent-state/logs/20260907-040220-rdg-editor-vulkan-detail.log`.
  Passing CTest exit status is not a clean validation-layer result. Triage these
  initialization diagnostics before closing Stage 3; no RDG regression or
  performance qualification is inferred. Ordinary `./DevTool configure` restored
  application tests to OFF after execution. Earlier blocked-run evidence below
  is retained as diagnostic history.
- Final default `MacOS-arm64-Debug-DurinEditor`: `./DevTool test affected`
  passed all 22 selected targets, including `RenderContractTests` (127 cases),
  `RendererSceneContractTests`, `EditorRenderingTests`,
  `VolumetricCloudSceneContractTests`, and texture coverage added by the
  prerequisite fixture repair. Evidence:
  `Build/.agent-state/logs/20260907-032706-955457-26830-ctest.log`.
- Full default `./DevTool build` passed. The application lane was then enabled
  temporarily with `./DevTool configure -DDURIN_ENABLE_APPLICATION_TESTS=ON`.
- `VulkanRHIIntegrationTests` and `EditorGridVulkanTests` both built successfully.
  Enabling these normally excluded targets exposed two pre-existing fixture
  compile errors. `VulkanTextureSamplingTests.cpp` now consumes the current
  structured texture build result; `EditorGridVulkanTests.cpp` uses an explicit
  existing fixture camera direction instead of an undeclared array. These are
  prerequisite fixture repairs, not RDG runtime failures.
- `./DevTool test VulkanRHIIntegrationTests 'FVulkanResourceTransitionTests.*'
  --mode qualification` failed during CTest discovery before GPU tests ran.
  LaunchServices returned `NSOSStatusErrorDomain -10827 / kLSNoExecutableErr`;
  the application host binary exists. The subsequent system-log diagnosis
  identifies the direct launch blocker: at 03:20:09 CST, `open(22269)` was
  denied `mach-lookup` for `com.apple.CoreServices.coreservicesd`,
  `com.apple.lsd.mapdb`, and `com.apple.lsd.modifydb`. The controller
  (`22257`) invokes `/usr/bin/open -n -W` and its child inherits the sandbox.
  The missing-executable message therefore does not establish a missing file;
  LaunchServices service access was denied before the host started. Evidence:
  `Build/.agent-state/logs/20260907-032008-811525-22239-ctest.log` and
  `Build/.agent-state/logs/20260907-032009-rdg-launch-sandbox.log`.
  Read-only checks also found that the host carries only the linker-generated
  ad-hoc signature (`Info.plist=not bound`, `Sealed Resources=none`), and
  bundle signature verification fails. That is a separate packaging finding;
  its effect after LaunchServices access is restored has not been tested.
  No backend test or timing result is claimed. The transition fixture now
  includes rejected repeat recording and a new builder importing the actual
  previous final buffer access, but its GPU execution remains unqualified.
- Resume Stage 3 in an authorized macOS application lane (or an appropriate
  backend host), run Vulkan transition qualification and the affected renderer
  GPU output fixtures, and inspect real validation results. Do not mark this
  plan Completed until that gate passes. Default configuration was restored
  with `./DevTool configure` after the application attempt.

## Validation and Handoff

Follow [agent build/run guidance](../Agents/BuildAndRun.md) before configuring,
building, or running targets and [agent testing guidance](../Agents/Testing.md)
before selecting native tests. Stage 0 must resolve exact target names through
those workflows rather than inventing commands. The expected coverage areas
are RenderGraphTests, RendererSceneContractTests and affected renderer output
fixtures, and VulkanResourceTransitionTests. No additional build may overlap
an existing build process tree in this checkout.

Follow [documentation validation](../Agents/Documentation.md) for document
changes. Keep source/build work single-writer. Each implementation handoff
records passed checks, remaining gates, and exact Plan/Stage commit trailers
under the repository handoff rules. This plan's creation does not count as
completion of an implementation stage.

## Related Code and Contracts

- [RDG public API](../../Engine/Source/Runtime/RenderCore/Public/RDG.h)
- [RDG implementation](../../Engine/Source/Runtime/RenderCore/Private/RDG.cpp)
- [Scene graph execution](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPipeline.cpp)
- [Scene graph capture](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRendererProfiling.cpp)
- [RDG tests](../../Engine/Tests/Native/RenderCoreTests/Private/RDGTests.cpp)
- [Vulkan transition tests](../../Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp)
- [Render Graph contract](../Runtime/Rendering/RenderGraph.md)
- [Renderer frame preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)

UE reference: Epic's [FRDGBuilder API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBuilder)
describes compilation and execution inside Execute; its
[user validation API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGUserValidation)
documents execution-state tracking and shipping-build removal of validation.
This plan deliberately retains recoverable error reporting and lifecycle guards
in release builds rather than copying that validation policy.
