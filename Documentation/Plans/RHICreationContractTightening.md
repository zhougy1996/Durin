# RHI Creation Contract Tightening Plan

Summary: Align RHI creation with UE: keep allocation/device failures terminal, allow pipeline compilation failure, and preserve asynchronous scheduling and asset errors.

Last reviewed: 2026-10-31

Status: Active
Completed:

## Current Status

User-directed revision after review of `8845f1739`: Stage 5 supersedes the
overly strict portions of Stages 0-3. UE's public graphics PSO contract permits
null on compilation failure and permits backend-specific fatal behavior. A
normal pipeline compilation failure must therefore remain an accepted-work
Failed result, not be confused with rejection, OOM or device loss. Ordinary
buffer/texture allocation remains required-success. Cache retention and optional
precache admission must not impose fatal limits on synchronous required work.
Returned fatal errors must reach existing process crash reporting; native OOM
retains the immediate-exit fallback. The historical decisions below describe
the previous implementation and are superseded by this revision where noted.

Stages 0-3 are implemented. Windows RHI/backend creation, Renderer preparation,
asset/editor consumers and integration scenarios have passing validation.
Metal source is migrated; Stage 4 remains open for macOS compilation/runtime
and terminal-failure evidence. This Windows host cannot close those gates.
Stage 5 is implemented and validated on Windows. Previous Stage 4 receipts apply
only to their recorded earlier inputs; revised host evidence is recorded below.
Do not treat existing nullable factories or their tests as evidence that a
recoverable native-creation contract is required.

Initial review found nullable Vulkan texture, buffer, shader, sampler, view and
pipeline creation; Renderer default/fallback textures and fullscreen geometry
also use fallible resource slots. RDG rolls back physical allocation failures,
and generic slots allow repeated native creation attempts. Texture initialization
catches all exceptions and converts them into fallback. These are migration
targets, not supported recovery requirements.

Stage 0 decisions and inventory (2026-10-31, base `74d62a83d`):

- Searched Engine source/tests and Sandbox/RoadWeaver source roots declared by
  the workspace; raw matches are in `Build/RHICreationContract/inventory.txt`.
  Direct native-failure machinery belongs to RHI, VulkanRHI, MetalRHI, RenderCore,
  Renderer, Engine and editor consumers. Project consumers need compilation even
  where no direct native-error handling exists.
- Delete ordinary factory nulls, Vulkan recoverable-result translation, native
  Failed publication/retry, texture catch-all fallback and RDG physical-failure
  retry. Retain asset/build/compiler errors, cache misses, capability queries,
  task cancellation/closure and explicit pre-admission metadata limits. Per user clarification, supported GPU timing query creation also requires
  success: grow storage rather than reject at an arbitrary slot ceiling. Unavailable
  swapchain drawables and unsupported scheduling are not native factory failures.
- Core's platform-misc interface has no suitable guaranteed immediate exit.
  Returned native failures use a small exported RHI terminal helper: stack-buffer diagnostic, direct OS
  stderr write, then Windows `TerminateProcess` or POSIX `std::_Exit(EXIT_FAILURE)`. No logger, teardown or scheduler
  is required. Backends supply borrowed operation/reason strings and native code.
  Use GoogleTest subprocess death/exit assertions already supported by RHITests;
  cover the common path without Core and injected creator null/throw/OOM paths.
- Explicit request/description budget reservations retain CapacityExceeded.
  Actual `std::bad_alloc` terminates rather than becoming budget pressure.
  Native cache retention limits remain bounded: existing eviction/reservation is
  preserved; exhaustion with all entries pinned is a terminal internal capacity
  configuration failure, not a retryable creation result. This deliberately
  narrows the earlier pre-admission-only capacity rule for backend-owned native
  caches, whose dependencies are acquired during native creation. No unbounded
  cache growth or false OOM classification is introduced.
- Validation targets: RHIPipelineCreationTests, RHIResourceViewValidationTests,
  RHICommandListTests, RenderContractTests,
  VulkanRHIIntegrationTests and affected texture/mesh/reload/thumbnail suites.
  Inspect affected selection after migration. Final all build covers shared
  declarations and workspace consumers. Metal runtime evidence requires macOS;
  this Windows host cannot satisfy that acceptance gate.

## Goal

Make ordinary native resource creation a required-success operation. Keep GPU
fault policy in RHI and its backend, rather than in each texture, mesh, pass or
editor consumer. OOM terminates the process after best-effort diagnostics.
Keep recoverable source/content errors and genuine asynchronous progress under
their existing owners.

## Selected Contract

### Native creation and terminal failures

Ordinary buffer/texture/shader/view factories require success. Graphics/compute
PSO factories may return null for recognized compilation failure, following
[UE's public contract](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FDynamicRHI/RHICreateGraphicsPipelineState).
Accepted asynchronous work publishes Ready, Failed or Canceled. Failed means
compilation rejection, not OOM/device loss. Existing resource slots retain
generation-scoped suppression without per-frame retry.

Native factories and the async creator remain `noexcept` to keep unexpected
exceptions terminal. Returned fatal errors use existing process crash reporting
after bounded diagnostics; real OOM retains immediate nonzero exit without
teardown. No backend installs a process-global terminate handler.

Synchronous PSO creation bypasses optional async admission. Cache entry retention
does not limit live resources: release cache references while preserving external
owners. Interned raw descriptor/render-pass handles may grow until device teardown.
Optional request/description budgets still reject before admission; native CPU
metadata accounting does not turn their thresholds into fatal creation limits.

GPU timing queries follow the same creation contract on supported devices. Grow
query storage when all live slots are occupied; native allocation failure exits.
Timestamp capability checks and Pending/invalid measurement results remain separate.

### Retained asynchronous and asset behavior

| Operation or condition | Retained behavior |
| --- | --- |
| PSO work accepted but unfinished | Pending, completion dependencies, priority and first-consumer ordering |
| Request admission pressure, closure, cancellation or unsupported scheduling mode | Explicit rejection/cancellation; bounded deferred admission where already supported |
| Invalid external request before native creation | Validation error at the owning input boundary; do not turn malformed assets into native calls |
| Shader source/material compilation, reflection or content validation | Owned diagnostic; reject candidate, retain accepted content or select the defined ErrorMaterial |
| Asset I/O, decoding, DDC/build failure or stale candidate | Existing asset result, cancellation and publication rules |
| Unsupported feature/format determined before creation | Explicit capability or permutation selection; choose a supported path before allocation |
| Feature disabled or absent optional input | Existing feature policy and neutral bindings |
| Ordinary resource creation after validation | Required success; allocation/device failures terminate |
| Recognized native PSO compilation failure | Null synchronous result or accepted Failed; no per-frame retry |

An optional feature is not permission to swallow OOM/device faults. A failed PSO
compilation is a separate, backend-recognized outcome. Keep explicit input,
content, capability and generation ownership; never expose half-created objects.


## Scope and UE Evidence

Owners include RHI, VulkanRHI, MetalRHI, RenderCore, Renderer, Engine and editor
preview/thumbnail consumers. Search every source/test root declared by
`Durin.dworkspace`, including Engine, Sandbox and RoadWeaver, when shared APIs or
contracts change. Preserve GPU-aware retirement and render-resource ownership.

UE evidence retrieved on 2026-10-31:

- [Epic's D3D12 OOM explanation](https://forums.unrealengine.com/t/renderthread-c-exception-on-pre-rtx-dxr-gpus-after-r-raytracing-usereferencebasedresidency-0/2762839)
  describes UE 5.7 routing OOM through TerminateOnOutOfMemory and process exit,
  rather than asking asset callers to recover.
- [UE GPU crash guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/dealing-with-a-gpu-crash-when-using-unreal-engine)
  notes that OOM behavior depends on RHI. This plan deliberately selects process
  exit for Durin's supported backends; it does not claim every UE version/backend
  has identical failure handling.
- [UE PSO precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine)
  describes delayed proxy creation, skipped draws or default materials while PSOs
  are still compiling. Retain that asynchronous distinction without treating it
  as evidence for recovery from native creation failure.

No complete local UE source checkout was available during planning. Pin any later
source comparison to its exact version/revision and paths; do not copy UE source.

Coordinate overlapping readiness work with
[Metal load preparation](MetalCookedLibrariesAndLoadPreparation.md) and
[startup/scene activation](RuntimeStartupAndSceneActivation.md). Their asynchronous
progress and asset diagnostics remain valid, but must not reintroduce native OOM
recovery. Broader device recovery, scheduler redesign, allocator tuning and
render-resource reinitialization redesign are outside this plan.

## Implementation Stages

### Stage 0: Define Contract and Migration Inventory

Dependencies: none.

- [x] Record required-success creation, terminal OOM exit, and retained error boundaries.
- [x] Inventory factories, backend result handling, sync/async bridges, nullable
  consumers, catches, retry commands, fault-injection tests and documentation
  across all workspace projects. Keep raw inventory under ignored `Build/`.
- [x] Map each existing Failed/fallback/retry branch to native creation, asset
  error, scheduling pressure or feature selection; record deletions and survivors.
- [x] Identify the low-allocation terminal reporter/exit primitive and the
  subprocess test harness. Include startup without Core and background creators.
- [x] Resolve backend cache-capacity admission and real host-OOM handling before
  changing factory contracts. Select owning/consumer test targets from actual code.

Completion: inventory, terminal mechanism and validation selection are recorded
above. The GPU timing pool clarification is incorporated: supported creation
grows storage instead of using optional-admission nulls.

### Stage 1: Enforce RHI and Backend Creation Contracts

Dependencies: Stage 0.

- [x] Implement the common terminal boundary, including OOM diagnostics and
  nonzero exit, using existing platform facilities where possible.
- [x] Migrate Vulkan and Metal texture/buffer/shader/sampler/view/declaration and
  graphics/compute pipeline factories; cover aliased/transient creation and native
  dependencies. Remove null returns that represent native failure.
- [x] Document normal-return non-null guarantees in RHI headers. Keep capability
  checks, input validation and scheduling support separate from native creation.
- [x] Route async native failures to the same terminal boundary. Preserve Pending,
  completion edges, cancellation and admission rejection; remove native-failure
  result plumbing where it no longer describes a reachable nonterminal outcome.
- [x] Migrate cache capacity and exception translation so logical budget pressure
  remains pre-creation admission and actual OOM cannot become CapacityExceeded.
- [x] Add subprocess coverage for deterministic injected native failures and OOM
  in sync and background paths; test real handling boundaries without exhausting
  the host GPU or killing the test runner.

Completion: ordinary factories cannot return native-failure nulls, and sync/async
fault probes terminate with the expected diagnostic and nonzero exit.

### Stage 2: Simplify RenderCore and Renderer Preparation

Dependencies: Stage 1.

- [x] Remove generic ShaderCreationFailed/ResourceCreationFailed/native pipeline
  and sampler failure propagation, diagnostics fingerprints and retry machinery
  that exist only for nullable native factories. Preserve shader-content errors.
- [x] Simplify resource slots to the states and transitions still needed for
  demand, asynchronous progress, accepted content and invalidation. Do not blindly
  delete Failed where asset/shader preparation can still fail.
- [x] Remove native-failure fallbacks from default textures, fullscreen geometry,
  samplers/views, fixed renderers, mesh rendering and editor assistance.
- [x] Remove RDG physical-allocation-failure retry/recovery plumbing. Preserve
  graph validation, planning-budget rejection, single-use builders, reservation
  consistency and ordinary successful allocation/reuse/aliasing behavior.
- [x] Remove native retry behavior from `renderer.retry-resources`; retain or
  narrow the command only if an explicit shader/asset retry responsibility remains.
- [x] Keep deliberate feature selection, neutral resources and PSO Pending draw
  policies. Audit retained skip branches for accidental masking of fatal failures.

Completion: selected native resources are assumed valid; asynchronous preparation
and content errors retain explicit outcomes without generic native retry loops.

### Stage 3: Migrate Asset and Editor Consumers

Dependencies: Stage 2.

- [x] Remove texture catch-all conversion to fallback and native-upload failure
  queries/branches made obsolete by required-success creation.
- [x] Simplify StaticMesh readiness/failure reporting and selected-object waits
  without merging CPU residency, queued initialization and successful publication.
- [x] Migrate package reload, material/texture/mesh previews, thumbnails and other
  workspace consumers. Preserve source/build/I/O failure, stale result rejection
  and old-version retention at their asset transaction boundaries.
- [x] Keep render-command fences distinct from GPU completion; preserve retained
  input lifetime, stable references and ordered resource retirement.
- [x] Update tests to assert retained asset and asynchronous semantics. Replace
  native-OOM fallback/retry tests with subprocess terminal tests; do not merely
  delete failure coverage or turn native failures into asset errors.

Completion: no asset/editor caller classifies native errors, falls back after
native creation failure or schedules a retry of a failed native allocation.

### Stage 4: Validate Integration and Publish Lasting Contracts

Dependencies: Stages 1-3.

- [x] Run selected Windows RHI/backend/RenderCore/Renderer and affected asset/editor tests,
  including Pending-to-ready, admission pressure, cancellation, invalid assets,
  failed compilation and accepted-version retention.
- [x] Verify Windows terminal native failure/OOM handling, including startup
  without task scheduling, native Vulkan results and background PSO creation.
  Record diagnostic, exit status and bounded completion evidence.
- [x] Run a final `all` build on the supported Windows host configuration because shared
  RHI headers/contracts and removed state/error types affect cross-project consumers.
- [ ] Qualify Metal compilation, normal execution and terminal native failure/OOM
  on macOS. This host has no macOS toolchain or Metal device; source migration
  and Windows builds do not substitute for that evidence.
- [x] Smoke-test Windows editor startup, scene activation, normal rendering, thumbnails,
  shader/material editing and package reload; use the Stage 0 selected scenarios.
- [x] Update authoritative RHI creation/execution, Renderer recovery/preparation,
  texture/mesh and material contracts. Remove contradictory native-recovery claims
  and rename/restructure documents only when their remaining responsibility needs it.
- [x] Validate changed documents and all plan lifecycle metadata. Record final
  tested inputs, result applicability, backend coverage and remaining limitations.

Completion: all required gates have evidence; long-lived documents describe the
implemented policy and the plan can be marked Completed.

### Stage 5: Align UE Creation Semantics

Dependencies: implemented Stages 0-3; macOS acceptance remains open in Stage 4.

- [x] Allow backend-recognized PSO compilation failure and publish accepted Failed
  results; retain deterministic failure suppression until content invalidation.
- [x] Remove optional async admission from synchronous PSO creation and make native
  cache retention independent of live-object ownership.
- [x] Unify returned fatal errors with process crash reporting, retain immediate
  OOM exit, and close cached-view exception boundaries.
- [x] Validate pipeline failure/ready/cancellation, cache pressure and normal GPU
  rendering; build all for shared RHI consumer coverage and update lasting contracts.
- [x] Record Windows evidence; explicitly retain unavailable macOS validation.

Completion: revised contracts and consumers pass host verification; platform
limitations remain recorded without claiming identical behavior on every UE RHI.

## Validation and Handoff

Follow [build selection](../Agents/BuildAndRun.md),
[test selection and result reuse](../Agents/Testing.md) and
[documentation validation](../Agents/Documentation.md). Do not run concurrent
build trees in the same checkout. Reuse passing results when relevant inputs and
environment remain unchanged; the final broad build is an explicit acceptance
gate, not a requirement after every stage.

Each implementation commit updates this plan's evidence-backed status and uses
`Plan: Documentation/Plans/RHICreationContractTightening.md` and the exact owning
stage title as Git trailers. Store generated reports and fault-probe logs under
ignored `Build/` or CI artifacts, with concise evidence summaries in this plan.

Windows evidence (2026-10-31, implementation based on `78d7751d7`):

- Profile `windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, MSVC; NVIDIA
  GeForce RTX 3090, Vulkan 1.4.351. Builds/tests ran outside the sandbox after
  its MSVC PDB service failed with C1902. The final `build all` passed; receipt
  `Build/.agent-state/logs/20261031-213008-779991-35888-cmake.log` covers final
  production code and shared API consumers. No clean-build claim is made.
- Reports under `Build/RHICreationContract/`: `renderer-final.xml` (18 targets),
  `texture.xml` (4 targets), `static-mesh.xml` (6 targets), `material.xml`
  (12 feature targets), `asset-reload.xml` (18 cases),
  `asset-reload-vulkan.xml` (1 qualification target) and `thumbnail.xml`
  (8 cases) pass. Renderer coverage includes RHI pipeline creation (19 cases),
  command lists (145), RenderContract (244), native Vulkan integration, RDG,
  resource reload, scene contracts and normal cloud presentation.
- Subprocess assertions cover the RHI terminal helper without Core/scheduler,
  returned Vulkan OOM/device-loss codes, invalid native descriptions, injected
  null pipeline creation, thrown `bad_alloc` and unexpected creator exceptions.
  Returned-error assertions require exit code 1 and the operation/reason/native
  code diagnostic; thrown-exception assertions require process death through
  `noexcept`. The 19-case pipeline suite completed in about two seconds;
  native-error probes complete within the bounded native-test execution.
- Editor startup/first presentation/three ticks/normal shutdown passed via
  `run --project Sandbox/Sandbox.dproject -- --exit-after-ticks=3` (2.35 seconds),
  receipt `Build/.agent-state/logs/20261031-212947-742539-77500-DurinEditor.log`.
  Scene activation, shader/material editing and package reload are covered by
  automated lifecycle/integration tests, not manual UI editing.
- Corrected standalone drivers: pending cloud presentation returns acquired
  swapchain images; thumbnail polling advances frames so retained per-frame
  preparation budgets refresh. Replaced the failing receipts with passing
  reruns rather than changing Pending into success or increasing deadlines.
- Affected discovery expands across shared test headers and backend sources;
  named owner domains plus explicit reload/thumbnail integration bound runtime
  coverage. The final all build covers cross-project compilation. Earlier
  texture/mesh/material receipts remain applicable: their inputs, toolchain,
  packages and settings are unchanged; later production changes only remove
  swallowed preparation OOM and a native-target null branch, with renderer and
  thumbnail regression rerun. macOS acceptance remains outstanding.


Stage 5 Windows evidence (2026-10-31, based on `8845f1739`):

- Revised PSO creation follows UE's documented nullable compilation result,
  not a universal recoverable native-error interface. Vulkan recognizes shader
  rejection; Metal recognizes documented compiler error domains/codes. No
  error-message matching, arbitrary driver-error fallback or per-frame retry.
- `ue-pipeline.xml`: 21 cases pass, including retained Failed identities,
  cancellation, unexpected exceptions and a returned-error probe that reaches
  an installed terminate handler. The explicit OOM checker still requires immediate exit 1; allocator exceptions
  crossing `noexcept` use the existing process terminate policy.
- `ue-vulkan.xml`: 6 focused cases pass. `ue-cache-final.xml`: 4 final cache
  cases pass, including synchronous creation while optional 64 MiB admission is
  exhausted and eviction while all PSOs/layouts have external references.
  Native metadata is accounted separately from optional request budgets.
- `ue-renderer.xml`: all 18 owner-domain targets pass, including full Vulkan
  integration, RHI/RenderCore contracts, RDG, reload and normal presentation.
  `ue-thumbnail.xml`: all 8 cases pass. `ue-material.xml`: all 12 feature
  targets pass, including compilation, editing, runtime and thumbnail lifecycles.
  Reports are under `Build/RHICreationContract/`.
- Final Windows `build all` passed (48.10 seconds), receipt
  `Build/.agent-state/logs/20261031-221559-163086-63464-cmake.log`, covering final
  RHI declarations and Engine/Sandbox/RoadWeaver consumers. The final preparation
  diagnostic change also passes all 244 cases in `ue-render-contract-final.xml`.
  No clean-build claim is made.
- Editor Sandbox startup/three ticks/normal shutdown passed (2.34 seconds),
  receipt `Build/.agent-state/logs/20261031-221339-761486-55080-DurinEditor.log`.
  Profile, device and sandbox workaround match the recorded Windows environment.
- Changed docs and all-plan metadata validate. Affected discovery expands to
  `all` for backend platform sources; bounded owner-domain/integration selection
  covers changed runtime behavior and the all build covers shared consumers.
  Prior texture/mesh results remain applicable; this revision does not change
  their asset inputs, ordinary allocation or publication implementation.
- Metal compilation/runtime remains unverified on this Windows host. Stage 4's
  macOS gate stays open; public UE contract alignment does not claim identical
  failure handling across every UE version/RHI.
