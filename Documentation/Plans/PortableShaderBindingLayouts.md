# Portable Shader Binding Layouts Plan

Summary: Preserve one logical binding layout across backends, move target-specific shader data out of common RHI types, and prepare native binding operations before draw and dispatch.

Last reviewed: 2026-10-15

Status: Active
Completed:

## Current Status

Stage 0 preparation is complete against reference revision
`192f80a392e3cc26b6eb240c65f190da7fb11f35`. The workspace consumer inventory,
artifact ownership decisions, version migration, parameter-state semantics, and
measurement acceptance rules are recorded below. Stages 1 through 3 are
implemented. Stage 4 Windows correctness validation is complete; native Metal and stable
performance qualification remain open. On 2026-10-15 the user confirmed that a
macOS host is still unavailable, so this plan cannot yet be marked complete.
A follow-up review tightened raw/owned shader admission and Vulkan pipeline
membership validation, and removed redundant binding state and checks. The
post-implementation review receipts below cover these corrections; native Metal
and timing qualification remain deferred.
The user authorized a UE-inspired simplification on 2026-10-15. The follow-up
uses one `FShaderCode` representation for compiled code, reflection and tagged
platform bytes, replacing the separate artifact and its duplicated logical
layout. Reflection remains the canonical named declaration list; only merged
pipeline layouts are materialized. ShaderMap publishes a shared immutable code
owner that RHI creation reuses. This chooses a smaller public API than splitting
reflection into another layout/name-table abstraction. Full identity and all
admission checks remain required, and native binding preparation is unchanged.
The [simplification receipts](#code-ownership-simplification) record successful
Windows validation and the shared-API build; macOS remains deferred.
The [buffer and validation cleanup](#buffer-and-validation-cleanup) removes the
outer code-buffer pointer, centralizes logical-binding conversion in ShaderFormat,
and consolidates validation helpers while retaining every admission boundary and
prepared-binding invariant. Its Windows regression and build receipts are below.
The two simplification runs are retained as a [compact acceptance summary](#windows-simplification-validation-summary);
their generated XML reports are no longer tracked.
All earlier generated reports and timing samples for this plan are also kept
locally under ignored `Build/Qualification/PlanEvidence/PortableShaderBindingLayouts/`.
The repository retains acceptance summaries and reproduction settings; no
qualification was rerun during this report-retention cleanup.
On 2026-10-15 the user accepted the retained Windows correctness and timing
observations as sufficient to begin extraction and API migration. Stable timing
acceptance and native Metal qualification remain final gates; they no longer
block Stage 1. This changes execution sequencing, not the recorded noise limits
or the interpretation of failed timing checks.

The user confirmed an exclusive Windows timing lane on 2026-10-15. Native macOS
Metal execution will be supplied later. The default Windows Debug/Tracy build
is correctness-only and cannot establish timing acceptance. A binding-specific
qualification fixture now records producer update/recording, queue/replay, and
combined CPU samples with checked graphics/compute output. The retained Windows
A/A runs exceed the predefined noise limits; no timing baseline is accepted and
no candidate runtime migration was measured. A user-requested Windows retry on
2026-10-15 passed output checks and met both noise limits in 4/12 scenarios;
the full-matrix baseline gate remains open.

The [Metal RHI plan](MetalRHI.md) continues to own backend qualification and its
remaining workload, lifecycle, image-parity, and performance gates. This plan
owns binding ownership and preparation; it does not close those existing gates.

## Goal

Use a single backend-neutral logical resource address and let each target own
its lowering strategy. Compiler output and runtime must agree on the lowering.
Do not allocate native slots, decode platform metadata, or search logical-to-
native maps during direct or indirect draw/dispatch execution.

## Current Implementation and Migration Surface

Paths below are repository-relative inspection locations, not proposed APIs.

| Location | Current responsibility or issue | Required change |
| --- | --- | --- |
| `Engine/Source/Runtime/RHI/Public/RHIResources.h` | `FMetalShaderBinding`, remap helpers, Metal fields in `FRHIShaderDesc` and `FRHIShader`, public Metal getters | Retain logical vocabulary and generic artifact identity; remove Metal declarations, storage, and accessors |
| `Engine/Source/Runtime/RHI/Private/RHIResources.cpp` | Metal remap hashing/validation; Metal target branch in compute initializer validation | Move target rules to format/backend owners; retain universal validation |
| `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h` | Target identity includes MSL language version and binding remap schema | Separate generic target identity from target-specific compile settings |
| `Engine/Source/Runtime/RenderCore/Public/Shader/ShaderCompilerCore.h` | Logical reflection and explicit Metal map share the compiled output API | Keep reflection; carry immutable tagged platform bytes |
| `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderMetalBindings.cpp` | Stage-specific compact allocation, limits, and reserved vertex buffer slots | Move to SDK-independent target format implementation |
| `Engine/Source/Developer/ShaderBuild/Private/MetalShaderTranslator.cpp` | Generates MSL using the binding map | Generate code and metadata from the same mapping instance |
| `ShaderCompiledOutput.cpp`, `ShaderSharedOutput.cpp` | Target validation and field-by-field map serialization | Share target codec/validation rather than duplicate Metal rules |
| `ShaderPipelineLayout.cpp` | Copies Metal metadata into RHI creation arguments | Forward generic target artifact and logical interface |
| `MaterialCookedProgram.cpp`, `MaterialProgramCache.cpp`, `ShaderOutputCache.cpp` | Metal-specific serialization and memory accounting | Preserve generic bytes and count their actual ownership/storage |
| `ShaderMap.cpp`, `ShaderBuildSession.cpp`, `ShaderCompileUtilities.cpp`, `ShaderSourcePreparation.cpp` | Target settings participate in cache identity and transport | Migrate all identity and transport producers together |
| `Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalPipeline.h` and `.cpp` | Shader admission and pipeline compatibility; shaders inherit common Metal fields | Own decoded metadata in `FMetalShader`; own prepared operations in pipeline objects |
| `Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalCommandContext.cpp` | `EncodeComputeDispatch` searches the map per parameter; `BindGraphicsParameters` searches layout and stage maps; parameter updates merge/sort arrays | Replace draw-time resolution with prepared operations and indexed parameter state |
| `Engine/Source/Runtime/VulkanRHI/Private/VulkanShader.cpp` | Explicitly rejects Metal fields | Admit supported target/format without knowing Metal schema |

## Selected Design

### Logical Contract

Reuse `FBindingLayoutItem`, `FBindingLayout`, `FPipelineLayoutDesc`, and
`FRHIShaderParameterResource`; avoid a parallel layout API. Define existing
`SetIndex` as a logical group, not an obligation to allocate a native descriptor
set. Preserve current group ordering and source names initially to reduce churn.

- `(Group, Binding)` identifies one resource declaration regardless of type.
- Type, array count, and stage visibility belong to the declaration.
- Array elements are separately indexed; they do not consume adjacent logical
  binding numbers. Backend register ranges must still be checked for overlap.
- Push constants retain byte ranges and stage visibility, not native buffer slots.
- Dynamic offsets remain per-binding data; uniform/dynamic-uniform normalization
  must preserve the existing shader-interface compatibility rules.
- Merged stage layouts must reject incompatible declarations before publication.

### Ownership and Dependencies

Introduce an SDK-independent runtime module, provisionally `ShaderFormat`, with
generic artifact dispatch and explicitly separated Metal/Vulkan format units.
It can depend on Core and RHI logical value types, but not RenderCore,
ShaderBuild, MetalRHI, VulkanRHI, or native GPU SDKs. RHI must not depend back on
ShaderFormat. RenderCore, ShaderBuild, and backend modules may depend on it.
Using one module initially avoids a registration/plugin framework solely for two
formats; format-specific types stay behind dedicated headers.

Move `FMetalShaderBinding`, allocation, canonical remap hashing, schema codec,
and structural validation to its Metal unit. Adapt RenderCore reflection into
logical input values at the caller, rather than importing RenderCore reflection
types into the new module. Native device/function validation stays in MetalRHI.

Common shader creation carries an owned immutable target artifact: generic
target/format/version tags, code, entry point, and opaque target metadata bytes.
Reuse existing shared-buffer ownership types. The exact wrapper must retain
bytes through asynchronous creation; avoid borrowed metadata views escaping a
call. `FRHIShader` needs common identity, not a persistent copy of every payload.
`FMetalShader` owns the decoded Metal map after admission.

The ownership follow-up supersedes the separate artifact wrapper with a shared
immutable `FShaderCode` snapshot. Canonical named declarations now live in RHI
alongside that generic code representation, so ShaderFormat can validate them
without importing RenderCore. No per-stage `FPipelineLayoutDesc` or separate
name digest is retained. The required lifetimes and admission rules above remain
in force.

Backend selection and code-format identifiers may remain common discriminants.
MSL language options and remap policy versions move to target settings/metadata;
common identities carry a canonical digest of those settings. Do not remove
legitimate capability vocabulary, startup selection, or portable indirect record
layouts simply because their implementation mentions a platform.

### Target Strategies and Artifact Integrity

- Vulkan preserves the existing logical set/binding identity where possible;
  omit an explicit identity map. Validate actual code/layout compatibility.
- Metal generates a compact per-stage mapping at compilation, including vertex
  reservations and push-constant slots. Persist it beside the generated MSL.
  Runtime reads it; runtime must not independently reassign slots using a merged
  pipeline layout with a different resource population.
- A future DX12 backend can select register classes and preserve indices where
  legal, but must handle array register-range collisions through compiler-agreed
  lowering. No DX12 backend is implemented by this plan.

The artifact identity covers code, stage, entry point, target settings, format
version, logical interface, and metadata affecting execution. Keep code-only
hashes separately named where needed. Pipeline and prepared-operation identities
also include the merged logical layout and all shader stage identities.

Generic readers validate container bounds and hashes; target codecs validate
target schema and agreement with the logical interface; backends validate native
compatibility. Unknown schemas and corrupt maps fail before publication. A hash
alone is not semantic validation. Preserve complete-or-failure publication.

Version affected DSHD, shared-output transport, Material cooked data, and any DSLB
envelope/index whose contract changes. Explicitly invalidate obsolete cache
entries; incompatible cooked libraries require recooking and a useful error.
Do not add a silent fallback that accepts mismatched code and metadata.

### Preparation and Hot-Path Contract

At pipeline creation, build immutable backend binding operations by joining
logical declarations with the validated per-stage native mappings. Each operation
contains a dense parameter-state index, stage, native resource category, array
extent/native base slot, and required buffer-offset handling. Precompute push
constant destination slots and range extents as well.

Keep a bounded sparse-address-to-dense-index lookup for parameter updates; do not
allocate arrays sized by the maximum user-supplied binding number. Prepared
storage scales with declared bindings/elements, with checked size limits.

`RHISetShaderParameters` validates updates and writes dense resource state using
this lookup. It must not linearly scan stage maps, rebuild the operation list,
sort all accumulated parameters, or reconstruct all owners after each update.
Retain resource references and captured buffer allocations with their current
snapshot semantics. An update may occur every draw, so benchmark it together
with submission rather than hiding costs outside a narrowly timed draw call.

Draw/dispatch iterates prepared operations and accesses dense state directly.
Native calls, dynamic offset arithmetic, resource retention, and encoder-specific
re-emission remain allowed. No format decoding, native-slot allocation, map/layout
search, reflection merge, or mapping-related allocation is allowed, including
on the first draw. Array slot arithmetic is not a new mapping allocation.

Replace repeated layout-search-based completeness checks with prepared required
state/coverage checks while preserving errors for missing bindings and partial
push-constant writes. Pipeline switches reset or explicitly revalidate parameter
state according to existing behavior; no stale operation table may survive shader
reload or generation replacement. New encoders re-emit bindings even when their
logical values did not change. Direct and indirect paths share this contract.

Existing upload allocations and native-resource lifetime work are outside the
zero-mapping-allocation claim. Do not describe the entire draw path as allocation
free. Broader push-constant upload optimization is not required by this plan.

## Implementation Stages

### Stage 0: Freeze Contracts and Baselines

Dependencies: none.

- [x] Inventory consumers across source and test roots of Engine, Sandbox, and
  RoadWeaver as declared by `Durin.dworkspace`; include generated-code inputs.
- [x] Finalize artifact wrapper, target-setting identity, module declarations,
  format versions, and existing parameter reset/retention semantics.
- [x] Inspect native target registrations and select matching existing fixtures;
  record available graphics/compute correctness and CPU preparation/submission
  reference observations. Stable timing acceptance and native Metal receipts
  are deferred to Stage 4 by user decision on 2026-10-15.
- [x] Define same-host measurement noise and regression thresholds before
  candidate measurements, following the owning performance protocol.

Exit: reviewed API/ownership table, consumer inventory, and reproducible reference
receipts; no assumption that moving work to parameter updates improves frame time.
Timing stability is required before performance acceptance, not before extraction.

### Stage 1: Extract Target Formats and Generic Artifacts

Dependencies: Stage 0.

- [x] Add ShaderFormat and move Metal mapping/schema logic with deterministic
  round-trip tests; keep it buildable without the Metal SDK.
- [x] Introduce immutable tagged artifacts and target-setting digests; migrate
  ShaderBuild generation, shared transport, RenderCore output, and cache identities.
- [x] Migrate Material cooked serialization, memory accounting, and cooked-library
  admission; apply explicit schema/cache invalidation.
- [x] If temporary old-field adapters are needed for a buildable transition,
  generate them from the single artifact and remove them in Stage 2. Never maintain
  two independently mutable authorities for the map.

Exit: producer/consumer round trips preserve code-map agreement, corrupted or old
payloads fail predictably, and cross-platform tools do not load native backends.

### Stage 2: Remove Platform State from Common RHI

Dependencies: Stage 1.

- [x] Migrate shader creation and both backends to generic artifact admission.
- [x] Move decoded Metal data into `FMetalShader`; update backend-local getters
  and pipeline validation without changing existing native slot assignments.
- [x] Remove Metal types, helpers, fields, getters, and Metal-only compute checks
  from common RHI; remove target-specific rules from generic serializers.
- [x] Remove transition adapters, migrate all fixtures/consumers, and document
  which backend/format discriminants deliberately remain public.

Exit: a repository-wide symbol audit confirms no Metal schema dependency in common
RHI or VulkanRHI; shader/pipeline failure behavior remains complete-or-failure.

### Stage 3: Prepare Native Binding Operations

Dependencies: Stage 2.

- [x] Build graphics and compute operation tables before pipeline publication.
- [x] Replace parameter accumulation searches/sorts with bounded indexed state;
  preserve dynamic-offset, resource-owner, and buffer-allocation capture semantics.
- [x] Replace graphics layout/map searches and compute map searches in command
  submission with operation execution; prepare push-constant destinations/coverage.
- [x] Cover pipeline switch, encoder restart, shader reload, failed preparation,
  missing parameters, stage fan-out, arrays, and direct/indirect execution.
- [x] Audit Vulkan preparation/submission to retain its identity mapping without
  introducing new runtime translation or platform-schema coupling.

Exit: structural inspection plus counters demonstrate zero map construction,
decoding, searching, or mapping allocations in all draw/dispatch variants,
including first use. Parameter-update costs are measured separately and together.

### Stage 4: Qualify and Publish Contracts

Dependencies: Stage 3.

- [ ] Run format, cache/transport, Material cooked, RHI validation, RenderCore
  reflection, Vulkan, and native Metal fixtures selected in Stage 0.
- [ ] Compare checked rendered/compute outputs for shared-stage bindings, sparse
  logical indices, arrays, dynamic uniform offsets, storage resources, and push
  constants. Include malformed schema/count/slot and code-map mismatch cases.
- [ ] Measure pipeline preparation time/memory and parameter-update-plus-submit
  CPU median/p95 for unchanged resources, per-draw changes, and pipeline changes.
  Report raw samples and lookup/allocation counts; compare identical workloads.
- [ ] Validate on Windows Vulkan and macOS native Metal; cross-compilation or CPU
  format tests alone do not close native Metal correctness/performance gates.
- [x] Complete affected project validation and the shared Engine API `all` build.
- [x] Unify compiled code and reflection ownership, remove the separate artifact
  and repeated layout construction, and preserve validation and prepared bindings.
- [x] Remove the outer byte-buffer pointer and RenderCore binding adapter, share
  validation helpers, and verify unchanged admission and prepared-binding behavior.
- [x] Update owning runtime/module contracts, retain evidence in this plan, and
  reconcile dependent Metal qualification work without declaring unrelated gates
  passed.
- [ ] Mark complete only after required platform receipts are available.

Exit: correctness gates pass, hot-path invariants hold, and agreed CPU/memory
regression limits pass. Unavailable hardware remains an explicit open gate.

## Stage 0 Execution Record

### Consumer Inventory

Inspected the source and native-test roots declared by all three projects in
`Durin.dworkspace`, plus `Templates`, `Tools`, and the generated native-test
environment input. Searches covered target identity/constants, compiled shader
values, RHI shader descriptors, Metal map fields/helpers/getters, remap identity,
and target-setting version fields. Sandbox and RoadWeaver have no direct
consumers of those symbols; their project targets still require final shared-API
validation. Templates and Tools have no direct consumers either.

| Engine owner | Migration consumers beyond the implementation table |
| --- | --- |
| RHI | `RHIPipelineCreation.cpp` admission/metadata budgets; graphics/compute key hashing and `FRHIShaderCreateDesc` ownership; `RHIPipelineCreationTests.cpp` and `RHICommandListTests.cpp` |
| ShaderBuild | `SlangShaderCompiler.cpp`, `ShaderCompileUtilities.h`, `MetalShaderTranslator.h`, `ShaderLibraryProducer.cpp`, `ShaderBuildSession.h`, and `ShaderBuildVersion.h`; generated and authored requests must share target-setting transport |
| RenderCore | Public `Shader.h`, `MaterialShader.h`, `ShaderCompilerCore.h`; `MaterialShader.cpp`, `ShaderCookedLibrary.cpp`; `ShaderFoundationTests`, `ShaderReflectionTests`, `ShaderDerivedDataTests`, `ShaderBuilderTests`, `ShaderCookedLibraryTests`, `ShaderCookIntegrationTests`, `RenderResourceCreationTests`, and `RDGTests` |
| Engine Materials | `MaterialProgramCompiler.h`, `MaterialProgramGenerator.cpp`, `MaterialProgramCompiler.cpp`, `MaterialPreparedProgram.h`, `MaterialInterface.cpp`, `MaterialCook.cpp`; `MaterialCompilerTests`, `MaterialCompileLifecycleTests`, `MaterialQualificationTests` |
| Renderer | `Renderers/MeshRendererShared.h`; `RendererSceneContractTests` and `StaticMeshRenderPreparationVulkanTests` |
| VulkanRHI | `VulkanShader.cpp`; `VulkanResourceCreationTests`, `VulkanTextureSamplingTests`, `VulkanPublicRHIConformanceTests` |
| MetalRHI | `MacOS/MetalPipeline.h/.cpp`, `MacOS/MetalCommandContext.cpp`; `MetalRHIHeadlessTests.cpp`, `MetalRHIResourceTests.mm`, and `MetalMaterialQualificationTests.mm` |

Generated-code input
`Engine/Tests/NativeTestSupport/Private/AuthoredShaderTestEnvironment.cpp.in`
loads ShaderBuild but embeds no shader metadata. Material generated source is
owned by `MaterialProgramGenerator.cpp`; its request target is part of this
migration. No DHT declaration embeds the selected target/map types. Re-run the
symbol inventory after removing transition fields, including all three projects.

### Frozen Ownership and API Decisions

These are implementation decisions, not implemented runtime contracts.

| Value or operation | Selected owner and contract |
| --- | --- |
| `FShaderTargetIdentity` | RHI discriminants plus `FXxHash128 SettingsDigest`; remove language/remap settings from common identity. Hash fields canonically, never native struct bytes. |
| `FShaderTargetArtifact` | New common RHI value, published as `std::shared_ptr<const FShaderTargetArtifact>`. Own target identity, code format, format version, stage, compute group size, binary entry point, stage logical `FPipelineLayoutDesc`, `FSharedByteBuffer` code and metadata, code hash, and complete artifact identity. Creation descriptors retain the immutable owner through asynchronous work; FRHIShader retains identity and common properties only. |
| Compile target settings | ShaderFormat resolves immutable target-settings profiles through an opaque canonical profile digest. Profile `Durin.ShaderTargetProfile.2.1` owns MSL 200 and remap policy 1. Unknown digests are rejected; different settings require a new profile/digest. ShaderBuild and generated requests carry that digest through every identity/transport path. No independently mutable settings bytes or legacy map. |
| Logical adaptation | RenderCore converts reflection into existing RHI logical layout values. Preserve names in reflection only; format allocation consumes no RenderCore types. Validate stage visibility and normalize dynamic/uniform compatibility at the logical boundary. |
| Artifact identity | Domain-separated canonical hash over target/settings digest, code format/version, stage/group size, entry point, code bytes, logical interface, and execution metadata. Keep `CodeHash` separately named. Pipeline identity also covers merged layout and every stage artifact identity. |
| ShaderFormat module | `Engine/Source/Runtime/ShaderFormat/ShaderFormat.dmodule`, registered in `Engine.dproject`, shared link type, public dependencies Core and RHI. Generic artifact dispatch plus dedicated Metal/Vulkan headers and source units. No native SDK, backend, RenderCore, or ShaderBuild dependency; no RHI dependency back to ShaderFormat. |
| Module consumers | RenderCore, ShaderBuild, MetalRHI, and VulkanRHI depend on ShaderFormat at the visibility required by their final includes. Engine Materials uses generic RenderCore artifacts; format tests link ShaderFormat without either native backend. |
| Metal codec | One schema-1 allocation/hash/codec implementation used by generation and admission. Preserve current slots: vertex buffers start at 16, other stage buffers at 0, limits 31 buffers/128 textures/16 samplers, push slot after resource buffers. Reuse the same mapping instance for MSL and metadata. |
| Vulkan codec | SPIR-V artifact with empty target metadata; validate code/interface compatibility without a serialized identity map or Metal declarations. |
| Prepared state | Pipeline-owned immutable operations and required coverage; sparse logical declaration lookup plus checked dense array-element storage. Do not resize by the largest binding number. Charge retained metadata against the existing pipeline metadata budget and preserve capacity failures before publication. |

Version changes are applied atomically with their producers/readers, not as this
preparation record: DSHD payload 5 to 6; shared output schema 6 to 7; shared
compile constants/options schema 2 to 3; compile build function 4 to 5; Material
payload/family schema 13 to 14; DSLB schema 5 to 6. Preserve runtime request
identity v5: inspection during Stage 1 confirmed its source-independent request
inputs are unchanged, and its existing compatibility golden must remain stable.
This corrects the preparatory proposal to bump it with the container.
Bump variant key v8 to v9, dependency key v3 to v4, source request v2
to v3, and shader-map key v2 to v3. Advance compile/Metal translation/library
producer revisions when their output changes. Preserve source-only closure
identity when its byte contract is unchanged. Reject obsolete cooked payloads
with a recook diagnostic; obsolete cache values miss and regenerate. The Metal
mapping policy remains schema 1 because its native allocation is unchanged;
the generic artifact envelope starts at version 1.

### Parameter State and Lifetime Baseline

`MetalCommandContext.cpp` clears parameters, retained owners, captured uniform
allocations, push bytes, and written-byte coverage when setting either pipeline,
even when setting the same pipeline again. Submission/render-pass lifecycle
resets also clear the corresponding state. Preserve those reset boundaries.
Updates replace one `(SetIndex, BindingIndex, ArrayElement)` value; current
graphics state is shared across vertex/fragment visibility. Validate the entire
update before publishing changes. Ordinary resources retain strong RHI owners;
uniform buffers capture `GetAllocation()` during the update, so later logical
buffer updates cannot change a recorded dispatch. Keep ownership and allocation
snapshots per dense element when replacing the accumulation vectors/maps.

Current draw/dispatch checks require complete bindings and every byte of each
visible push range. Prepared coverage must preserve those failures and accept
legal partial updates that collectively complete state. Encoder restart must
re-emit native bindings. Direct and indirect compute share
`EncodeComputeDispatch`; graphics direct/indirect paths must share prepared
execution without delayed first-use table construction.

### Fixture Selection and Timing Acceptance

Registry discovery succeeded with `test list shader`, `test list RHI`, and
`test list render-performance` on `Win64-Debug-DurinEditor`. Source registrations
also confirm macOS-only `MetalRHIHeadlessTests` and `MetalShaderQualificationTests`;
their absence on Windows is expected, not passing coverage.

| Gate | Selected existing fixture/target |
| --- | --- |
| Logical reflection, compiler, artifact admission | `RenderShaderContractTests`, `RenderShaderBuilderTests` |
| Cache/shared transport and cooked libraries | `RenderShaderCacheTests`, `RenderShaderCookedLibraryTests`, `RenderShaderCookIntegrationTests` |
| Common RHI validation and lifetime | `RHIPipelineCreationTests`, `RHICommandListTests`, `RenderContractTests`; select Material lifecycle targets from the registry before execution |
| Windows graphics | `VulkanRHIIntegrationTests`, especially `FVulkanPublicRHIConformanceTests.DynamicOffsetsReuseSparseArrayDescriptorsAndSelectCorrectPixels` and `DescriptorSetsReuseCommonStateAcrossMaterialAndCompatiblePipelineChanges` |
| Native Metal compute/lifetime | `MetalRHIHeadlessTests`, especially `FMetalRHIComputeTests.RecordedDispatchWritesStructuredBufferInBothExecutionModes`, `StorageWriteThenSampleUsesNativeTextureAndSamplerViews`, and `FMetalRHIBufferTests.DeferredUniformVersionsReachSeparateDispatches` |
| Native Metal output | `MetalShaderQualificationTests`, especially push/storage readback, resource arrays, authored ImGui pixels, and production Material pixels |
| Existing creation/preparation observations | `VulkanCreationQualificationTests`, `StaticMeshRenderPreparationVulkanTests`; these do not replace the binding-specific samples below |

Add a binding-specific workload to registered qualification coverage before
changing runtime behavior. Freeze equivalent checked graphics and compute
workloads with shared-stage resources, sparse logical bindings, arrays, dynamic
uniform offsets, storage resources, push ranges, and direct/indirect commands.
Measure unchanged bindings, one changed element per draw/dispatch, and pipeline
switches. Report pipeline preparation time/retained bytes, parameter-update CPU,
submission CPU, and combined CPU independently; count map builds, decodes,
searches, and mapping allocations including first use. Do not time GPU wait as
CPU submission; retain checked output/readback separately from timed intervals.

Use Release Editor, Tracy off (`DURIN_WITH_TRACY=0` verified in compile commands),
validation/capture off, threaded RHI, warm caches, and the user-confirmed quiet
lane. Use 30 warm-up and 120 measured samples per scenario per run, three runs,
raw CSV, nearest-rank p95, exact Git revision/device/driver/toolchain, logs and
XML. Before any candidate samples, repeat the unchanged reference in alternating
A/A order. Define noise as maximum absolute paired relative difference across
runs for each metric; require median noise <= 2% and p95 noise <= 5%, otherwise
the lane is inconclusive and must be remeasured. Candidate combined-CPU median
may regress at most 5%, p95 at most 10%, on every scenario. Do not hide a failing
combined result behind a faster submission-only result. Pipeline creation
median/p95 and retained metadata may regress at most 10% against the same
workload; also enforce the existing absolute pipeline metadata budget. Record
zero mapping work/allocation counts in every submission variant as a separate
structural gate. These are premeasurement budgets, not measured improvements.

### Validation Receipts and Remaining Prerequisites

- Reference code revision: `192f80a392e3cc26b6eb240c65f190da7fb11f35`;
  only this plan was changed during initial reference execution.
- `RenderShaderContractTests --report`, `Win64-Debug-DurinEditor`: 55/55 cases
  passed. XML: `Build/NativeTestResults/Win64-Debug-DurinEditor/RenderShaderContractTests.xml`.
  Raw log: `Build/.agent-state/logs/20261015-110922-670081-30116-RenderShaderContractTests.log`.
- MSVC 19.44.35219 initially failed with C1902 inside the sandbox, including
  compilation of the PCH itself. The same DevTool PCH build and shader test
  succeeded outside the sandbox. Future native builds use that execution lane;
  no compiler flags or source behavior were weakened to suppress the failure.
- `configure --preset Win64-Release-DurinEditor -DDURIN_ENABLE_TRACY=OFF`
  passed; `compile_commands.json` confirms `DURIN_WITH_TRACY=0`.
- Initial Release `VulkanCreationQualificationTests` execution selected
  `FVulkanCreationQualificationTests.ColdHotAndConcurrentRequests` and passed.
  Device: NVIDIA GeForce GTX 1060 6GB; Vulkan API 1.4.312; raw driver value
  `0x91908000`. This initial run removed its work directory on success, so it
  establishes execution/correctness only. Retained reference runs use
  `DURIN_TEST_KEEP_WORK=1`; never claim missing raw samples as timing evidence.
- Three retained creation runs passed. Raw samples, device records, and XML are
  retained as `Creation-1.csv` through `Creation-3.csv`, `CreationDevice-*.txt`,
  and `CreationReport-*.xml` in the evidence directory linked below. These are
  creation observations; they do not satisfy binding-update timing gates.
- Windows Debug `VulkanRHIIntegrationTests`: all three selected sparse-array,
  dynamic-offset/compatible-pipeline, and uniform-version graphics/compute cases
  passed. XML:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/PortableShaderBindings-Correctness.xml`.
  Raw log:
  `Build/.agent-state/logs/20261015-112206-869870-47280-VulkanRHIIntegrationTests.log`.
- Release `FVulkanCreationQualificationTests.BindingUpdateAndSubmission` passed
  checked output for all 12 route/scenario combinations. It covers direct and
  indirect graphics/compute, unchanged resources, partial uniform replacement,
  and resetting the same pipeline. It does not substitute same-pipeline reset
  for distinct-pipeline changes, or add sparse/array/counter coverage implicitly.
  Each measured dataset has 8640 rows: 12 combinations, six runs, 120 samples.

Local report directory (ignored): `Build/Qualification/PlanEvidence/PortableShaderBindingLayouts/2026-10-15-Windows/`.

| Retained dataset | Environment and interpretation |
| --- | --- |
| Initial samples and noise | 64 commands/sample, original pilot without frame boundaries; diagnostic only. All 12 combined-CPU A/A noise checks failed. |
| Frame-bounded samples and noise | Final fixture: 1024 commands/sample, normal BeginFrame/EndFrame, HighQoS enabled, affinity 65535. Only 1/12 combinations met both noise limits. |
| Affinity samples, summary, and noise | Same final fixture, HighQoS enabled, process affinity `0xFFF` (logical processors 0-11). 0/12 combinations met both noise limits; median noise 0.72-16.10%, p95 noise 5.59-63.63%. Inconclusive timing, not an accepted baseline. |

The final fixture uses the complete frame lifecycle to retire descriptor caches.
A 1024-command pilot without frame retirement hit the existing descriptor-pool
expansion limit; no runtime limit was raised to accommodate the test. Pilot
failures are not timing evidence. The stable owning workload is documented in
the [performance protocol](../Development/Build/RenderingPerformanceBaseline.md#binding-update-and-submission-workload).

Summary calculation: sort each group of 120 rows by each metric, average sorted
positions 60/61 for median, and use sorted position 114 for nearest-rank p95
(positions are one-based). Compare paired runs 0/1, 2/3, and 4/5 using
`abs(B / A - 1) * 100`; retain the maximum per scenario/metric. The acceptance
columns use unrounded noise, not displayed rounded percentages. No failed
scenario was dropped and no threshold was relaxed.

Reproduction uses the final fixture with these process-local environment values:
`DURIN_VULKAN_VALIDATION=off`, `DURIN_TEST_KEEP_WORK=1`,
`DURIN_CREATION_QUALIFICATION_HIGH_QOS=1`, and
`DURIN_CREATION_QUALIFICATION_AFFINITY=0xFFF`, then:

```powershell
.\DevTool.bat test VulkanCreationQualificationTests FVulkanCreationQualificationTests.BindingUpdateAndSubmission --mode qualification --preset Win64-Release-DurinEditor --test-jobs 1 --timeout 900 --report
```

The exact reference runtime revision is recorded above. The added fixture's
SHA-256 is `81971FF4C6F70DF65ECE133B9AD4280EDC8E2B405E763CC4A881B47F173531F1`;
the unchanged `DeferredBufferVersions.slang` input SHA-256 is
`32F4A7021CF068AB409EA121F697EA909EEE1803D1666E9A86012C48548EB5C1`.
The measured executable SHA-256 is
`CF6298002D928EED08226D8A95844EC7704685A7A2F0589E186B98A0D27918B7`.
The final raw test output, JUnit result, and host details are retained as
`BindingRun.txt`, `BindingReport.xml`, and `BindingDevice.txt` in the evidence
directory. The final local CTest log is
`Build/.agent-state/logs/20261015-112713-021841-48848-ctest.log`.

### Windows Retry on 2026-10-15

At the user's request, reran the unchanged final fixture with the same Release,
Tracy-off, validation-off, threaded RHI, HighQoS, and affinity `0xFFF` settings.
The measured checkout was `2b555bd24`; fixture and executable SHA-256 values
matched those recorded above. No workload, threshold, or runtime code changed.
All graphics/compute output checks passed. Retained 8640 measured rows across
six runs for each of the 12 combinations.

The retry met both combined-CPU noise limits in 4/12 scenarios, compared with
0/12 in the previous affinity dataset. Median noise ranged from 0.47% to 2.64%
and p95 noise from 1.97% to 7.60%. The predeclared limits remain 2% and 5%; the
entire matrix must pass, so this remains an inconclusive timing receipt.

Raw retry samples, summaries, noise checks and host/test reports remain local under ignored `Build/Qualification/PlanEvidence/PortableShaderBindingLayouts/2026-10-15-Windows-Retry/`.
Local CTest log:
`Build/.agent-state/logs/20261015-114017-083754-44588-ctest.log`.

Final timing acceptance still requires the complete binding workload to have a
repeatable same-host timing receipt within the predeclared noise limits. A quiet
lane was user-confirmed, but the observed noise still failed; exclusive use
alone is not evidence of stable timing. Native Metal baseline execution is
unavailable on this Windows host; the user will provide macOS later. Preserve
the reference revision for that host rather than measuring the migrated runtime
and calling it a pre-migration baseline. No native Metal correctness or timing
gate is closed by the Windows/compiler receipts.

## Stage 1 Execution Record

Completed on Windows on 2026-10-15. The SDK-independent ShaderFormat module now
owns Metal allocation, remap validation, and the bounded versioned metadata
codec. ShaderBuild configures translation and emits metadata from the same map.
Vulkan uses the generic artifact contract without an additional identity map.

Compiled outputs retain opaque metadata. Immutable runtime artifacts own code,
metadata, entry/debug names, and the logical interface. Their identity includes
the target settings, code, metadata, and name-to-address interface identity;
the existing compiled-code hash remains code-only. Backend admission checks
attached artifacts before native creation. Metal's temporary legacy fields are
decoded from the attached artifact, which remains the single authority.

Shared transport, DSHD, cooked libraries, Material payloads, cache identities,
and retained-memory accounting were migrated together. Old payloads are rejected
with recook diagnostics. The source-independent runtime request identity stays
at v5 because its inputs did not change; the frozen version decisions above
record the changed formats and keys. Target settings use immutable known profile
digests, with unknown profiles rejected before translation or creation.

Validation on the final source:

| Check | Result | Local receipt |
| --- | --- | --- |
| Shader domain | 7/7 targets passed, including 4 SDK-independent ShaderFormat cases | `Build/NativeTestResults/Win64-Debug-DurinEditor/domain-shader.xml` |
| Material compiler, lifecycle, and cook | 82, 6, and 7 cases passed | Logs `20261015-121718-204224-42192-MaterialCompilerTests.log`, `20261015-121628-143204-27636-MaterialCompileLifecycleTests.log`, and `20261015-121850-160453-35440-MaterialCookTests.log` under `Build/.agent-state/logs/` |
| RHI command list and pipeline creation | 142 and 14 cases passed | Logs `20261015-121922-594633-31732-RHICommandListTests.log` and `20261015-121939-028017-50108-RHIPipelineCreationTests.log` under `Build/.agent-state/logs/` |
| Vulkan sparse/dynamic/compatible layouts and uniform capture | 3 cases passed | `Build/NativeTestResults/Win64-Debug-DurinEditor/PortableShaderBindings-Stage1-Vulkan.xml` |
| Final shared-API `all` build | Passed for Engine, Sandbox, and RoadWeaver | `Build/.agent-state/logs/20261015-122000-441860-30784-cmake.log` |

The transition still retains legacy common-RHI storage/accessors and the Metal
compute-group check for Stage 2. Draw/dispatch preparation remains Stage 3 work.
Native Metal compilation/execution and stable performance acceptance were not
performed on this Windows host and remain open; these correctness receipts do
not close those gates.

## Stage 2 Execution Record

Common RHI no longer declares or stores Metal schema values. Complete artifact
identities replace remap-only pipeline identity fields. RenderCore forwards the
owned artifact directly, Metal requires it and owns the decoded map, and Vulkan
admission contains no Metal-field checks. Target execution-metadata rules are
centralized in ShaderFormat rather than duplicated in generic serializers.
Native fixtures finalize test authoring state into generic artifacts before
calling RHI; no production legacy-field adapter remains. Public discriminants
are documented in the RHI API contract.

The Windows shared-API `all` build passed in 178.40 seconds, with receipt
`Build/.agent-state/logs/20261015-124443-354069-15636-cmake.log`.
The shader domain passed all seven targets, with receipt
`Build/.agent-state/logs/20261015-124829-857530-21212-ctest.log`.
Common RHI and Vulkan source symbol audits found no Metal binding-schema
dependencies. Native Metal source changes require the pending macOS build and
runtime receipts; Windows compilation does not establish those results.

## Stage 3 Execution Record

Graphics and compute pipelines now prepare an immutable `FMetalBindingPlan`
before publishing native state. It joins the merged logical declarations with
the admitted per-stage map and stores direct native operations, dense parameter
indices, and push slots/extents. Preparation rejects incompatible declarations,
stage populations, array extents, native slots, or push coverage. Storage is
bounded by the format's native resource limits rather than sparse addresses;
the retained plan also enforces the 1 MiB absolute metadata ceiling.

The command context replaces accumulated vector searches/sorts, tuple-keyed
allocation maps, and full owner rebuilding with indexed parameter state and
per-entry owners/allocation snapshots. Updates validate the batch before
mutation. Graphics draw/indexed/indirect paths share operation execution;
compute direct/indirect paths share dispatch encoding. Both use constant-time
missing-resource/push-byte counts and emit every operation for a new encoder.
Pipeline selection and submission/pass teardown reset state; new pipeline and
shader identities cannot inherit an old operation table.

The SDK-free `MetalBindingPlanTests` host target covers sparse arrays, stage
fan-out, complete-or-failure preparation, owner replacement/reset, dynamic offset
values, partial push writes/holes, and generic native-fixture artifact creation.
Its first/repeated-submission case executes 500 operations in 100 batches with
zero additional parameter lookups, state mapping allocations, format map builds,
or metadata decodes. The counters cover binding work, not all native uploads or
resource-retention allocations. Native encoder restart and direct/indirect output
receipts still require the registered macOS fixtures in Stage 4.

Vulkan retains its descriptor set/binding identity. Its existing descriptor
validation, dynamic offsets, uniform resolution, and cache reuse were inspected
in `VulkanPendingState.cpp`, `VulkanDescriptorSets.cpp`, and `VulkanPipeline.cpp`;
no Metal schema or new target translation was introduced. This change does not
claim removal of Vulkan's existing descriptor update/validation work.

Host tests exposed two format-admission details: dynamic uniform declarations
must normalize to the same native uniform category, and MSL may have a leading
comment, compact include, or no include. ShaderFormat now checks source bounds
and embedded NULs; native compilation remains responsible for language validity.
Corruption fixtures use illegal NUL bytes rather than a textual include prefix.
The Metal translation producer revision advances to 3 so cached pre-normalization
metadata cannot be reused as fresh compiler output.

Windows receipts: the shader domain passed all eight targets; the final
`MetalBindingPlanTests` target passed seven cases, with log
`Build/.agent-state/logs/20261015-125939-489070-16392-MetalBindingPlanTests.log`.
The final host counter XML records 100 batches/500 operations with zero added
map builds, metadata decodes, parameter lookups, or state mapping allocations.
The final shader-domain receipt is
`Build/.agent-state/logs/20261015-130007-446564-32728-ctest.log`; the final shared
Engine API `all` build passed in 1.18 seconds with receipt
`Build/.agent-state/logs/20261015-130022-531165-8660-cmake.log`.

## Stage 4 Windows Receipts and Open Gates

Candidate implementation revision: `f8a6d837c` (following Stage 2 `47689c4c7`
and Stage 1 `52b90de28`). Validation used `Win64-Debug-DurinEditor`; it is
correctness and structural evidence, not a timing configuration.

| Selection | Result |
| --- | --- |
| Shader domain | 8/8 targets passed on the final producer version |
| SDK-free Metal binding plans | 7/7 cases passed; 100 submission batches, 500 operations; zero additional map builds, metadata decodes, parameter lookups, or state mapping allocations; fixture plan retains 388 bytes |
| Material cooked payloads | 7/7 cases passed |
| Material compiler and lifecycle | 82/82 and 6/6 cases passed |
| RHI command lists and pipeline creation | 142/142 and 14/14 cases passed |
| RenderCore contracts | 221/221 cases passed |
| Vulkan sparse/dynamic/compatible layouts, uniform versions, and storage-to-sampled images | 5/5 selected cases passed |
| Final `all` build | Engine, Sandbox, and RoadWeaver passed |

Structural inspection of `EncodeComputeDispatch` and `BindGraphicsParameters`
found no map build/decode, `GetMetalBindings`, layout binding walk, resource
search/sort, or layout-based completeness call. Both direct and indirect routes
use those functions. Common RHI and VulkanRHI contain no Metal binding-schema
types, fields, helpers, or dedicated format includes. The host counters measure
the shared SDK-free plan/state implementation; they do not establish native
encoder execution or include native uploads/resource retention in their scope.

The following prerequisites remain open and prevent plan completion:

- A macOS host is unavailable, confirmed by the user on 2026-10-15. Native Metal
  source compilation and the registered `MetalRHIHeadlessTests` and
  `MetalShaderQualificationTests` must run on the candidate, including real
  direct/indirect output, sparse arrays, stage fan-out, dynamic offsets,
  push/storage resources, uniform-allocation capture, encoder restart, and
  pipeline/shader replacement. Existing older Metal receipts are not receipts
  for this migration.
- The retained Windows reference A/A matrix has not passed its predeclared
  noise limits. No candidate timing comparison is accepted. Repeat the reference
  in a stable Release/Tracy-off lane before comparing the candidate; preserve
  the reference revision and identical workloads on each platform.
- The complete preparation-time/retained-memory and update-plus-submission
  median/p95 matrix is still required for both platforms, with raw samples,
  first-use binding-work counters, and the frozen regression limits. A
  correctness pass or a 388-byte host fixture is not native memory/performance
  acceptance. The initial timing fixture's same-PSO reset is not evidence for
  distinct-pipeline switching or the full sparse-array workload.

Resume with the current candidate for correctness, preserving the Stage 0
reference for baseline measurements. Follow the linked build, native-test,
GPU-environment, and performance protocols. Do not set `Status: Completed` or
archive this plan until both native correctness and performance gates pass.

## Post-Implementation Review

Review base: `d61365aa9`. The user requested direct fixes and cleanup while
keeping macOS qualification deferred. Three omissions were corrected:

- Raw Vulkan descriptions previously skipped generic admission. They now
  validate the target/format, bounded code shape, code digest, frequency, entry
  point, and execution metadata. A nonzero artifact identity requires an owned
  artifact, and an explicitly supplied identity must match that artifact.
- Artifact hashes did not establish logical-interface validity. Admission now
  rejects invalid resource types/visibility, duplicate logical addresses,
  invalid array bounds, malformed or overlapping push ranges, and embedded NULs
  in entry names. Compiled outputs and native admission use the same interface
  validator; logical bounds are checked before layout construction and artifact
  hashing. Recomputed hashes do not bypass these semantic checks.
- Vulkan parameter updates compared only code hashes. They now compare target,
  format, stage, artifact identity, entry point, and compute group metadata as
  well. Backend-only RHI predicates share this rule, and compute uniform state
  is updated only after validation. Identity rejection remains enabled in Release.

Cleanup removed the unused operation type field and state getter, temporary
parameter records in preparation, obsolete serialization helpers/constants,
redundant native Metal code/hash/group checks, and ignored one-off edit scripts.
Metal native limits are now shared by allocation, validation, metadata decoding,
and preparation. Valid serialized artifacts and their identities are unchanged;
this review does not require another wire/cache version change.

| Final Windows validation | Result |
| --- | --- |
| Shader domain | 8/8 targets passed |
| Format/admission checks | 6/6 cases, including raw rejection and rehashed malformed interfaces |
| Metal plan/state checks | 7/7 cases; 100 batches/500 operations with zero added map builds, decodes, parameter lookups, or state mapping allocations |
| RHI command/pipeline contracts | 143/143 and 14/14 cases; includes same-code/different-artifact and entry-point rejection |
| Material compiler, cooked payloads, and lifecycle | 84/84, 7/7, and 6/6 cases |
| Selected Vulkan output/resource cases | 5/5 cases passed |
| Shared API `all` build | Passed for Engine, Sandbox, and RoadWeaver in 26.50 seconds |

The SDK-free fixture plan now retains 364 bytes after removing redundant
operation storage (previously 388 bytes). This is a host fixture observation,
not native Metal memory or performance acceptance. The platform and timing
prerequisites listed above remain open.

## Code Ownership Simplification

Review base: `fce2a7578643a95846b5b0cce6f5beff7c04bd58`. The user authorized this
follow-up while retaining native macOS qualification as an open gate.

`FShaderCode` replaces both `FCompiledShader` and `FShaderTargetArtifact`.
Compiler, cache and cooked data use the same representation; ShaderMap publishes
one immutable shared snapshot and passes that owner into RHI creation. Reflection
is the sole stored declaration list. Complete identity is derived directly from
it, with the name digest kept internal. `CodeHash` and `ShaderIdentity` distinguish
bytecode-only and full-contract identity. The legacy v1 hash encoding and valid
DSHD/shared-output/Material/DSLB wire formats are unchanged.

Cleanup removes the duplicate logical layout, identity-only temporary code
owners, and ShaderMap's temporary copies of complete compiled stages. All
workspace consumers were migrated. A new host regression checks shared owner
reuse, source-buffer lifetime, frozen reflection, and order-independent identity.
Existing malformed-code/interface/metadata rejection and native binding
preparation remain covered.

Windows validation passed for `f9c1709c2`; results are consolidated in the
[acceptance summary](#windows-simplification-validation-summary).

## Buffer and Validation Cleanup

Review base: `f9c1709c2047c0f0bd73dc44377d83e9c15f919b`. The user authorized
three further simplifications while retaining macOS qualification as deferred.

- `FShaderCode::Code` now stores `FSharedByteBuffer` directly. Copies and decoded
  cache subviews retain the same immutable allocation without an outer buffer
  object/control block. Retained-byte accounting reads the buffer directly.
- ShaderFormat owns `BuildLogicalBindings`; its validator, the Metal translator
  and fixtures use that one implementation. The RenderCore adapter and its source
  file were removed.
- `ValidateCodeHash` is shared by generic shader admission, raw RHI creation,
  DSHD and Material Cook checks. Owned RHI admission compares all descriptor
  fields and bytes before validating the owner once; shared-output writing uses
  the complete validator instead of repeating its individual checks. Archive
  bounds, exact error classification, metadata checks and every admission boundary
  remain in place. Wire formats, identity encoding and native binding preparation
  are unchanged.

Ownership regressions verify storage sharing and source lifetime. Owned creation
also rejects a changed descriptor digest, truncated byte view, null entry point,
and a corrupt owner digest even when the descriptor repeats that digest.

Windows validation passed for `57ac14164`; results are consolidated below.

## Windows Simplification Validation Summary

Both revisions were validated on 2026-10-15 with the Windows
`Win64-Debug-DurinEditor` preset. This summary replaces the 18 generated XML
reports added by `f9c1709c2` and `57ac14164`; it records prior results, not a new
test run during report cleanup.

| Selection | Result for each revision |
| --- | --- |
| Shader domain | 8/8 targets passed, covering ownership, admission, reflection, cache and Cook |
| RHI command/pipeline contracts | 143/143 and 14/14 cases passed |
| Material compiler, Cook and lifecycle | 84/84, 7/7 and 6/6 cases passed |
| Rendering contracts | 221/221 cases passed |
| Vulkan public conformance, uniform-version capture and dual-use texture cases | 5/5 cases passed |
| Metal plan/state host checks | 7/7 cases; 100 batches/500 operations, zero submission map builds, decodes, parameter lookups or mapping allocations; 364 retained plan bytes |
| Engine, Sandbox and RoadWeaver `all` build | Passed: 28.01 seconds for `f9c1709c2`, 54.95 seconds for `57ac14164` |

Build-only checks passed for `ShaderBuildQualificationTests`,
`MaterialQualificationTests` and `VulkanCreationQualificationTests` in both runs.
`StaticMeshRenderPreparationVulkanTests` also compiled for `f9c1709c2`, and
`MaterialCreationQualificationTests` for `57ac14164`. These opt-in targets were
not executed as performance qualification.

Native macOS compilation/output/timing and the existing stable timing gates
remain open.

## Validation and Contract References

Follow [build guidance](../Agents/BuildAndRun.md) and
[native test selection](../Agents/Testing.md) before executing implementation
validation. Use the [rendering performance protocol](../Development/Build/RenderingPerformanceBaseline.md)
for authoritative timing; no universal percentage improvement is promised.

Update the [RHI public API contract](../Runtime/Rendering/RHIPublicAPIStability.md),
[graphics binding contract](../Runtime/Rendering/GraphicsStateAndBindings.md),
[compute pipeline contract](../Runtime/Rendering/SynchronousComputePipelines.md),
[global shader contract](../Runtime/Rendering/GlobalShaders.md), and
[module ownership](../Workspace/CodeModules.md) as behavior lands, not in advance.

The design borrows explicit logical layouts and backend preparation from
[NVRHI's binding guide](https://github.com/NVIDIA-RTX/NVRHI/blob/main/doc/ProgrammingGuide.md)
and [Vulkan binding implementation](https://github.com/NVIDIA-RTX/NVRHI/blob/main/src/vulkan/vulkan-resource-bindings.cpp).
It deliberately retains Durin's unified logical numbering instead of adopting
HLSL category offsets or treating NVRHI as a Metal implementation reference.
