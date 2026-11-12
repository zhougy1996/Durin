# UE-Aligned GPU Scene Plan

Summary: Introduce persistent Primitive and Instance scene data, shared mesh-command bindings, and pass-local instance submission using UE-aligned interfaces.

Last reviewed: 2026-11-12

Status: Active
Completed:

## Current Status

Stage 3 implementation now includes provider-declared persistent/dynamic identity,
effective-state command sharing and ordered CPU instancing for compatible Local
opaque/masked and shadow draws. Per-view transform/payload comparisons and the
ComponentId lookup map have been removed. Sources retain independent cache
invalidation slots while equivalent commands share state buckets.

At the user's request, closeout is limited to the final scene contracts, Vulkan
regressions, workspace compilation and document checks. The extended validation
matrix remains open; Stage 3 is not marked fully qualified. Stages 4-5 remain open,
including removal of legacy GBuffer culling inputs. No timing speedup is claimed.
Implemented rules live in [GPUScene storage and submission](../Runtime/Rendering/Scene/GPUScene.md).

### Stage 1 validation receipt

Tested inputs are base `b747a0eef` plus the Stage 1 source/fixture changes, hashed
in `Build/Qualification/UEAlignedGPUSceneStage1Inputs.json`. Subsequent changes
are documentation only and do not invalidate these runtime results. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, application-hosted tests off.
The sandboxed initial build hit the documented MSVC C1902 PDB-service failure;
the same target built successfully outside the sandbox. No clean build was needed.

| Selection | Result and evidence |
| --- | --- |
| `RendererSceneContractTests` | 154/154, including nine new GPUScene cases: packing/large origins, hidden/default/empty instances, stable reorder, one-payload patches, scene mutation hooks, abandoned/compile-failed retries, allocation rejection, growth/recovery and generation/GPU-safe reuse. Log: `Build/.agent-state/logs/20261112-192356-437508-17872-RendererSceneContractTests.log`. |
| `RenderContractTests` | 246/246 RenderCore/RDG/RHI contract cases. Log: `Build/.agent-state/logs/20261112-192412-740631-24828-RenderContractTests.log`. |
| `SandboxGameplayTests` | 13/13; Sandbox consumer compiled and passed. Log: `Build/.agent-state/logs/20261112-192440-461925-34220-SandboxGameplayTests.log`. |
| `RoadSceneIntegrationTests` | 5/5; RoadWeaver consumer compiled and passed. Log: `Build/.agent-state/logs/20261112-192527-731592-7744-RoadSceneIntegrationTests.log`. |
| `StaticMeshRenderPreparationVulkanTests FGPUSceneVulkanTests.*` | 1/1: ten successful native submissions and one rejected graph; 30 exact buffer-to-texture readbacks match Primitive/Instance/payload mirrors and zero capacity padding through patches, growth and recovery. Log: `Build/.agent-state/logs/20261112-192250-391512-30736-StaticMeshRenderPreparationVulkanTests.log`. |
| Existing Vulkan render regressions | 3/3: `FMaterialCustomDataVulkanTests.*`, `FMaterialAnimationVulkanTests.CachedCommandsInvalidateResourceChangesAndReuseNumericUpdates`, and `FStaticMeshRenderPreparationVulkanTests.HitProxyIdsRespectDepthBackgroundAndCacheInvalidation`. Log: `Build/.agent-state/logs/20261112-192704-915188-9196-StaticMeshRenderPreparationVulkanTests.log`. |

Native Vulkan used NVIDIA GeForce GTX 1060 6GB, API 1.4.312, threaded RHI,
with `VK_LAYER_KHRONOS_validation` loaded; the passing logs contain no validation
errors. CPU fixtures exercise inline replay and deliberately delayed completion.
No Metal execution or timing qualification was performed on this Windows host.
The final `all`/cross-backend shader migration gates remain assigned to Stage 5.
Affected-test discovery expands new test sources to the entire Engine test
project; the selected owning suites and both workspace consumer targets bound
Stage 1's actual behavior/API changes without running unrelated asset/editor tests.

This plan selects the architecture below. Runtime documents continue to describe
implemented behavior until the corresponding migration and validation land.
The completed indirect-culling work does not establish GPUScene support; its
historical scope and qualification remain valid for its tested implementation.


### Stage 2 validation receipt

Tested inputs are base `ac1bb114e` plus Stage 2 source/fixture changes, recorded
in `Build/Qualification/UEAlignedGPUSceneStage2Inputs.json`. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`; builds run outside the
sandbox for the established MSVC PDB-service limitation. Application-hosted
tests remain off. Later documentation edits do not invalidate these receipts.

| Selection | Result and evidence |
| --- | --- |
| `StaticMeshRenderPreparationVulkanTests` | 21/21: exact packed-buffer readbacks, direct/grouped/individual-reference pixels, selected-instance ObjectPosition in fragments, WPO/custom channels/masked shadows, Local/Spline mirrored normals, picking and approximately 1e12-origin depth, resource/numeric cache changes and independent/custom VF fallback. Log: `Build/.agent-state/logs/20261112-202056-727674-1556-StaticMeshRenderPreparationVulkanTests.log`. |
| `MaterialCompilerTests` | 103/103, including active/reserved stage order, stage sharing, selected pixel scene-buffer reflection, strict dependency closure and detached/Cook round trips. Log: `Build/.agent-state/logs/20261112-202154-641504-37196-MaterialCompilerTests.log`. |
| `MaterialCookTests` | 6/6: Game five-stage products, package loading, binary/version rejection and stripped authoring data. Log: `Build/.agent-state/logs/20261112-202356-169136-29088-MaterialCookTests.log`. |
| `MaterialCompileLifecycleTests` | 29/29. Log: `Build/.agent-state/logs/20261112-202422-570002-7476-MaterialCompileLifecycleTests.log`. |
| `RendererSceneContractTests` | 155/155, including persistent direct-ID buffer reuse, scoped dynamic records, unsupported factory fallback and unchanged persistent uploads. Log: `Build/.agent-state/logs/20261112-202502-918745-6136-RendererSceneContractTests.log`. |
| `RenderContractTests` | 246/246. Log: `Build/.agent-state/logs/20261112-202507-805874-38740-RenderContractTests.log`. |
| `RenderShaderContractTests` | 72/72: fixed vertex resource/layout ABI and exact descriptor checks. Also repaired a pre-existing deferred-light reflection assertion that omitted its existing LocalLights binding. Log: `Build/.agent-state/logs/20261112-202804-279518-40380-RenderShaderContractTests.log`. |
| `RenderShaderCookIntegrationTests` | 1/1. Log: `Build/.agent-state/logs/20261112-202818-838743-9576-RenderShaderCookIntegrationTests.log`. |
| `SandboxGameplayTests` / `RoadSceneIntegrationTests` | 13/13 and 5/5. Logs: `Build/.agent-state/logs/20261112-202833-438892-14904-SandboxGameplayTests.log`, `Build/.agent-state/logs/20261112-202849-009975-30464-RoadSceneIntegrationTests.log`. |

The incremental workspace `all` build passed in 27.27 seconds after the shared
ABI migration. Log: `Build/.agent-state/logs/20261112-202903-792412-4784-cmake.log`.
This covers remaining editor/tool/provider compilation, not unexecuted behavior
or a clean rebuild. Reuse the receipt only while relevant inputs remain unchanged.

Native Vulkan uses NVIDIA GeForce GTX 1060 6GB, API 1.4.312, threaded RHI with
`VK_LAYER_KHRONOS_validation`; the passing full-suite log has no validation errors.
CPU ownership/retirement fixtures use inline recording and delayed GPU completion.
Metal execution and native Metal compilation are unavailable on this Windows host;
its rejection policy is tested, while cross-backend qualification remains Stage 5.
No timing qualification or rendering speedup is claimed. Affected-test discovery
expands shared Engine/RenderCore/Renderer changes to unrelated asset/editor suites;
the selected suites cover scene access, mesh submission, material products and all
three workspace projects. The workspace-wide build checks remaining API consumers.



### Stage 3 focused validation receipt

Tested inputs are base `4086c3536` plus the Stage 3 changes recorded in
`Build/Qualification/UEAlignedGPUSceneStage3Inputs.json`, profile `windows-msvc-x64`,
preset `Win64-Debug-DurinEditor`, application-hosted tests off. Builds run outside
the sandbox for the established MSVC PDB-service limitation. Documentation-only
closeout edits do not invalidate these results.

| Selection | Result and evidence |
| --- | --- |
| `RendererSceneContractTests` | 156/156. Explicit stable Instance IDs are independent of cache BatchId; invalid persistent references reject. Effective-state tests cover separately-created pipeline/declaration wrappers, binding/stream/bias/winding differences and ordered CPU runs without translucent merging. Log: `Build/.agent-state/logs/20261112-210327-358105-35056-RendererSceneContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21. Direct/CPU/GPU pixels match across Forward/GBuffer, custom channels, WPO, masks, mirrors and picking. Receiver native draws fall from 64/3/65 to one for compatible runs; the 65-instance case grows scene storage and rebinds correctly. Shadow comparison verifies 64 to one. Small groups also use CPU merging with GPU culling requested. Log: `Build/.agent-state/logs/20261112-210031-296373-16512-StaticMeshRenderPreparationVulkanTests.log`. |

The full Vulkan receipt preceded a final two-line ownership correction: retain
both the source lookup pipeline and the bucket's canonical pipeline. Scene
contracts were rerun for the final inputs, and the focused native cache
invalidation/numeric-update regression passed again (1/1), log
`Build/.agent-state/logs/20261112-210332-898519-27496-StaticMeshRenderPreparationVulkanTests.log`.
The full pixel/draw-count receipt remains applicable to unchanged merging,
scene-data, fixtures and shader inputs; the focused rerun covers the ownership delta.

Workspace `all` compilation passed, including Engine, Sandbox, RoadWeaver and
remaining editor/tool consumers. Initial broad log:
`Build/.agent-state/logs/20261112-210049-752105-37884-cmake.log` (49.02 seconds).
Final incremental confirmation after the ownership fix:
`Build/.agent-state/logs/20261112-210334-775924-40740-cmake.log` (1.73 seconds).
These are incremental compile/link checks, not a clean build or consumer runtime tests.

Vulkan used NVIDIA GeForce GTX 1060 6GB, API 1.4.312, threaded RHI and
`VK_LAYER_KHRONOS_validation`; no validation errors were reported. CPU fixtures
use inline recording and delayed completion. At the user's request, extended
cached-owner replacement/removal and mixed CPU/GPU partition lifecycle coverage,
Metal execution, timing qualification and broader native consumer tests are
skipped for this commit. Stage 3's extended validation checkbox remains open;
these receipts establish the implemented scope without claiming full qualification.


## Goal

Separate scene updates from view/pass preparation. All supported scene mesh draws,
including single-instance direct draws, read persistent scene data through IDs.
Compatible commands share bindings across Primitives. Views prepare visibility,
LOD selection, instance-ID lists and draw overrides without rebuilding or uploading
unchanged scene attributes.

Align the major names, responsibilities and data flow with UE. Do not claim
source compatibility or copy an undocumented UE byte layout. UE public API pages
span versions; the Durin declarations below are selected adaptations, not quoted
UE signatures. Shader packing and backend lowering remain explicit Durin ABIs.

## UE Reference Model

| UE reference | Selected alignment |
| --- | --- |
| [Mesh drawing pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine) | Retained commands, identical bindings for dynamic instancing, scene-wide Primitive data selected by ID; view/pass command setup remains legitimate work. |
| [FPrimitiveSceneShaderData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FPrimitiveSceneShaderData/__ctor?lang=en-US) | Distinguish semantic Primitive parameters from their packed scene-buffer representation. |
| [FInstanceSceneShaderData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FInstanceSceneShaderData) | Packed instance data identifies its Primitive and instance-relative identity; CPU packing and shader decoding agree. |
| [FPrimitiveSceneInfo](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FPrimitiveSceneInfo) | SceneInfo tracks instance allocation and requests GPU-scene updates. |
| [FInstanceCullingContext](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Renderer/FInstanceCullingContext?lang=en-US) | Pass-local setup, culling and submission over GPU-scene instances, separate from scene-data ownership. |
| [UE 5.4 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5.4-release-notes?application_version=5.4) | Persistent GPU-scene Primitive identity is distinct from a packed CPU scene-list index. |

`FGPUScene`, `FGPUSceneResourceParameters`, and the update lifecycle below adopt
UE vocabulary. Exact private UE allocation/update implementation is not assumed
from public documentation. Durin does not need Nanite, bindless materials, Hi-Z,
native multi-draw, or UE's full scene-extension machinery for this design.

## Ownership and Main Interfaces

`FScene` owns one `FGPUScene` and its authoritative membership, CPU mirrors,
allocators and dirty state. The scene-associated render-resource owner retains
its native buffers through the existing resource registry; device recovery
rebuilds them from current mirrors. No process-wide scene-data singleton is added.

`FPrimitiveSceneInfo` stores a GPU-scene Primitive handle, instance range and
allocation generation. Engine proxies publish source facts; Renderer derives
GPU identity, packing and uploads. `FSceneRenderingService` coordinates execution
but does not merge data from different scenes.

| Interface/type | Responsibility and module |
| --- | --- |
| `FPrimitiveUniformShaderParameters` | View-independent semantic Primitive values; RenderCore value contract, built from Engine producer facts by Renderer. Also supplies any explicit non-GPUScene fallback. |
| `FPrimitiveUniformShaderParametersBuilder` | Checked construction of semantic parameters from transform, bounds, flags and custom data; keep validation/defaults shared with packing and fallback. |
| `FPrimitiveSceneShaderData` | Versioned packing of Primitive parameters; shared CPU/shader layout contract in RenderCore. No native allocation ownership. |
| `FInstanceSceneDataBuffers` | Immutable source snapshot of transforms, stable instance IDs, bounds and custom channels; RenderCore vocabulary published by Engine. It is source data, not a GPU allocation. |
| `FInstanceSceneShaderData` | Packed instance header/transform representation; RenderCore CPU/shader ABI. |
| `FGPUSceneResourceParameters` | Counted Primitive, Instance and payload buffer ranges plus limits; Renderer scene/pass binding and RDG access vocabulary. |
| `FGPUScene` | Renderer-private scene membership, ranges, dirty requests, update transaction and external-buffer access. |
| `FVisibleMeshDrawCommand` | Frame-local command reference plus Primitive/Instance selection, sort/state bucket and submission overrides. |
| `FInstanceCullingManager` | Frame/submission owner for shared culling resources and registered views; no second scene-attribute mirror. |
| `FInstanceCullingContext` | Per-pass command setup, CPU ID-list or GPU-culling construction and submission. |

Illustrative Renderer interface shape; use repository containers/RDG conventions
when implementing these declarations:

```cpp
class FGPUScene
{
public:
    auto AddPrimitive(FPrimitiveSceneInfo& Info) -> void;
    auto RemovePrimitive(FPrimitiveSceneInfo& Info) -> void;
    auto RequestUpdate(FPrimitiveSceneInfo& Info, EGPUSceneDirtyState Dirty) -> void;
    auto Update(FRDGBuilder& GraphBuilder) -> FGPUSceneUpdateResult;
    auto GetShaderParameters(FRDGBuilder& GraphBuilder)
        -> FGPUSceneResourceParameters;
};

// FPrimitiveSceneInfo forwards the request to its owning scene.
auto RequestGPUSceneUpdate(EGPUSceneDirtyState Dirty) -> void;

class FInstanceCullingContext
{
public:
    auto SetupDrawCommands(/* visible commands, view IDs, pass policy */) -> void;
    auto BuildRenderingCommands(/* graph, scene parameters */)
        -> FInstanceCullingDrawParams;
    auto SubmitDrawCommands(/* command list, visible commands, draw params */)
        -> void;
};
```

Dirty categories distinguish Added, Transform, PrimitiveCustomData,
InstanceData, Bounds and Removed. Coalesce requests by handle and revision;
remove wins over pending writes. Keep draw-state invalidation separate.
`Update` returns usable resources and dependency/revision information only for
an admitted transaction; it does not clear dirty state merely during graph setup.
Failed/canceled updates remain pending for the next valid submission.

## Identity and Data Structures

| Identity | Meaning |
| --- | --- |
| `FPrimitiveComponentId` | Existing component publication identity; not a buffer offset. |
| `FPrimitiveInstanceId` | Proposed typed wrapper for the existing stable nonzero uint32 instance ID, scoped to its Primitive; snapshot reordering preserves association. |
| GPU-scene Primitive handle | Internal persistent slot plus CPU generation; independent of CPU scene-list compaction. Shader `PrimitiveId` denotes its validated slot. |
| GPU-scene Instance ID | Current instance-buffer address; source stable ID maps to it. It may change when ranges move. |
| Instance-relative ID | Index inside the current Primitive instance allocation; never the public stable ID. |

Use UE-shaped names at the SceneInfo boundary: `PersistentPrimitiveIndex`
(a typed `FPersistentPrimitiveIndex`), `InstanceSceneDataOffset`,
`NumInstanceSceneDataEntries`, and `InstancePayloadDataOffset`. Keep allocation
generation in CPU ownership metadata, distinct from the shader index. Expose
`GetPersistentIndex()` and `RequestGPUSceneUpdate(...)`; callers must not derive
GPU identity from `GetPrimitiveSceneInfos()` vector positions or `BatchId`.
Packed logical fields use `PrimitiveId`, `InstanceRelativeId`, `InstanceFlags`,
`InstancePayloadDataOffset` and `InstanceCustomDataCount`; offsets have explicit
element/float4 units instead of interchangeable bare byte counts. These are the
selected Durin API names; packing is defined separately from semantic fields.

Allocate one default instance for an ordinary mesh, zero for an explicitly empty
instance snapshot. Preserve the current distinction between empty and cleared
snapshots. CPU handle generations reject stale requests; published GPU ID lists
must also be retired or ordered before allocation reuse. A generation check on
the CPU alone cannot protect an already-recorded GPU reference.

Selected logical data layout:

| Data | Fields/semantics |
| --- | --- |
| Primitive parameters | Primitive-to-world transform/origin, aggregate bounds, flags, instance offset/count, CustomPrimitiveData and explicit payload metadata. |
| Instance source | LocalToPrimitive, stable source ID, local bounds, CustomInstanceData. |
| Packed Instance | PrimitiveId, relative ID, flags, payload offset/count, composed transform/origin and instance bounds needed for shader access/culling. |
| Instance payload | CustomInstanceData, separate from Primitive channels; reserve extension through flags/offsets, not speculative fixed fields. |

`FPrimitiveSceneShaderData` and `FInstanceSceneShaderData` expose explicit
float4-aligned packing/stride constants, with shader `GetPrimitiveData` and
`GetInstanceSceneData` accessors. Do not upload compiler-dependent C++ object
layouts. Specify matrix order, padding, integer bit interpretation and invalid
IDs and verify them on Metal and Vulkan. Begin with uncompressed transforms;
compression is a later measured optimization.

Retain split high/low world origins and existing normal/inverse-transform
semantics. Store world-independent-of-view bounds/origins and translate relative
to the current view in shaders. Component transforms can dirty all composed
instances; changing one instance dirties only its record/payload and affected
aggregate bounds. This cost is explicit, not described as constant-time updates.
Retain the existing 32-float Primitive and Instance channel limits, finite-data
admission, conservative WPO bounds obligation and aggregate component LOD policy.

## Scene Update and Submission Flow

1. Ordered Add/Remove/Transform/CustomData/Instance commands change source state
   and request dirty updates. Full source snapshots may be diffed by stable ID;
   a whole-component CPU publication does not require a whole-scene GPU upload.
2. Before consuming views, commit the current scene revision: allocate/patch
   ranges, coalesce dirty byte ranges and admit uploads. Build from scene source
   facts, including hidden/offscreen members, rather than visibility discovery.
3. Import persistent buffers into each RDG that consumes them. One unchanged
   revision needs no repeated attribute upload. A later scene update between
   views produces a new ordered revision; do not cache an upload decision by
   frame number alone. RDG handles never escape their builder.
4. Prepare per-view visibility, requested/selected LOD and visible commands.
   Each visible command refers to scene-instance IDs/runs. Compatible commands
   can merge after comparison of their complete effective draw state.
5. CPU selection or GPU culling generates instance-ID lists and draw overrides.
   Compute reads persistent bounds; graphics reads the same Instance/Primitive
   records. Candidate IDs, visible IDs, counts and indirect arguments may remain
   transient. Complete transforms/custom channels are not copied per view.

`GetShaderParameters` must declare exact RDG read ranges. Dirty writes precede
compute/graphics readers, including cross-graph submissions and queue transfers.
Separate builders do not infer dependencies on one another: carry the existing
RHI submission/access dependencies explicitly through the scene resource owner.
Preserve CPU preparation/recording joins before mutation. On buffer growth,
initialize the new capacity from current CPU mirrors, publish a new resource generation,
and retire old buffers/bindings only after their recorded/GPU uses complete.
No historical per-view scene versions or unconditional GPU idle waits are added.

Stage 1 deliberately uses a full mirror upload on geometric growth instead of a
GPU copy followed by patches. This initializes unused capacity deterministically
and avoids a second copy/retirement path during initial adoption. Growth bytes
are reported separately by buffer rebuild count; unchanged-capacity updates stay
incremental. Revisit GPU-to-GPU growth copies only with measured growth cost.
The internal RequestUpdate takes a generation-bearing handle; SceneInfo retains
the UE-shaped RequestGPUSceneUpdate forwarding interface. Non-compilation graph
failure discards native storage before retry so a potentially recorded prefix
cannot authorize unsafe reuse or carry an unknown final access state forward.

The scene-resource binding is pass-scoped and late-resolved. Cached mesh commands
must not retain an obsolete raw scene buffer after growth/recovery. Use the
existing resource-table indirection where sufficient; otherwise add a bounded
generation-aware pass rebind, without rebuilding every geometry command.

## Mesh Commands, Shader Access and Fallback

Keep source-owned cache lookup/removal identity separate from instancing identity.
`FCachedMeshDrawCommandInfo` retains Primitive association. Add
`FMeshDrawCommand::MatchesForDynamicInstancing` and a consistent hash/state bucket
over effective PSO, shaders, topology, vertex/index buffers and ranges, resource
bindings and draw state. Hash equality alone is insufficient. Pipeline/binding
wrapper pointer equality alone must not reject equivalent effective state.

Exclude per-object transforms, numeric custom data and source IDs from that
comparison. Preserve differences in determinant/cull parity, depth bias, stencil,
material bindings, per-component streams and deformation resources. Translucent
ordering remains authoritative; shared scene access does not authorize arbitrary
translucent reordering or Spline merging.

Direct and indirect submissions use the same scene-data geometry shaders.
The vertex factory resolves an Instance ID through an instance-frequency ID
stream or the supported backend's equivalent indexed ID buffer; one direct draw
uses one ID. Preserve and qualify the current base-instance interpretation.
Pass a flat Primitive/Instance identity to fragments that need it. Existing
referenced-channel varyings may remain an optimization of these semantics.
WPO, masking and HitProxy must use the selected instance, not a group representative.
Carry picking identity through selected Primitive data or an indexed selection
table so per-draw picking uniforms do not silently defeat sharing.

`UseGPUScene` is a capability/vertex-factory data-access decision.
`bEnableGPUCulling` controls the selection algorithm independently. A small group
or unavailable compute pipeline uses direct/CPU-instanced submission with the
same GPUScene records. The 64-candidate floor is not a GPUScene admission floor.
Providers without scene-data support keep an explicit uniform path and cannot
join GPUScene groups. Dynamic providers use a scoped dynamic allocation/upload
context with the same shader addressing; keep those records alive through use,
without turning view-generated geometry into persistent scene membership.

Use an explicit instance-data access capability in mesh/VF preparation instead
of the GBuffer-specific `bGPUCulling` pipeline distinction. Provider-specific
deformation bindings remain independent. Missing scene data must fail admission
or the graph transaction, never draw partial/stale geometry. No unordered late
redraw is introduced.

## Existing Contract Changes

These are implementation prerequisites, not edits to current runtime claims in
this design-only change. Update each owning document with its migrated code.

| Owning contract / current rule | Required adjustment |
| --- | --- |
| [Scene representation](../Runtime/Rendering/Scene/SceneRepresentation.md): SceneInfo owns transform/bounds and scene commands publish updates | Add scene-owned GPU membership, persistent handles/ranges and RequestGPUSceneUpdate hooks for all source mutations and teardown. Preserve generic Primitive membership and queue ordering. |
| [StaticMesh rendering](../Runtime/Rendering/Scene/StaticMeshRendering.md): per-Primitive uniforms and view-local copied transforms; explicitly excludes GPUScene/instancing | Replace supported-path uniforms with scene IDs and packed scene resources. Keep visibility/LOD/prepared values view-local while sharing only scene-owned attribute data. |
| [Retained batch publication](../Runtime/Rendering/Scene/StaticMeshBatchPublication.md): source batch facts plus downstream Primitive IDs/instance uniforms | Publish instance source data independently from per-section geometry/material descriptions. Keep IDs out of geometry capability tables; attach instance ranges at visible-command construction. Dynamic/custom producers retain explicit admission. |
| [Material custom data](../Runtime/Rendering/Materials/MaterialCustomData.md): one batch per stable instance; 512-byte combined uniform; GPU grouping uses InstanceTransforms | Publish stable instance snapshots through FInstanceSceneDataBuffers, separate Primitive/Instance storage, and remove repeated Primitive channels. Preserve stable-ID APIs, limits, defaults, WPO and pixel semantics. |
| [Material stage selection](../Runtime/Rendering/Materials/MaterialShaderStages.md): optional GPUCullingVertexMain restricted to Local GBuffer | Make scene-data access independent of GPU culling and available to supported Local/Spline passes. Replace the culling-only geometry usage with the unified Local entry contract; migrate persisted masks and reject incompatible cooked products explicitly. |
| [Material system](../Runtime/Rendering/Materials/MaterialSystem.md) and [shader parameters](../Runtime/Rendering/Shaders/ShaderParameters.md): Primitive uniform binding/reflection | Add scene resource parameters and selected-instance access to fixed/generated shaders, strict reflection admission and VF bindings. Update translation/pass/binding compatibility identities and Cook invalidation; no unversioned reinterpretation. |
| [Frame preparation](../Runtime/Rendering/Pipeline/RendererFramePreparation.md): resolved PrimitiveUniforms and source-owned cache bindings | Add an ordered scene-update boundary, scene read parameters and visible instance selection. Retain CPU joins, current-value semantics, frame-local execution and transactional publication. |
| [Resource lifecycle](../Runtime/Rendering/Pipeline/RenderResourceLifecycle.md): source-owned Primitive uniform resources | Scene owns native scene buffers; source owns registration/cache associations. Define replacement generations, scene release, dynamic allocation retirement and recovery from mirrors. |
| [GBuffer](../Runtime/Rendering/Pipeline/GBuffer.md): per-view candidate/transform uploads and graphics permutation | Replace temporary attribute arrays with persistent scene reads; use generic FInstanceCullingContext. Keep default-disabled culling, direct fallback and correctness qualification until new evidence authorizes policy changes. |
| [Directional shadows](../Runtime/Rendering/Features/DirectionalShadows.md): direct per-Primitive uploads/reuse and cascade-local preparation | Read shared scene data in shadow vertices; preserve cascade view/bias and masked-WPO behavior. Add pass-local CPU merging without promising shadow-map content caching. Coordinate the existing [shadow optimization plan](ShadowPreparationOptimization.md), whose exclusion of GPUScene limits that plan, not this one. |
| [RHI resource views/transfers](../Runtime/Rendering/RHI/RHIResourceViewsAndTransfers.md) and [GPU completion](../Runtime/Rendering/RHI/VulkanMemoryAndGPUCompletion.md) | Preserve ordinary FRHIBuffer storage, WriteBuffer patches, counted ranges and completion retirement. Specify scene-level use of these contracts; extend RHI only if a demonstrated ID-fetch/synchronization capability is missing. Do not add a GPUScene-specific RHI resource class. |
| Telemetry in frame/GBuffer/shadow contracts | Separate scene dirty/upload bytes, resource rebuilds, visible IDs, command reuse, merge counts and culling costs. Replace assumptions that PrimitiveUniformUploads represents all instance-data cost; preserve defined legacy counters during migration or rename their consumers together. |

Material/VF/binding API changes require symbol searches in source and test roots
of Engine, Sandbox and RoadWeaver declared by `Durin.dworkspace`; migrate every
consumer, including custom providers. Inspect reflection/schema dependencies
before choosing version bumps. Existing archived acceptance receipts do not
qualify the changed ABI and must not be rewritten as new evidence.

## Implementation Stages

### Stage 0: Specify interfaces and contract migration

- [x] Select UE-aligned owners, identities, public vocabulary and submission flow.
- [x] Identify current contracts to replace or extend and preserved invariants.
- [x] Separate design from implemented runtime claims and define acceptance gates.

Outcome: this reviewable design. Stage 1 depends on this interface direction.

### Stage 1: Add scene storage and incremental updates

- [x] Implement semantic/packed values, stable handles, instance ranges and mirrors.
- [x] Wire scene mutation hooks and full-snapshot diffing into dirty requests.
- [x] Implement ordered patch uploads, failed-update retry, growth and recovery.
- [x] Validate ordinary/empty/multiple instances, offscreen mutations, stable-ID
  reorder/removal, range relocation, generation rejection and GPU-safe reuse.

Outcome: unchanged scene revisions upload zero attribute bytes; one instance
change patches only affected records/payload/bounds. Depends on Stage 0.

### Stage 2: Unify scene-data shader and direct submission

Stage 2 implementation decisions:

- Commit dirty persistent scene data through a dedicated upload graph before
  mesh binding preparation. Geometry graphs import the resulting counted buffers
  and declare reads explicitly. A successful geometry graph registers its terminal
  completion with GPUScene so logical slots wait for actual drawing, not merely
  the preceding upload graph. Scene-data commit is independent of view output.
- `FGPUSceneDynamicContext` prepares submission-owned packed records for unpublished
  or provider-specific transforms. Built-in factories advertise `SupportsGPUScene`;
  other factories retain their explicit uniform vertex contract. Direct draw
  overrides select an instance-ID list through base instance while cached geometry
  arguments remain source values. This establishes ID transport without Stage 3 merging.
- Local/Spline geometry stages serve both direct and indirect submission. Legacy
  material GeometryUsage bit 4 no longer contributes a culling-only stage; new
  materials default to mask 3. Authored load/edit and shader identity normalize
  legacy masks. The reserved stage ordinal remains readable for rejection and
  diagnostics; old binaries are rejected by Pass contract 13, generator revision
  16 and compile-adapter revision 13. The Cook container schema is unchanged because
  the persisted container shape is unchanged.
- Until Stage 4 removes the old culling plan, it still constructs/uploads its
  temporary transform array for existing budget/telemetry semantics; graphics
  selects scene IDs instead of reading that array. Per-group compatibility includes
  the scene-data owner so persistent and scoped dynamic IDs cannot alias.

- [x] Add scene accessors, VF capability and one-instance ID selection.
- [x] Migrate fixed/generated Local and Spline geometry, Forward/GBuffer/Shadow/
  HitProxy consumers, stage selection, reflection, Cook and cache compatibility.
- [x] Provide scoped dynamic scene records and explicit unsupported-provider fallback.
- [x] Validate CPU/shader packing, base instance, WPO/custom channels, masking,
  normals, negative scale, picking and 1e12-origin precision against direct output.

Outcome: supported direct rendering uses persistent scene data with GPU culling
disabled; all affected workspace consumers compile. Depends on Stage 1.

### Stage 3: Share commands and add CPU dynamic instancing

Stage 3 implementation decisions:

UE reference interfaces: [EPrimitiveIdMode](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/EPrimitiveIdMode),
[FMeshDrawCommand](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FMeshDrawCommand),
and [FVisibleMeshDrawCommand](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Renderer/FVisibleMeshDrawCommand).
Durin implements the fields required by its current transport; it does not add
unused UE indirect/root-constant override fields merely to copy the complete API.

- Providers select `EPrimitiveIdMode` on their batch and identify the stable
  source instance explicitly. Collection captures `PersistentPrimitiveIndex`;
  submission resolves that typed source reference after the scene update. Remove
  the ComponentId lookup map and all transform/payload repacking comparisons.
  An invalid persistent reference fails admission instead of silently becoming
  dynamic. Standalone consumers without a scene explicitly use scoped storage.
- `MatchesForDynamicInstancing` compares effective shaders, pipeline description,
  layout/declaration, streams/ranges and resource binding values. Hash buckets
  narrow candidates; wrapper addresses and source cache IDs do not decide equality.
  Cached commands remain source-owned; visible commands carry state bucket IDs and
  `PrimitiveIdBufferOffset` and `NumInstances` for submission-local ID ranges/counts.
- Merge consecutive compatible opaque/masked command runs without reordering.
  Keep Spline, sorted translucency, custom unsupported factories and instance-rate
  streams outside this path. Use a separate CPU-selected ID list with counted
  submission ownership and explicit RDG reads; no scene buffer is captured in a
  cached geometry command.
- Preserve original direct commands alongside CPU runs until GPU visibility
  submission is chosen. If a CPU run contains any GPU-admitted member, use original
  commands for its direct remainder; if dispatch fails, use the complete CPU run.
  Stage 4 can unify this selection with its common culling submission context.
  Logical draw conservation remains separate from actual native submission counts.


- [x] Add MatchesForDynamicInstancing/hash buckets, visible instance ranges and
  draw override parameters; keep source cache identity separate.
- [x] Carry explicit persistent/dynamic scene-data provenance from collection into
  visible commands. Remove Stage 2's per-view transform repacking/comparison used
  to distinguish provider overrides; normal scene draws should select validated
  scene IDs without rebuilding attribute data to infer ownership.
- [x] Route compatible opaque/masked and shadow submissions through CPU ID lists.
- [ ] Validate equivalent separately-created state merges, incompatible bindings/
  winding/bias do not merge, ordering is preserved, and growth rebinds correctly.

Outcome: different transforms/custom channels share a draw when effective state
matches, including small groups. Depends on Stage 2.

### Stage 4: Generalize GPU instance culling

- [ ] Add manager/context setup and BuildRenderingCommands/SubmitDrawCommands.
- [ ] Migrate GBuffer candidate processing to IDs and persistent bounds reads;
  delete per-view Transforms generation/upload and culling-only graphics shaders.
- [ ] Validate zero/partial/full visibility, group budgets, multiple views/graphs,
  CPU fallback, async queue dependencies and allocation/cancellation failure.
- [ ] Rebase transient storage budgets and telemetry to the new record sizes;
  account for persistent scene capacity separately from per-view culling cost.

Outcome: CPU selection and GPU culling consume the same scene attributes and
graphics path, without rendering readback. Depends on Stage 3.

### Stage 5: Qualify and retire replaced contracts

- [ ] Remove obsolete supported-path uniforms, arrays, stage bits and adapters;
  retain only documented capability fallbacks with exercised consumers.
- [ ] Update the owning runtime contracts listed above with implemented behavior.
- [ ] Complete targeted correctness/native qualification and workspace integration
  gates, recording tested inputs and reusable receipts in this plan.
- [ ] Compare static/dynamic, small/large, multi-view and shadow-heavy workloads;
  record CPU preparation, upload bytes, draw counts, GPU time and retained memory.

Outcome: no duplicate authoritative path for supported scene meshes, no unmeasured
default culling-policy change, and documented acceptance evidence. Depends on Stage 4.

## Validation and Completion Gates

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection](../Agents/Testing.md). Stage 0 required changed-document and
all-plan validation only; implementation stages require runtime evidence below.

Implementation stages use owning-module and affected-consumer builds/tests.
Final integration requires an `all` build because shared VF, mesh binding and
material stage/Cook ABIs span workspace providers and cannot be established by
Renderer-only coverage. Run this broad gate after the final shared-ABI migration;
reuse it while relevant inputs/environment remain unchanged.

Native qualification covers supported Metal/Vulkan paths and inline/threaded
recording, with validation enabled for correctness. Include direct/CPU-merged/
GPU-culled output comparisons, multiple scenes, interleaved scene changes and
views, buffer growth, teardown/recovery and cross-queue use. Legacy uniform
fallback and old-cooked-product rejection/migration require explicit cases.

Structural acceptance requires zero unchanged Primitive/Instance attribute
uploads on camera-only movement and additional views of the same committed
revision. Visibility/ID/argument uploads are reported separately. Dirty uploads
scale with changed records plus allocator growth, not visible draws times views.
Per-frame command creation and actual native draw counts prove reuse/merging.
Timing comparisons must report persistent memory and scene-update cost as well
as pass cost; fewer draws alone do not establish a speedup.
