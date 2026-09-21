# Rendering Structured Diagnostics Plan

Summary: Complete structured error propagation through renderer resource creation and RDG, building on the migrated Material, RHI, and Shader contracts.

Last reviewed: 2026-09-17

Status: Archived
Completed: 2026-09-17

## Current Status

Shader migration is complete. Stage 1 is complete:
scoped result forwarding replaces assignment-heavy propagation; code-only and
filesystem factories remove repeated wrappers; source capture retains the failing
entry and typed limit kind/maximum/actual with named budgets. The payload audit
retains existing reflection, provider, compiler, file, and identity fields because
they carry distinct actionable context. Capture limits no longer overload the
generic expected/actual pair. No universal result framework was introduced.

Stage 1 validation (macOS arm64 Debug, 2026-09-17): workspace `all` passed
(`20260917-213559-556043-42409-cmake.log`); affected selection passed 6/6:
`RenderContractTests`, `RenderShaderCacheTests`, `RenderShaderContractTests`,
`RenderShaderCookIntegrationTests`, `RenderShaderCookedLibraryTests`, and
`RenderShaderServiceTests` (`20260917-213543-652690-41880-ctest.log`).
New tests cover owned filesystem/native context, capture-limit formatting, and
canceled capture clearing previously published output. Changed-document validation
passed (2 files); all-plan validation passed (7 active, 8 completed, 363 archived).
GPU execution is not part of this construction-only stage.
Stage 1 implementation commit: `f7d261ee1`.

Stage 2 is complete. Resource failures now own Shader/RHI causes, semantic
fingerprints exclude external wording, and asynchronous pipeline scopes preserve
native creation failures. Material binding errors format only at their logging
boundary. Fullscreen geometry and invalidation test aggregates were included in
the migration audit in addition to named error-helper consumers.

Stage 2 validation (macOS arm64 Debug): `all` passed
(`20260917-214544-989599-46003-cmake.log`); affected selection passed 23/23
(`20260917-214425-187673-44877-ctest.log`), covering EditorAssetWorkflowTests,
EditorRenderingTests, MaterialCompileLifecycleTests, MaterialCompilerTests,
MaterialCookTests, MaterialEditingPersistenceTests, MaterialEditorInteractionTests,
MaterialFunctionTests, MaterialGraphEditingTests, MaterialPackageTests,
MaterialRuntimeTests, RenderContractTests, RenderShaderCacheTests,
RenderShaderContractTests, RenderShaderCookIntegrationTests,
RenderShaderCookedLibraryTests, RenderShaderServiceTests,
RendererSceneContractTests, SkyBoxTests, TextureImportWorkflowTests, TextureTests,
TextureThumbnailTests, and VolumetricCloudSceneContractTests. Added native-cause
propagation coverage passed with all 175 RenderContractTests cases
(`20260917-214520-700188-45936-RenderContractTests.log`). Existing tests cover
unchanged-generation suppression, retries, fallback, and recovery; new tests
cover wording-independent identity and owned nested context after slot reset.
Changed-document validation passed (2 files). Optional GPU execution was not
run in Stage 2; Stage 3 retains the explicit Vulkan integration gate.
Stage 2 implementation commit: `f6bacf1f6`.

Stage 3 implementation commit: `53d6fd41c`.
Stage 3 is complete, including relevant Vulkan integration acceptance.
RDG categories remain the success authority, with specific reasons and owned
metadata/use/dependency/limit/contract/allocation contexts. All allocator
implementations forward typed results; Renderer retains the native creation
cause across suppressed retries and publishes allocation output transactionally.
Optional Vulkan consumers (including memory policy, texture sampling, and
editor-grid assertions) were included by compiling their normally excluded
targets. Tests cover graph destruction, retained native causes, structural
limits, invalid declarations, and allocation failure/cleanup.

Stage 3 validation (macOS arm64 Debug): final workspace `all` passed
(`20260917-220813-681588-51484-cmake.log`); Renderer/RHI contracts passed
10/10 (`20260917-220103-663979-50487-ctest.log`). Final affected selection
expanded to all routine targets and passed 90/90
(`20260917-220915-905691-51583-ctest.log`). Changed-document validation passed
(3 files), and all-plan validation passed (7 active, 8 completed, 363 archived).
`VulkanRHIIntegrationTests` and `EditorGridVulkanTests` compiled successfully
with application tests explicitly enabled (`20260917-220727-420099-51288-cmake.log`
and `20260917-220745-008542-51328-cmake.log`). The default application-test OFF
configuration was restored afterwards.

GPU receipts and coverage limits:

- Unsandboxed `RendererResourceReloadVulkanTests` initialized Apple M4,
  MoltenVK API 1.3.334 / driver `0x28a1`. The broken-refresh/reload recovery
  case passed. The whole selection failed because
  `AsyncPoolReuseWaitsForTheRecordedTerminalJoin` requires independent compute,
  while this device exposes shared graphics/compute/transfer queues
  (`20260917-220119-615304-50777-ctest.log`). Async coverage is not passed.
- The initial application-host admission attempt timed out while awaiting the
  user's system approval (`20260917-220507-099121-50997-ctest.log`). After the
  user confirmed approval, NativeTestApplicationExecutionTests passed
  (`20260917-222611-234659-54576-ctest.log`). The old timeout is superseded;
  it is not an unresolved host defect.
- `VulkanRHIIntegrationTests` filter `FVulkanResourceTransitionTests.*` passed
  8/8 on Apple M4 (`20260917-222654-218056-54644-ctest.log`), covering actual
  buffer/texture transitions, failed submitted-prefix lifetime without extraction,
  rejected allocation, and successful graph replay. One remaining legacy text
  assertion was corrected to retain and check `ERDGError::AllocationFailed` and
  `ERDGReason::AllocatorFailure` after the allocator scope ends.
- The same target's `NativeResourceFactoriesRunOutsideReplayAndRollbackPublication`
  and `RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread` cases passed 2/2
  (`20260917-222730-673277-54718-ctest.log`), covering native creation rollback
  and recovery. RendererResourceReloadVulkanTests' filtered
  `BrokenRefreshRetainsPipelineAndChangedReloadRecoversInProcess` also passed
  (`20260917-222759-474969-54765-ctest.log`).
- Gate-selection correction: the original Stage 3 requirement is relevant Vulkan
  integration, not every queue-topology qualification. The earlier status
  incorrectly made the extra independent-compute case a completion prerequisite.
  Required changed-path coverage is now satisfied by the graph/allocator failure,
  lifetime, native rollback, and renderer recovery cases above, together with the
  CPU contracts. Queue scheduling/submission behavior was not redesigned. The
  independent-compute case remains unavailable on this device and is not marked
  passed; no test, initialization check, or capability requirement was weakened.
  The default application-test OFF configuration was restored after validation.

Stage 4 is complete (initial audit commit `b12dfdde1`, followed by this acceptance
closure). Final closure validation passed for the changed document and all plans
(6 active, 9 completed, 363 archived). All Engine, Sandbox, and RoadWeaver source/test roots declared by
`Durin.dworkspace` were searched, including normally excluded Vulkan consumers.
No old RDG `Message`, allocator string output, or coarse TryCreate failure-output
signature remains. Retained `Message`/`OutError` fields belong to asset/Cook,
editor/property/actor construction, RoadWeaver domain validation, console
presentation, Core modular-feature retirement, and native application-host
protocols. They are not rendering failure transport. Graph capture dependency
causes and allocation observation tags remain identity/observation data.
Shader compiler text, Core fingerprint text, and third-party exceptions remain
bounded external adapters, never success or deduplication authorities.

Representative coverage verifies Shader-to-resource ownership after producer
reset, native RHI-to-Renderer-allocator-to-RDG causes and suppressed retries,
and final nested formatter composition. Lasting contracts are in ShaderDiagnostics,
RHIDiagnostics, RendererResourceRecovery, and RenderGraph. The final `all` and
90/90 regression receipts above apply to the unchanged runtime implementation;
Stage 4 adds only presentation-boundary regression coverage and documentation.
Final RenderContractTests passed 179/179
(`20260917-221102-102910-54123-RenderContractTests.log`). Changed-document
validation passed (2 files); all-plan validation passed (7 active, 8 completed,
363 archived).

The working baseline contains:

- `6a42c408a`: structured ShaderMap, material shader map, reflection, and payload errors.
- `b4bb1e8d3`: typed compiler, provider, cooked request, and library failures,
  with shared consumers and tests migrated.
- Validation on 2026-09-17: workspace `all` build passed; 58/58 affected
  test targets and 6/6 shader-domain targets passed; documentation validation passed.
  Logs under `Build/.agent-state/logs`:
  `20260917-204324-408502-5132-cmake.log`,
  `20260917-204130-264308-28036-ctest.log`, and
  `20260917-204359-414393-33328-ctest.log`.

Verified remaining seams:

| Owner | Current transport | Required change |
| --- | --- | --- |
| RenderCore `RenderResourceCreation.h` | `FRenderResourceCreateError.Message`; `GetFingerprint()` hashes text | Structured causes and semantic fingerprinting |
| Renderer resource producers and `RendererResourceDiagnostics` | Helpers accept prose and reporters read `Message` | Preserve causes until logging or UI presentation |
| RenderCore `RDG.h` / `RDG.cpp` | `FRDGResult` combines `ERDGError` with `Message` | Structured reasons and graph context under the existing result semantics |
| `FRDGAllocator` / `RendererRDGAllocator` | `Allocate` returns `bool` with `std::string& OutError` | Typed allocation results with lower-layer causes |

## Resource Failure Mapping

Stage 2 inventory covers Engine, Sandbox, and RoadWeaver source/test roots;
resource producers are in RenderCore, Renderer, and TextureEditor, with Engine
and RenderCore test consumers. No Sandbox/RoadWeaver resource-error owner exists.

| Failure sites | Reason and owned context | Cause | Retry / fingerprint |
| --- | --- | --- | --- |
| GlobalShader compilation, map binding; mesh material adapters; preview compilation | ShaderFailure; owner and exact set/material identity | `FShaderError` | Existing shader/manual masks; code and semantic shader context |
| GlobalShader null/missing type | ShaderFailure; type name and set identity | NullShaderType/MissingShaderType | Existing manual or shader/manual masks |
| Nullable shader, sampler, buffer, texture, view, geometry batches | ResourceCreationFailed / ShaderCreationFailed / SamplerCreationFailed / FullscreenGeometryUnavailable, retaining owner and identity | BackendReturnedNull where nullable APIs provide no richer result | Preserve site-specific masks; reason, identity, cause |
| Graphics/compute pipeline slots | PipelineCreationFailed | Preserve asynchronous `FRHICreationError`; nullable fallback records BackendReturnedNull | Existing shader/device/manual masks |
| Global-set wrapper failures | GlobalShaderUnavailable | Original failure is reported by GlobalShader owner | Preserve suppression and shader/manual retry |
| Material binding diagnostics | Existing Material code/layout identity at immediate logging boundary | `FMaterialError` stays in Engine/Renderer; no reverse RenderCore dependency | Existing material diagnostic gating |
| Test injections | Explicit ShaderFailure/PipelineCreationFailed | Typed injected Shader/RHI causes | Same slot semantics as production |

Resource fingerprints select category, reason, owned owner/identity, nested code
and semantic context, retry mask, and fallback state; exclude generation and
external prose. Mutually exclusive Shader/RHI causes use `std::variant`.

## RDG Failure Mapping

Stage 3 inventories all 74 RDG construction sites, result consumers in renderer
presentation and tests, and the Renderer plus three test allocator implementations.
The existing `ERDGError` remains the sole success/category discriminator. A
specific `ERDGReason` identifies each invariant; `FRDGResult` owns a context
variant rather than formatted prose:

| Sites | Context alternative |
| --- | --- |
| Parameter metadata/layout/binding validation | Struct/member/binding names, member index, size/alignment/offset, nesting |
| Pass uses, producer coverage, overlapping declarations | Pass/resource/parameter names and indices, access, byte/subresource ranges, conflicting use |
| Resource/pass identities, typed values, extraction | Owned names/type identities, declaration indices, expected/actual values |
| Dependency validation | Producer and consumer indices |
| Structural limits | Stable dimension name, actual and limit |
| Conflicting external imports | Both owned names and copied resource descriptions/access contracts |
| Allocation/preparation | Resource ID and expected/actual descriptors where applicable; typed Resource or RHI cause |
| Builder/compilation/preparation/recording state | Specific reason, no synthetic context |

`FRDGAllocator::Allocate` returns `FRDGResult`; allocation failures use the
existing AllocationFailed category and preserve cause/context through graph
preparation. Rollback, retry and retained-resource publication remain unchanged.
Inspection found `RHITryCreateTexture/Buffer` discard native RHI context into an
enum output. Their output is migrated to `FRHICreationError` in the same stage,
including Vulkan and test implementations, to satisfy the selected cross-layer
cause-preservation gate. This extends the typed RHI contract without changing
creation or retry behavior.

## Goal

Engine-owned rendering failures carry stable codes and owned context through
validation, allocation, creation, and recovery. Logs, assertions, console output,
and editor presentation format text at their boundaries. Callers and tests can
classify failures without parsing English diagnostics.

## Scope and Selected Decisions

- Keep domain-specific errors. Reuse the existing Material, RHI, and Shader
  contracts rather than introducing a universal error hierarchy.
- Typed errors must also simplify their call sites. Use small named construction
  helpers for recurring shapes, direct result forwarding, and ordinary control
  flow. Do not introduce propagation macros, generic builders, implicit success
  conversions, or a new result framework merely to shorten syntax.
- Preserve `FShaderError` and `FRHICreationError` as typed nested causes where
  resource creation or RDG allocation crosses those domains. Use explicit
  alternatives for mutually exclusive causes; avoid preformatted cause strings.
- Extend existing resource categories/reasons and RDG categories as needed.
  Success has one authoritative representation, independent of text or payload.
- Retain useful identities, pass/resource/parameter indices, ranges, expected
  and actual values. Stored diagnostics must own data that outlives temporary
  builders, allocator attempts, and compiler outputs.
- Preserve bounded opaque external diagnostics only at existing external
  adapters. They do not determine classification, retry eligibility, or deduplication.
- Resource failure fingerprints use selected semantic fields and nested cause
  identities, excluding formatted prose. Preserve generation gating, retry
  dependencies, retained fallback, failure suppression, and recovery notification.
- Preserve RDG declaration, compilation, scheduling, rollback, resource
  retirement, and GPU submission behavior. This is not a graph execution redesign.
- Keep existing presentation strings where practical, but semantic tests assert
  codes and context. Formatting tests own wording assertions.
- Exclude unrelated asset/Core error APIs, successful console messages, and
  general telemetry. Record any remaining string adapter with its owner and reason.

Required references:
[Shader diagnostics](../../../Runtime/Rendering/ShaderCache.md#results-and-diagnostics),
[RHI diagnostics](../../../Runtime/Rendering/RHICommandExecution.md#results-and-diagnostics),
[Material diagnostics](../../../Runtime/Rendering/MaterialSystem.md#results-and-diagnostics),
[Renderer resource recovery](../../../Runtime/Rendering/RendererResourceRecovery.md),
[Render Graph](../../../Runtime/Rendering/RenderGraph.md),
[build workflow](../../../Agents/BuildAndRun.md),
[test workflow](../../../Agents/Testing.md), and
[documentation workflow](../../../Agents/Documentation.md).

## Implementation Stages

### Stage 0: Establish the Shader baseline

Dependency: completed Material and RHI typed-error migrations.
Outcome: Shader operations supply typed causes for the remaining rendering layers.

- [x] Migrate ShaderMap, reflection, compiled payload, compiler, provider,
  dependency capture, and cooked library results.
- [x] Migrate shared consumers and document presentation boundaries.
- [x] Validate workspace build, affected consumers, and shader-domain tests;
  record commits and evidence in Current Status.

### Stage 1: Simplify Shader error construction and propagation

Dependency: Stage 0. Outcome: typed diagnostics remain useful while error
plumbing stops obscuring normal compilation and source-capture control flow.

The motivating case is `ShaderBuildModule.cpp` source capture: cancellation,
directory limits, and filesystem failures repeatedly expand nested
`FShaderOperationResult` / `FShaderError` initializers. Filesystem status checks
also use the mount root even when the failing operation concerns one entry.
Keep required error checks; simplify construction and improve context accuracy.

- [x] Audit repeated error construction and propagation in ShaderBuild and
  RenderCore Shader code. Distinguish justified detail from wrapper-only syntax,
  overloaded generic fields, unused temporaries, and redundant copies.
- [x] Introduce the smallest domain-local helper set supported by actual reuse:
  code-only failure, filesystem failure with path and native error, and repeated
  limit/context shapes. Prefer local helpers for capture-only cases and shared
  factories only when multiple owners need the same semantics. Do not create
  one factory per enum value or require one-off errors to use a builder.
- [x] Reduce double-brace construction at routine failure sites and forward
  existing typed results directly. Use scoped result variables where inspection
  is needed; remove assignment-heavy propagation and misleading text-era names.
- [x] Audit `FShaderError` payloads against callers, formatting, and tests.
  Retain fields that support classification, recovery, or actionable diagnosis;
  remove accidental duplication. Use meaningful context names for recurring
  shapes rather than growing an unstructured bag of optional fields.
- [x] Replace repeated capture-budget literals with named constants. Preserve
  iterator-construction, increment, metadata-query, and post-loop error checks;
  report the actual failing entry when known without dereferencing an invalid
  iterator. Keep cancellation, limits, symlink policy, and output publication.
- [x] Ensure formatting exposes retained useful context, including relevant
  paths and limits. Preserve bounded external diagnostics and native error codes.
- [x] Add or adapt focused semantic tests where construction or context changes;
  review representative before/after capture and compile paths for readability.
  Pass Shader tests, affected consumers, the workspace `all` build when shared
  APIs change, and documentation validation; record evidence in Current Status.

Completion gate: routine failures read as short, explicit operations; typed
causes and useful context survive unchanged or become more accurate. No hidden
control flow or general error framework is introduced. Apply this construction
style to Stages 2 and 3 instead of repeating the verbose baseline pattern.

### Stage 2: Migrate renderer resource creation and recovery

Dependency: Stage 1. Outcome: resource creation preserves typed causes and
recovery behavior no longer depends on diagnostic wording.

- [x] Inventory `FRenderResourceCreateError`, `MakeRendererResourceCreateError`,
  fingerprinting, and all producers/reporters across Engine, Sandbox, and
  RoadWeaver source and test roots declared by `Durin.dworkspace`.
- [x] Record a mapping of existing failure sites to reason, owned context,
  nested cause, retry dependencies, and fingerprint fields before editing APIs.
- [x] Replace resource `Message` storage and string-producing helpers, including
  GlobalShader and Renderer material/pipeline/resource adapters, with typed data.
- [x] Add one resource formatter and migrate logs, assertions, console/UI
  adapters, and diagnostic callbacks without formatting during propagation.
- [x] Test distinct semantic causes, wording-independent deduplication,
  unchanged-generation suppression, generation-triggered retry, retained
  fallback, recovery notification, and error lifetime after producer teardown.
- [x] Update the resource recovery contract; pass the workspace `all` build,
  selected resource/recovery tests, affected consumer tests, and documentation
  validation. Record exact targets, results, and any skipped GPU coverage.

Completion gate: no engine-owned resource failure requires prose for identity,
classification, or propagation; typed Shader/RHI causes reach presentation intact.

### Stage 3: Migrate RDG validation and allocation results

Dependency: Stage 2. Outcome: RDG validation and allocator failures expose
actionable structured graph context without string error outputs.

- [x] Inventory every `FRDGResult` construction/consumer and allocator
  implementation, including test allocators and graph diagnostic/capture adapters.
- [x] Map existing failure sites to RDG category, specific reason, graph
  identities/indices, ranges, and expected/actual values; record the selected
  payload shape before changing the shared interface.
- [x] Replace `FRDGResult.Message` with structured diagnostics while preserving
  existing success and graph-state semantics.
- [x] Replace `FRDGAllocator::Allocate` and Renderer/test implementations'
  `bool` plus `OutError` transport. Retain typed resource/RHI causes and preserve
  failed-output, partial-allocation rollback, and retained-resource contracts.
- [x] Format only at graph logging, assertion, capture presentation, and UI
  boundaries. Audit stored diagnostic ownership after graph reset/destruction.
- [x] Test invalid declarations/metadata/dependencies, missing producers,
  safety limits, allocation failure, missing/incompatible allocations, and
  failure-path cleanup using codes and context rather than substrings.
- [x] Update Render Graph contracts; pass the workspace `all` build, RDG and
  allocator tests, affected consumers, and relevant Vulkan integration coverage.

Completion gate: all allocator implementations and RDG callers compile against
typed results; rollback, graph execution, and resource lifetimes remain covered.

### Stage 4: Close cross-layer migration and document residual adapters

Dependency: Stages 1 through 3. Outcome: the rendering error path is consistently
typed and its remaining external/presentation boundaries are explicit.

- [x] Search all project source/test roots for removed signatures, prose-based
  error decisions, early formatting, and lost nested causes. Classify residual
  `Message`/`OutError` matches by ownership instead of renaming unrelated fields.
- [x] Verify representative Shader-to-resource and RHI-to-allocator-to-RDG
  failures preserve original codes/context through their final presentation.
- [x] Complete final workspace `all` build and affected-target regression;
  document GPU environment and any unavailable coverage without marking it passed.
- [x] Reconcile lasting contracts, run changed-document and all-plan validation,
  and record validation receipts and implementation commits in Current Status.
- [x] Mark this plan Completed only when every required gate is satisfied.

Each implementation commit updates this plan's status/checklists and includes
the repository-required `Plan` and exact `Stage` trailers. If code inspection
changes a selected decision, update the decision and rationale before proceeding.
