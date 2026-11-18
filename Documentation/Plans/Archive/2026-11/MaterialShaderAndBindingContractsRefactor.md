# Material Shader and Binding Contracts Refactor Plan

Summary: Align Shader/VF ownership and parameter binding with UE's separation of semantic metadata, compiled allocation and runtime submission.

Last reviewed: 2026-11-17

Status: Archived
Completed: 2026-11-17

## Current Status

M15 of the [Material System roadmap](../../../Roadmaps/MaterialSystem.md). Stages 0-5
are complete: registered Shader/VF descriptions own compilation; composable
parameter-owner metadata, compiler allocation and typed submission replace the
intermediate global binding catalog. Material, ShaderBuild, Vulkan, Cook and
workspace consumer coverage and final Editor/Game `all` builds pass.

Stage 3 centralized the former global logical set/binding numbers. Stage 4 removes
that catalog and separates composable parameter metadata from compiler allocation.
Stage 5 completes isolated Cook/Game corpus execution, Windows rendering checks,
size/work observations and final publication on 2026-11-17.
Implemented contracts remain documented in
[Material shader stages](../../../Runtime/Rendering/Materials/MaterialShaderStages.md).

Stage 4 implementation now composes dynamic Material/collection metadata with
registered Shader/VF owner metadata. UE-style
`FShaderParameterMap`/`FParameterAllocation` and `FShaderParameterBindings` drive
shader-instance binding; generated metadata uses
`FShaderParametersMetadataBuilder`. Per-owner C++/Slang declarations replace the
global resource catalog and role enum. Accepted entries retain canonical schema
and product-bound allocation identities, actual compiler locations, dimensions,
array policy and constant layouts through shared ShaderBuild output, DDC and Cook.
Renderer submits typed providers through those accepted records and partitions
lifetimes by metadata scope. Stage 5 now validates an eight-material isolated Cook
corpus, allocation preservation, compatible-instance sharing and numeric updates
without source work in Editor and runtime-only Game test hosts. Cube, numerical texture WPO,
ClearCoat and PostProcess Vulkan checks pass on the current Windows environment.
The same corpus passes from a physically isolated exported Game runtime;
the Sandbox delivery passes its separate 30-tick Launcher verification.

The owner permits underlying API/derived-format changes. The preceding
[M14 plan](MaterialAttributesAndExecutionContractsRefactor.md) was completed with
its Stage 6 and native Metal acceptance explicitly skipped. Those skips are not
passes. Native Metal acceptance remains excluded from M15; the Stage 5
Windows integration gates pass without treating those exclusions as results.

## Goal and Boundaries

One registered Shader identity spans selection, compilation, Cook and runtime.
Material evaluation remains independent of pass entry bodies and geometry source.
Parameter metadata describes semantic fields and ownership; compiled output owns
the accepted mapping to actual resource locations. Renderer binds through that
mapping without reconstructing descriptor numbers from global material roles.

Preserve typed GUID attributes, functions, static selection, detached worker
inputs, ShaderBuild/DDC sharing, immutable publication, cancellation/stale-result
rejection, dynamic instances and source-free Game execution. Migrate all consumers
in `Durin.dworkspace` and selected plugins together. Reject incompatible derived
products and recook isolated outputs; authored content changes only if its source
schema changes. No parallel scheduler, compatibility registry or source fallback.
Compact uniform packing, preshaders, Water optics, Substrate and new domains remain
outside this plan.

## Selected Ownership and Transition

Local reference: `E:/Programs/Epic Games/UE_5.8`, UE 5.8.0, CL 53629095. Follow
responsibilities rather than binary layouts or every legacy path. Relevant
boundaries include `MaterialShaderType.h`, `MeshMaterialShaderType.h`,
`VertexFactory.h`, `ShaderParameterMetadata.h`, `ShaderParameterStruct.cpp`,
`ShaderCore.h` and Vulkan ShaderFormat's `SpirVShaderCompiler.inl`.

| Boundary | Selected contract |
| --- | --- |
| Material semantics | Engine owns attributes, schemas, normalized evaluation and analyzed reads. Workers receive detached values without authored-object queries. |
| Shader/VF composition | Registered types own applicability, permutations and entries; VFs own geometry source/environment. Actual type, target, permutation and applicable VF remain exact through Cook/runtime. |
| Parameter metadata | Reuse and extend RenderCore's existing `FShaderParametersMetadata` foundation. Material, Pass, VF and Scene owners contribute composable schemas with stable qualified field identity, kind, shape/array policy and required/optional semantics. Resource numbers do not belong to these semantic declarations. |
| Compilation and allocation | ShaderBuild matches metadata to compiler layout/reflection and emits an immutable parameter allocation map for each accepted Shader product. ShaderFormat owns backend remapping/compact allocation and target constraints. Compiler-assigned locations may differ across Shader types, permutations and targets. |
| Runtime binding | Renderer resolves semantic providers/structure members through the accepted map. Uniform submission policy derives from parameter metadata and pipeline/backend requirements rather than material-domain special cases. |
| RDG | Explicit parameters and declared reads determine graph dependencies and lifetimes. Binding numbers do not infer or manufacture RDG ownership. |
| Fixed conventions | Keep only justified root-layout/static-uniform conventions, named and localized to the responsible lower layer. They are explicit exceptions, not a global per-resource material slot table. |
| Publication and formats | Persist schema/allocation identity and accepted bindings with compiled content. Validate before publication; retain independent compilation-content and device-resource lifetimes. Bump affected identities/codecs together without compatibility readers. |

Stage 3's catalog, exact identity checks and negative-admission coverage are the
migration baseline. Retain useful semantic validation while replacing its global
role/slot dependency. Do not grow another allocation table alongside it. Freeze
exact C++/wire signatures and ownership before Stage 4 source edits; if a general
parameter-struct facility is missing, extend its existing owner instead of adding
a material-only replacement framework.

## Implementation Stages

### Stage 0: Select ownership and transition boundaries

- [x] Compare the local UE reference, select cross-module ownership and record M14's completion/waiver.

### Stage 1: Remove obsolete evaluation and cache paths

- [x] Require normalized property-root generation, remove the old aggregate/eight-input path, and lazily allocate independent fallback uniform caches; validate affected consumers.

### Stage 2: Unify Shader type and Vertex Factory compilation contracts

- [x] Replace fixed entry selection and aliases with registered detached Shader/VF policy, VF-owned geometry source and exact compile/Cook/runtime identity; validate an additional VF and migrated consumers.

### Stage 3: Unify binding metadata and runtime admission

- [x] Centralize current declarations/validation/mapping, publish canonical accepted binding references, reject incompatible products and validate material/render/Cook consumers. Fixed logical allocation remains transitional.

### Stage 4: Separate parameter metadata from binding allocation

Outcome: adding a parameter to a registered Pass/VF changes its owning schema,
not a central material role/slot list; compiled locations can change while runtime
submission remains correct through the accepted allocation map.

- [x] Freeze metadata composition, stable field identity, compiler allocation records, accepted-map validation and runtime submission signatures across RenderCore/Engine/ShaderBuild/ShaderFormat/Renderer; inventory all workspace and selected-plugin consumers.
- [x] Compose Material, Pass, VF and Scene parameter schemas through existing metadata facilities. Describe names/qualified paths, kinds, dimensions/arrays and usage/ownership without global per-resource set/binding assignments. Generate matching shader declarations; remove ordinal pass masks and central-enum edits as extension requirements.
- [x] Use compiler layout/reflection to construct the deterministic field-to-location allocation map, including target/type/permutation, schema identity, actual resource location/kind/array and relevant constant offsets. Keep numeric allocation/remapping in compilation/backend ownership; validate any named fixed-convention exceptions explicitly.
- [x] Persist the accepted allocation map through compiled content and Cook. Migrate Surface/PostProcess, Local/Spline/custom VF, shadows, hit proxies, PSO/preview/thumbnail and scene-resource consumers to bind via metadata and accepted maps. Remove slot arithmetic, reflection-to-global-role inference, domain-specific binding-kind switches and the obsolete fixed-slot catalog in the same migration.
- [x] Preserve analyzed material/collection/scene reads, required/optional rules, optimized-out fields, RDG lifetimes, atomic publication and source-free execution. Bump affected derived identities/formats together; reject missing/extra/wrong-kind/array, wrong schema/map identity and cross-product substitution before publication.
- [x] Prove extensibility with a new Pass/VF parameter without editing material core selection or a global resource enum. Prove allocation independence with differing locations across products and with unused fields optimized out; exercise runtime submission and source-free Cook replay through the accepted maps. Run owning/affected consumer coverage and stable Editor/Game workspace `all` builds for the shared API/layout migration.

### Stage 5: Validate integrated consumers and publish contracts

Entry gate: Stage 4's metadata/allocation/runtime migration and extension tests
pass; final acceptance does not substitute for implementing that boundary.

- [x] Validate owning and affected consumers across workspace projects, including isolated source-free Cook/Game execution for Surface, Cube/WPO, ClearCoat and PostProcess with final allocation maps and no shader-source/compiler fallback.
- [x] Satisfy the final workspace `all` gate for changed foundational headers/layouts/build inventory. Reuse Stage 4 Editor/Game results when relevant inputs and environment are unchanged; rebuild affected closure when they change.
- [x] Check rendering behavior, required resource/descriptor counts, parameter updates without compilation and compatible-instance sharing. Record actual size/work observations without claiming unmeasured timing acceptance.
- [x] Publish final ownership in Runtime/Rendering contracts, update the roadmap and record final tested inputs, selections and limits. Native Metal acceptance remains skipped by owner instruction and is not a passing result.

## Stage 4 Contract Freeze

### UE 5.8 interface alignment

Final Stage 4 migration selection: registered parameter members carry owned-schema
shader type and provider scope names in addition to kind/array/optional metadata.
The detached projection preserves these values. Built-in owners share declaration
lists between C++ metadata and Slang without descriptor numbers or pass masks.
Accepted entries retain the composed schema identity, named allocations and their
complete Shader/VF product identity. Renderer provider scopes and typed member
offsets replace the role enum. All Stage 4 gates passed on 2026-11-17; the receipts
below identify the tested working tree and selections.

Owner direction: align the main interfaces with the local UE 5.8 reference.
Existing Durin API, ownership and derived-format contracts may be changed as
needed; preserving them is not a prerequisite for the refactor. Migrate consumers
and invalidate incompatible derived products together when those contracts change.
The following selection supersedes the prototype naming and submission interface
in the initial freeze below; the remaining ownership and acceptance gates stand.

- Extend the existing `FShaderParametersMetadata` foundation with member shape,
  nested/included structures, use-case and layout identity. Follow
  `ShaderParameterMetadata.h` and `FShaderParametersMetadataBuilder` in
  `ShaderParameterMetadataBuilder.h` for dynamically generated Material/collection
  structures. Detached worker metadata is an owned projection of this facility,
  not a separately authored schema system.
- Use `FShaderParameterMap` and `FParameterAllocation` as the main compiler-output
  interfaces, following `ShaderCore.h`: named allocation lookup, kind, resource
  location and constant size/offset. Backend remapping remains below this layer;
  retain Durin's explicit set/binding representation where needed instead of
  copying UE's register field widths or interpreting register indices as sets.
- Add `FShaderParameterBindings` as the compiled bridge from metadata member byte
  offsets to allocation records, following `Shader.h` and
  `ShaderParameterStruct.cpp`. Binding validates the metadata/map once; typed
  runtime submission traverses accepted records rather than repeating semantic
  string lookup for every draw. Distinguish inline values, resources and uniform
  references. Included structs keep the enclosing shader-name prefix; nested
  structs extend it and add their C++ member offset.
- Preserve the established typed `SetShaderParameters` and prepared-batch paths.
  Material, Pass, VF and Scene populate their owning parameter structures/providers
  before the common binding layer. The prototype's name-vector provider resolver
  is not the selected per-draw submission interface.
- Keep complete product/schema/allocation identity and fail-closed Cook admission
  in Durin's accepted-product envelope. UE's mutable `bBound` bookkeeping is not
  part of immutable published identity. Keep explicit RDG read/lifetime ownership.

Reference inspected locally on 2026-11-17: UE `ShaderCore.h` allocation/map
declarations, `ShaderParameterMetadata.h` member/use-case declarations,
`ShaderParameterMetadataBuilder.h` included/nested builders, `Shader.h` binding
records and `ShaderParameterStruct.cpp` recursive member binding. This selects
responsibilities and major API vocabulary; UE binary layouts, UObject machinery,
bindless infrastructure and legacy entry points are not required dependencies.

This section records the selected implementation interfaces. The owning Runtime
contracts now describe their implemented behavior. The prototype interfaces below
remain historical design context where superseded by the UE-aligned selection.

### Metadata and composition

Implementation refinement: the initial resource-only allocation API groups target,
type, VF, permutation, frequency and the complete caller compile-key digest in
`FShaderParameterProduct`. Its validator receives this expected product explicitly;
this prevents a self-consistent map from authorizing a different compiled product.
Constant offsets and shape reflection must be added before material integration;
the initial builder rejects inline allocation instead of fabricating offsets.

The existing `FShaderParametersMetadata` and member declarations live in RHI's
`RHIShaderParameters.h`; RenderCore owns composition, reflection matching and
submission in `ShaderParameters.cpp`. Extend these owners instead of creating a
second material parameter system. Preserve the existing aggregate initializers
and macros during migration by appending new semantic members with defaults.

Add an owned, detached `FShaderParameterSchema` in RenderCore, containing a
`uint32 Version`, `FXxHash128 Identity`, and a vector of
`FShaderParameterField`. Each field contains owned `std::string QualifiedName`
and `ShaderName`, `EShaderParameterMemberKind Kind`, `ERHIBindingType Type`,
`uint32 ArraySize`, `uint32 ValueSize`, `uint32 ValueAlignment`, and
`bool bOptional`. Texture dimensionality must be explicit in the field shape;
samplers, buffers and textures cannot match solely through a broad resource kind.
Neither schema nor member metadata contains descriptor set/binding numbers.
Provider ownership is the qualified schema path, not another closed owner enum.

Extend metadata with nested-structure metadata and a qualified namespace.
Composition recursively adds parent byte offsets for C++ access and namespace
segments for semantic identity. Reject cycles, duplicate qualified names,
duplicate emitted shader names, empty segments, zero/unbounded arrays, invalid
shape and out-of-bounds member storage. The detached schema excludes C++ pointers,
byte offsets, padding and graph-resource addresses. Those remain process-local
access metadata. Existing included metadata must flatten once, with consistent
inherited offsets; runtime submission must no longer reject every included struct.

Use these RenderCore signatures, returning diagnostics rather than assertions
for detached/compiler/Cook inputs:

```cpp
auto BuildShaderParameterSchema(const FShaderParametersMetadata& Metadata)
    -> std::expected<FShaderParameterSchema, std::string>;
auto ComposeShaderParameterSchemas(std::span<const FShaderParameterSchema> Schemas)
    -> std::expected<FShaderParameterSchema, std::string>;
```

Canonical order is bytewise qualified-name order. Hash versioned, length-prefixed
UTF-8 names and fixed-width semantic values in that order; never hash object
memory. Material fields use stable authored GUID segments for texture/collection
identity. Registered Pass/VF descriptions supply their own schemas; Scene owns
the scene schema and providers. Applicability is registered policy, not an ordinal
entry mask. Engine generates the detached material fields from analyzed reads.
ShaderBuild emits declarations from the composed schema and its shader names.

### Compiled allocation and admission

Add `FShaderParameterAllocation` with `std::string QualifiedName`,
`uint32 SetIndex`, `uint32 BindingIndex`, `ERHIBindingType Type`,
`uint32 ArraySize`, `uint32 ByteOffset`, and `uint32 ByteSize`.
Constant offsets are compiler offsets within the allocated constant buffer;
they are never C++ structure offsets. Add `FShaderParameterAllocationMap` with
`uint32 Version`, `FXxHash128 SchemaIdentity`, `FXxHash128 Identity`,
`FShaderTargetIdentity Target`, `std::string ShaderType`,
`std::string VertexFactoryType`, `uint32 PermutationId`,
`EShaderFrequency Frequency`, and a vector of allocations. Empty VF identity is
valid only for a product whose registered type does not require a VF.

ShaderBuild constructs maps from the schema plus final compiler reflection.
ShaderFormat owns backend numeric remapping; reflection, emitted code and the
persisted map must describe the same final locations. Allocation identity hashes
all product identity and canonical allocation records. Engine admission also
compares the complete existing material shader compile key, including source and
output-layout identity; the allocation hash cannot replace that comparison.

RenderCore exposes `ValidateShaderParameterAllocationMap(const
FShaderParameterSchema&, const FShaderParameterAllocationMap&, const
FShaderReflectionData&) -> FShaderOperationResult`. ShaderBuild supplies final
reflection to this validator; Cook reads the persisted reflection and map through
the same validator. Reject extra fields, duplicate fields/locations, wrong kinds,
shapes or arrays, missing required fields, invalid constant ranges and mismatched
schema/map hashes. Optional fields may disappear when optimized out; no fake
allocation or mandatory runtime provider is synthesized. Required fields must
remain reflected. Backend aliases are allowed only through a named ShaderFormat
rule validated against emitted code. No fixed per-material-resource convention
is grandfathered into the new contract.

### Submission and persistence

Use `FShaderParameterProvider` containing owned `std::string QualifiedName` and
a vector of `FRHIShaderParameterResource` values. Provider-supplied location
numbers are ignored: only the accepted allocation determines the submitted
set/binding. Array element, resource range and lifetime still require validation.
RenderCore exposes:

```cpp
auto ResolveShaderParameterAllocations(
    const FShaderParameterSchema& Schema,
    const FShaderParameterAllocationMap& AcceptedMap,
    std::span<const FShaderParameterProvider> Providers)
    -> std::expected<std::vector<FRHIShaderParameterResource>, std::string>;
```

Resolve only after product admission. Reject duplicate providers, absent required
allocated resources, null required resources, incompatible types/array lengths
and invalid ranges. Providers for declared optimized-out fields may be unused;
unknown provider names are errors. Therefore submission retains the admitted
schema alongside its map to distinguish unused declared fields from unknown
ones. Prepared batches retain independent RHI references through
the existing immutable `FRHIShaderParameterBatch` path. Uniform dynamic/static
submission derives from metadata policy and pipeline requirements in lower
layers, with no material-domain switch. RDG reads and handles stay explicit and
are validated before conversion into providers; allocation never adds graph reads.

Persist the full schema and map with each accepted compiled product using bounded,
field-wise serialization, not native struct serialization. Replace the current
`Entry.Bindings` role/index sequence; do not retain a compatibility reader.
Version the generic Shader codec if its payload changes and bump DMAT, pass ABI,
translation/generator and compile adapter identities together when switching the
material pipeline. Existing physical uniform packing remains version 5 unless
its actual representation changes. Recook isolated outputs; reject the old
binding-schema-1 products before immutable publication. Never write device
resource references or process-local access offsets into Cook content.

### Migration inventory and validation closure

Inventory scope is all three `Durin.dworkspace` projects: Engine, Sandbox and
RoadWeaver. The production plugin found in the workspace is `Engine/Plugins/Water`;
the PluginFixture plugins are test fixtures, not new rendering providers.

| Owner / consumers | Required migration |
| --- | --- |
| RHI `RHIShaderParameters.h`; RenderCore `Shader.h`, `ShaderParameters.cpp`, compiler values and codecs | Extend existing metadata, owned detached schemas, canonical validation, accepted maps, nested access and immutable submission; preserve global shader/RDG consumers. |
| Engine `MaterialBindingSchema`, `MaterialProgramGenerator`, `MaterialCompiledLayout`, `MaterialCompilationOutput`, its serialization and `MaterialShaderMap` | Replace catalog-generated slots and reverse role inference; compose analyzed material reads with registered owner schemas; persist/admit exact maps. Delete obsolete catalog and enum only after consumers migrate. |
| ShaderBuild compiler/reflection, generated requests, shared output/DDC and cooked Shader library; ShaderFormat and native backends | Reflect final locations, backend remapping and canonical target constraints; include schema identity in requests/cache keys and preserve detached source closure. |
| Renderer `SurfaceMaterial`, `MeshRendererShared`, `PostProcessRenderer`, `MaterialSceneTextures` | Build semantic providers for Surface/PostProcess, view/primitive, lighting, environment, shadow, hit proxy and scene reads; remove slot arithmetic and domain-specific uniform switches. |
| Slang `Material/MaterialBindingSchema.inl`, `SurfacePassResources`, `SurfacePasses`, `SceneTextures`, `SceneData`, `StaticMeshBasePass`, `VertexFactory/SplineMaterialEntry` | Generate owner declarations without fixed slots, preserve Local/Spline geometry and stage applicability. |
| MaterialEditor, StaticMeshEditor, Renderer PSO/preview/thumbnail paths and GameExport | Consume the same accepted product/map; preserve preview lifecycle and source-free Cook admission. |
| Water resources/components/types; RoadWeaver scene/viewport integration; Sandbox | Validate material provider publication and compatible-instance sharing; compile all project/plugin consumers even where no direct old-catalog symbol occurs. |
| RenderCore foundation/reflection, Engine material compiler/layout/runtime, ShaderBuild contracts/feature/Cook, Vulkan/PSO and editor tests | Extend rejection fixtures and source-free replay; add registered Pass/VF fields without changing Engine selection or a global enum, verify differing compiled locations and optimized-out optional fields. |

The initial symbol inventory found direct old-catalog references in Engine source,
Slang and material compiler tests; no direct references in Sandbox, RoadWeaver or
Water source. This does not exempt their indirect integration coverage. Stage 4
acceptance requires both Editor/Game workspace `all` builds because the migration
changes shared metadata, compiler values and serialized layouts. Stage 5 retains
the isolated final Surface/Cube/WPO/ClearCoat/PostProcess Cook/Game corpus and GPU
behavior/resource-count checks; earlier Stage 3 receipts cannot cover these edits.

## Completed Validation Summary

### Stage 5 final integrated acceptance

Validated on 2026-11-17 from `c6e27ab9f` plus the integrated Cook/Game tests,
test registration and documentation changes, windows-msvc-x64, Debug/Tracy,
MSVC 19.44.35228.0,
18 build jobs. Current GPU execution uses RTX 3090, Vulkan 1.4.351 and SDK
1.4.357.0 validation layers. Production source, formats and authored content are
unchanged from Stage 4. Its owning/consumer results remain relevant; the current
checkpoint adds the following evidence rather than repeating that entire set.

- `MaterialCookTests` passes all 7 cases (`20261117-232009-545097-40816-MaterialCookTests.log`). Its integrated corpus covers Specular/DefaultLit, ClearCoat, Unlit, neutral/color/depth PostProcess, Cube and explicit-LOD Cube WPO. Editor/Game selections retain identical common shader code and Game excludes HitProxy. Replay renames the authored package tree out of reach, strips expressions/IR/generated source and preserves complete accepted entries, schema/map identities, actual allocations and code bytes. Six variants with active color fields exercise independent dynamic-instance updates and shared accepted programs. Shader compilation, dependency resolution and content-read counters do not increase during replay; instance creation/updates do not add material preparation requests.
- Cube Cook fixtures use the declared null-Cube fallback. Real Cube faces, face-local orientation, mip selection, replacement/fallbacks and numerical Texture2D/Cube vertex displacement are covered by `StaticMeshRenderPreparationVulkanTests` cases `CubeMaterialFacesMipsInstanceReplacementAndFallbacks` and `ExplicitTextureLODVertexDisplacementMatchesNumericalDepth`. The same bounded run passes `ClearCoatMatchesForwardDeferredDirectAndEnvironmentReferences`: 3 cases, `20261117-232127-388210-43320-StaticMeshRenderPreparationVulkanTests.log`.
- `MaterialSceneTexturesVulkanTests` passes its source-free scene-input case, including filtering, subrects, resize, depth conventions and RDG lifetime ownership (`20261117-232204-914909-35308-MaterialSceneTexturesVulkanTests.log`). Both GPU receipts contain no Vulkan validation errors.
- `cook-contracts.csv` records package bytes / shader bytes / retained-map bytes / summed accepted binding records respectively: Specular 171626/160292/180837/23; ClearCoat 172539/161136/181745/23; Unlit 35485/27296/41390/12; PostNeutral 10777/6280/11015/4; PostColor 11321/6600/11857/5; PostDepth 15045/10200/15698/6; Cube 37281/28060/43012/14; CubeWPO 40380/30392/46964/18. Binding counts sum all accepted entries, not simultaneous GPU descriptors. Retained sizes are the map's accounting; shared storage may overlap across maps. These observations establish no timing threshold or performance comparison.

- The final optional corpus export additionally writes the Game Shader library; its selected case passes in `20261117-232714-379355-21812-MaterialCookTests.log`. The other six Cook cases are reused because only that case's optional export block changed. `MaterialGameReplayTests` is registered only for Win64 Game and links runtime owners without ShaderBuild. It loads all eight packages, compares identities, stage bytes and aggregate accepted binding counts with the CSV, checks schema/map identities and stripped debug data, and exercises shared programs and numeric updates. Cooked Shader mode forbids ShaderBuild; neither ShaderBuild nor ShaderFormat is an active module.
- `DevTool export --project Sandbox/Sandbox.dproject --output-dir Build/AgentValidation/MaterialShaderAndBindingContracts/Stage5/SandboxExport --configuration Debug --verify` completes with `ExportComplete.json` and a verified 30-tick staged Launcher run. A separate copy of that delivery, with the Game replay executable and isolated corpus, passes the eight-material replay from `IsolatedGameReplay/Binaries/Win64/MaterialGameReplayTests.exe` (`20261117-233445-006351-31124-MaterialGameReplayTests.log`). Its tree contains no shader sources, C++ sources/headers, ShaderBuild or Slang compiler binaries. This combines material-specific runtime checks in the Game native host with actual Launcher deployment/startup coverage; it does not claim full-image pixel comparisons from the Launcher.
- Final workspace `all` builds pass for Editor (`20261117-233513-128548-21500-cmake.log`) and Game (`20261117-233525-675926-23832-cmake.log`). Earlier full dependency updates also passed; these final incremental runs include the Game-only test inventory and Editor exclusion. Test-CMake changes conservatively expand `affected` to the Engine test project. The bounded Cook/Game and GPU selections above cover the new fixtures; no production dependencies or APIs changed. Stage 4's unchanged owning/workspace consumer results are retained, with current SDK/build-graph closure supplied by these builds.

Initial Game fixture attempts exposed undeclared includes, mount setup after
manifest admission, incomplete resource preparation and an invalid blanket assertion
that every optimized entry has bindings. The final fixture declares dependencies,
sets mounts before admission, explicitly finishes material preparation and compares
actual accepted aggregate counts. It shuts Renderer down while render-command
admission remains open. Failed attempts and process exits are not passing evidence.
The first export attempt rejected a stale component receipt; rebuilding
`ExportComponent` refreshed it before the successful export.

Raw logs, source hashes, reproduction selections and copied Cook outputs are under
ignored `Build/AgentValidation/MaterialShaderAndBindingContracts/Stage5/`.
Set `DURIN_MATERIAL_STAGE4_CLOSURE_EVIDENCE_DIR` to that absolute directory when
running the Cook target to emit the CSV and packages; the existing environment
variable name is retained. For Game replay, set `DURIN_MATERIAL_GAME_CORPUS_DIR`
to the directory containing `Cooked/`, `Shaders/` and the CSV, then run
`DevTool test MaterialGameReplayTests --preset Win64-Debug-DurinGame`.
Set `DURIN_MATERIAL_GAME_REPLAY_EXECUTABLE` to a copy beside exported Game DLLs
to repeat installed execution; the registered runner launches it with a bounded
60-second lifetime and forwards its output. These correctness receipts establish
no timing acceptance, Shipping qualification or clean-machine prerequisite result.
Native Metal remains skipped by owner instruction, not passed.

### Stage 4 owner metadata and compiler allocation acceptance

Validated on 2026-11-17 from base `b1a2db156` plus the final migration. Exact
changed-file SHA-256 inputs, selections and raw logs are recorded under ignored
`Build/AgentValidation/MaterialShaderAndBindingContracts/Stage4/OwnerMetadata/`.

- Final routine selection: `@domain=material+shader+renderer+viewport+static-mesh+asset-cook+asset-package+thumbnail+road-graph+road-scene+gameplay+asset-import`, 71 targets. The 20261117-191808 CTest receipt passes 70 targets; its single thumbnail warm-counter failure was repaired with an explicit cold-job drain. The complete `ThumbnailVulkanTests` target then passed all 14 cases in 20261117-192319. Only that fixture and documentation changed after the broad run; the other 70 passing results are reused.
- Coverage includes Material compiler/runtime/lifecycle/editor/Cook, generic schema/allocation and ShaderBuild/DDC codecs, isolated source-free Shader library replay, Renderer/RDG, Local/Spline and custom geometry, PSO/preview/thumbnail, PostProcess scene/depth pixels, Water Vulkan integration, scene import, RoadWeaver and Sandbox consumers. GPU execution used the Windows Vulkan lane; native Metal remains excluded. The synthetic Metal codec test validates persistence, not native Metal execution.
- The registered M15 VF adds required `M15Offset` and optional `M15Unused` through its own shared declaration and metadata. Tests verify the required allocation and typed submission, removal of the unused allocation, exact Shader/VF/permutation rejection and Cook replay without editing core material selection or a global resource enum. Relocation to explicit set 2/binding 11 survives admission and Cook; corrupted kinds, arrays, dimensions, constant layout, schema/map identity and cross-product substitutions reject.
- Fixed-slot reflection assertions were replaced with name/type and reflection-to-pipeline agreement. Codec golden bytes moved with DSHD v7. Scene-input and independent geometry fixtures consume accepted allocations; forged-target Cook fixtures now reject at encode admission. Warm thumbnail counting waits for the cold Shader jobs it initiated, preserving the no-further-compilation assertion.
- `test affected --explain` expanded beyond semantic coverage because shared test headers select the Engine test project. The bounded domain union above covers the changed owners and consumers. The broader ABI/build-graph risk is closed by workspace `all` builds for `Win64-Debug-DurinEditor` (20261117-192354) and `Win64-Debug-DurinGame` (20261117-192451), both passing with 14 build jobs.
- Exact-symbol inventory across Engine, Sandbox and RoadWeaver source/tests/shaders finds no old catalog, role enum, binding-definition adapter or declaration pass masks. Changed-document validation and `git diff --check` pass.

Formats are material binding schema 3, generic parameter schema/map 2, pass ABI 25,
DMAT 28, detached translation 17, generator 29, material adapter 26, DSHD 7 and
shared Shader output 8; ShaderBuild compile/reflection revisions are 7/4. Physical
uniform packing remains 5. This Stage 4 receipt did not close Stage 5's isolated
final material corpus and publication gates; final evidence is recorded above.

### Test-contract cleanup during the allocation migration

Owner requested a review of tests that no longer match the architecture.
Material compiler tests no longer require View/Material/lighting/HitProxy to
occupy particular global descriptor numbers. Resource kinds, analyzed read
closure, required resources and immutable admission remain checked; the dedicated
binding-admission suite owns negative map/name/kind/array coverage. A real
compiler relocation and Cook replay case now checks location independence.
Renderer fixtures supply explicit semantic names and verify relocated submission
and provider-lifetime selection. Instance selection explicitly declares its
accepted location instead of relying on the old set 0/binding 31 convention.

Cook, import and reload fixtures now declare/load their Renderer dependency so
registered material Shader/VF types exist, and close Renderer while render-command
admission is open. The level-residency test checks released handle ownership and
collection after an explicit GC; it no longer requires releasing a handle to set
a global immediate-GC request flag, which is absent from the current residency
contract. No production GC policy was changed. Required binding rejection,
source-free Cook, resource retention and collection coverage were retained.

Compiler-location checkpoint, 2026-11-17, base `f5e0c20bb` plus this migration:
binding schema 2, pass ABI 24, DMAT 27, translation schema 16 and material compile
adapter 25. MIR 15, physical layout 5 and normalization/generator revisions 16/28
remain unchanged because authored semantics, uniform packing and default generated
declarations did not change. Old derived containers reject; no compatibility reader
was added. The new test recompiles the Material uniform at set 2/binding 11 and
checks accepted records, Cook encode/decode and rejection of altered locations.

On windows-msvc-x64, Debug/Tracy, MSVC 19.44, 14 jobs, the initial affected run
selected 62 targets: 56 passed and six failed. All six were rerun successfully
after the test-contract/fixture migrations: CookFunctional 45, SceneImport 38,
AssetPackageReload 22, MaterialCompiler 122, RendererSceneContract 174 and
Viewport 119 cases. RoadSceneIntegration passed 5 cases; the initial run covered
Sandbox and Water consumers. The 56 passing results are reused: subsequent
production edits only make failed binding preparation publish no partial instance
location; successful submissions are unchanged, and the final Renderer contract
run covers preparation/failure behavior. Remaining edits change only the six
rerun fixtures/tests, their explicit test dependencies and documentation.
Test-CMake edits conservatively expand `affected` to the whole Engine test project;
that label expansion does not add a production dependency or execution change to
the other targets.

Final Editor/Game workspace `all` builds passed in
`20261117-171522-974606-16952-cmake.log` and
`20261117-171541-741721-24904-cmake.log`. Raw logs and final source hashes are under
ignored `Build/AgentValidation/MaterialShaderAndBindingContracts/Stage4/CompilerLocations/`.
The first link attempt used a non-exported internal acceptance function from a
test; the test now uses the exported admission validator. Intermediate fixture
shutdown failures were corrected by closing Renderer with render-command admission
open; passing case output with a failed process exit was never counted as a pass.
This checkpoint does not complete owner-schema composition, catalog removal,
uniform submission policy migration or Stage 5's final packaged corpus.

UE interface foundation validation, 2026-11-17: base `f7091a4d2` plus the
uncommitted parameter foundation, on windows-msvc-x64, Debug/Tracy, MSVC 19.44,
14 jobs. `RenderShaderContractTests` passed 79 cases; after migrating pipeline
uniform policy to compiled binding records, `test affected` passed all 15 targets
again (`20261117-163514-026361-18388-ctest.log`). Editor and Game workspace `all`
passed (`20261117-163605-392684-38232-cmake.log` and
`20261117-163812-183184-14724-cmake.log`). The final builds include removal of the
unused binding compatibility helper; no executable source changed after them.
Source hashes and copied receipts are under ignored
`Build/AgentValidation/MaterialShaderAndBindingContracts/Stage4/UEFoundation/`.
These results cover the Map/Bindings/Builder API migration and nested resource
submission, not the still-pending replacement of the material catalog, constant
and texture-shape reflection, new material codecs or Stage 5's final corpus.

Stage 4 foundation validation (not Stage 4 acceptance): on 2026-11-17, base
`f7091a4d2` plus the RHI/RenderCore schema/allocation changes, Win64 Debug/Tracy,
MSVC 19.44 and 14 jobs, `RenderShaderContractTests` passed all 76 cases, including
three new schema/allocation cases. `test affected` passed all 15 selected targets,
covering RHI commands/resources, RenderCore contracts, ShaderBuild/cache,
source-free Shader Cook/library and Vulkan integration. Receipts are
`20261117-161838-292660-3884-RenderShaderContractTests.log` and
`20261117-162009-025213-44720-ctest.log`; source SHA256 values and copies of the logs
are under ignored `Build/AgentValidation/MaterialShaderAndBindingContracts/Stage4/Foundation/`.
An initial sandbox C1902 PDB-service failure was superseded by successful
execution outside the sandbox. No Editor/Game workspace `all` or final
material-allocation/Cook corpus acceptance is claimed for this foundation.

Stages 1-3 were validated on 2026-11-17, windows-msvc-x64, Debug/Tracy,
MSVC 19.44, 14 jobs, with the applicable Win64-Debug-DurinEditor/Game presets.
GPU correctness used GTX 1060 6GB / Vulkan 1.4.312 and SDK 1.3.296.0 validation
layers. These are stage-specific receipts, not validation of future Stage 4.

| Stage | Recorded input and coverage | Primary receipts |
| --- | --- | --- |
| 1 | `7c67b81a5` plus cleanup; 13 material targets with the compiler caller repaired and 119 compiler cases passing; focused Vulkan/Shader/cross-project tests and Game Engine build. No workspace `all` or packaged Game claim. | `20261117-112905-455223-41052-ctest.log`; compiler `20261117-113129-874401-46248-MaterialCompilerTests.log`. |
| 2 | `cfab588fb` plus migration; 13 material targets/120 compiler cases, Shader feature/contracts, Vulkan/PSO, Shader Cook, editor and both project consumers; Editor/Game `all`. | `20261117-132935-292706-26172-ctest.log`; all builds `20261117-135704-970985-19708-cmake.log` and `20261117-135736-053111-27936-cmake.log`. |
| 3 | `304b580d8` plus binding migration; 13 material targets/121 compiler cases, Shader feature/contracts, 27 Vulkan/PSO cases, scene/PostProcess/reload, source-free Shader Cook, 93 editor cases and both project consumers; Editor/Game `all`. Final binding case adds runtime, canonical-order and schema-version rejection. | `20261117-150227-586217-12000-ctest.log`; final binding `20261117-151829-091999-37128-MaterialCompilerTests.log`; all builds `20261117-152015-749710-38352-cmake.log` and `20261117-152052-050199-36288-cmake.log`. |

Raw logs/source hashes remain under ignored
`Build/AgentValidation/MaterialShaderAndBindingContracts/Stage1/`, `Stage2/` and
`Stage3/`; the latter two contain `TestedInputs.json`. Earlier failed caller,
include-path and fixture attempts were corrected; the recorded passing receipts
supersede them. Valid-input evidence was reused only where later edits preserved
relevant behavior; final binding/Cook/consumer runs and all builds cover the last
Stage 3 guards and equivalent runtime projection changes. No native Metal,
packaged final corpus or timing qualification is claimed.

The Stage 3 baseline is binding schema 1, pass ABI 23, DMAT payload 26,
translation schema 15, normalization/generator revisions 16/28 and compile adapter
24. Physical uniform packing remains version 5; Stage 4 must version its own
incompatible transition. Authored schemas and the workspace descriptor were
unchanged in completed stages. Detailed implemented contracts live in
[MaterialShaderStages](../../../Runtime/Rendering/Materials/MaterialShaderStages.md) and
[MaterialSystem](../../../Runtime/Rendering/Materials/MaterialSystem.md); Git history
preserves the expanded completed-stage notes.

Follow the [build](../../../Agents/BuildAndRun.md),
[test selection/result reuse](../../../Agents/Testing.md) and
[documentation](../../../Agents/Documentation.md) workflows. Keep raw evidence under
ignored `Build/AgentValidation/MaterialShaderAndBindingContracts/`.
