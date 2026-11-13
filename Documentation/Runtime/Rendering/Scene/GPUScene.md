# Persistent GPU Scene Storage

Summary: Define scene-owned Primitive and Instance records, incremental uploads, selected-instance shader access and allocation retirement.
Modules: RenderCore, Engine, Renderer
Last reviewed: 2026-11-13

## Scope and Ownership

`FScene` owns one `FGPUScene`, registered as a render resource. Membership,
CPU mirrors and dirty requests are independent of view visibility. Built-in Local and Spline factories consume these records in direct and indirect
Forward, GBuffer, shadow and HitProxy drawing. GPU culling is independent of
scene-data access. Unsupported factories retain the explicit `FMeshFallbackPrimitiveUniform`
contract through sparse `FallbackPrimitiveUniforms`; supported scene meshes
allocate no per-view Primitive uniform array. CPU command merging uses ordered compatible runs. Pass-local GPU culling reads the same packed attributes directly.

`FPrimitiveSceneInfo` stores `GPUSceneHandle`, `GetPersistentIndex()`,
`InstanceSceneDataOffset`, `NumInstanceSceneDataEntries` and
`InstancePayloadDataOffset`. Component IDs, persistent GPU Primitive indices
and CPU scene-vector indices are separate identities. CPU handles include a
generation to reject stale updates. Instance GPU addresses can change when
their range moves; stable nonzero `FPrimitiveInstanceId` values remain scoped
to a Primitive.

`FInstanceSceneDataBuffers` is immutable once published through a const shared
owner. StaticMesh proxies derive it from the validated full instance snapshot.
Null means one identity instance; non-null empty means zero instances. Shared
numeric/source values live in RenderCore without Engine-object dependencies.
Component APIs retain the existing uint32 IDs and complete snapshots.

## Values and Packing

`FPrimitiveUniformShaderParametersBuilder` validates affine finite transforms,
invertibility, finite float transport and custom channels. Semantic values
are packed explicitly into `FPrimitiveSceneShaderData`.
`FInstanceSceneShaderData::Build` shares transform packing. Invalid transforms
produce inactive zero records. Invalid bounds retain an invalid marker and are
retained conservatively by culling consumers.

The initial uncompressed ABI uses consecutive 16-byte float4 words:

| Word | Primitive and Instance common prefix |
| --- | --- |
| 0-3 | Transposed translation-free LocalToWorld |
| 4-7 | Transposed origin-relative WorldToLocal |
| 8-11 | Normal inverse-transpose in the same storage convention |
| 12-13 | Split high/low world origin, zero W |
| 14 | Bounds minimum relative to origin, validity in W |
| 15 | Bounds maximum relative to origin, determinant sign in W |

Primitive word 16 bit-encodes instance offset, count, active flags and reserved
zero. Words 17-24 hold 32 Primitive custom floats: 400 bytes per record.
Instance word 16 bit-encodes PrimitiveId, InstanceRelativeId, active flags and
payload offset. Word 17 holds custom-float count, stable source ID and two
reserved zeros: 288 bytes per record. Separate payloads hold 32 Instance custom
floats (128 bytes). Payload offsets use float4 elements; instance offsets use
records. Packing types own explicit word/stride constants, not C++ semantic layout.

Renderer sorts source instances by stable ID. Reordering snapshots produces
no uploads; changed channels patch their own payload. Component transforms can
dirty all composed instances. Split origins, normal/inverse semantics, aggregate
component LOD and conservative authored WPO bounds remain unchanged.

## Update Transactions

Scene Add/Remove/Transform/CustomData/Instance and Spline bounds commands feed
`RequestGPUSceneUpdate`. Requests coalesce before mirror refresh. Removal wins
over pending source work. Each transaction has a scene revision and resource
generation. `Update` imports or creates three buffers and queues dirty byte
ranges; adjacent dirty records coalesce. Allocation/growth uploads initialized
geometric capacity from CPU mirrors, including zero padding. This full upload
is an explicit exception to dirty-record scaling.

Keep `FGPUSceneUpdateResult` and its scene alive through execution and call
`CompleteUpdate` after that exact graph. Its extraction destinations belong to
the result; parameter handles belong to the builder. Source mutation follows
completion or abandonment of the prepared transaction. Only successful RDG
execution with matching resource generation acknowledges dirty data. Failure,
non-execution or dropping an unexecuted transaction with its graph retains
pending uploads. Submission failure follows the terminal RHI contract.

Non-compilation execution failure discards committed native buffers and marks
all mirrors dirty before retry. A potentially recorded prefix must not retain
unknown access state or grant allocation reuse. Rejected compilation preserves
the previous committed buffers because it cannot have recorded any writes.

Production preparation commits dirty data in a dedicated upload graph before
resolving mesh bindings. The drawing graph imports the committed buffers, declares
graphics reads and registers its terminal completion through `RegisterGraphUse`. Additional unchanged views upload zero scene
attribute bytes. Intervening scene commands create another revision; suppression
does not use frame number alone. Buffers finish graphics-readable; future
compute consumers must declare read ranges and queue uses in their graphs.

## Shader Access and Submission

`SceneData.slang` decodes the packed ABI through `GetPrimitiveData`,
`GetInstanceSceneData` and `GetMaterialPrimitiveData`. The material adapter combines
Primitive custom channels with the selected Instance transform, bounds and payload.
Set 0 bindings 27, 28, 29 and 31 contain Primitive words, Instance words, payload
words and uint Instance IDs. Attribute buffer stride is 16 bytes. Set 1 material
bindings remain independent.

`FGPUSceneShaderParameters` borrows native ranges. `FGPUSceneBufferSet` retains
counted owners for a submission; `FGPUSceneReadParameters` imports and declares
those resources per consuming graph. Builder handles never enter cached mesh
commands. Pass bindings resolve current buffers after growth or recovery.

Providers select `EPrimitiveIdMode` on each batch: built-in StaticMesh/Spline
publication uses `PrimID_FromPrimitiveSceneInfo`; extension batches default to
`PrimID_DynamicPrimitiveShaderData`. A persistent reference consists of the
collected `PersistentPrimitiveIndex` and explicit stable `InstanceSceneDataId`,
independent of the source cache's `BatchId`. Providers opting into persistent
access must publish the same transform, bounds and custom channels in their scene
snapshot. Submission resolves that reference after scene update; missing records
fail admission rather than inferring dynamic data. There is no per-view attribute
repacking/comparison or ComponentId lookup map. Standalone HitProxy explicitly
supplies no scene and uses scoped records.

`FGPUSceneDynamicContext` allocates dynamic Primitive/Instance ranges in the same
GPUScene as persistent records. A submission commits updates with an explicit
RDG allocator, then snapshots the final buffers after any growth. Standalone
consumers create a transient GPUScene through the same allocation/upload path.
One scene submission retains a shared context across receiver and shadow views.
`FGPUScenePrimitiveCollector::Commit` assigns dynamic ranges to explicit immutable
records. Sharing a record reuses its allocation; distinct records remain distinct
even when their transported bytes match. A changed record is a new identity.
Logical collection supplies these records; no packed-byte identity cache remains.
All receiver/cascade collectors commit before `Upload`, so growth completes once
and every view binds the same final attribute buffers. `UpdateCPUData` refreshes
membership/ID lookup without executing a graph. Binding consumes uploaded data.
The context is the sole allocation-domain authority. Independent preview/picking
requests create their own contexts; the one-shot `Prepare` helper commits and
uploads for these callers. The submission registers terminal drawing-graph use once.
Durin still executes one dedicated attribute upload graph before native binding
construction; UE can author uploads in its drawing graph. This integration boundary
does not change range retirement or permit per-view attribute graph execution.
Factories opt in through `SupportsGPUScene`; unsupported factories retain uniform access.
It creates a direct ID list, repeating the selected ID for existing multi-instance
geometry. `InstanceIdOffset` overrides emitted `FirstInstance`; source draw
arguments stay unchanged. Both direct and indirect vertices index the ID list
with `SV_InstanceID + SV_StartInstanceLocation`. Fragments needing object-space
data receive a flat selected Instance ID; referenced custom channels can remain
flat slot varyings. Standalone HitProxy uses scoped records and retains its
per-draw picking uniform.

## CPU Dynamic Instancing

`FMeshDrawCommand::MatchesForDynamicInstancing` compares effective pipeline,
shader/layout/declaration, geometry ranges, streams and resource bindings;
`GetDynamicInstancingHash` returns a uint32 candidate hash. Equality must still
be checked. Pipeline and binding wrapper addresses do not determine compatibility.
Source cache slots remain per Primitive/batch; eligible identical cached commands
share a state bucket and command allocation. Each source retains its own
`FCachedMeshDrawCommandInfo` and invalidation identity on `FPrimitiveSceneInfo`.
Cache lookup uses structural pipeline identity: pass, raster/depth/blend/target
description, factory/layout, native shader identities and declaration elements.
The immutable identity is cached once per pipeline snapshot and shared by cache
keys/entries; dynamic instancing uses that same equality definition. Copies create
new editable snapshots; published pipelines remain immutable. Native preparation
warms identity before publication, and concurrent readers share its cached value.
Pipeline wrapper addresses do not enter the key. Only the shared state bucket
retains its canonical pipeline until its last source entry is removed; equivalent
source wrappers need no additional retention. Replacing or removing one source retires only that source's visible
handles; other sources can continue using the shared command and pipeline.
Weak lookup buckets are pruned when their last source entry is removed.

`BuildMeshDrawCommandRuns` forms consecutive effective-state runs shared by CPU
instancing and GPU admission. Production builds these after final component draw
admission and reuses the same membership for GPU selection; no pass-local hash
buckets or separate CPU/GPU state numbering are needed. One submission table owns
run members, draw-to-run lookup, argument selection and dispatch outcome. CPU and
GPU consumers share it; there is no copied CPU group membership or post-dispatch
enable mutation. GPU eligibility does not split CPU-compatible runs: admission
accepts a whole run or leaves it to CPU fallback. CPU submission merges compatible opaque/masked Local runs in
Forward, retained Forward, GBuffer and shadows. It preserves prepared order;
Spline, sorted translucency, unsupported factories, original multi-instance draws
and instance-frequency vertex streams remain separate. Visible records carry
`StateBucketId`, `PrimitiveIdBufferOffset` (uint ID elements) and `NumInstances`.
Only emitted draw arguments receive the count/offset overrides. One counted
submission-owned ID buffer serves CPU runs across its passes and declares RDG reads.
`bEnableDynamicInstancing` defaults true and independently disables this path for
reference output comparisons.

GBuffer retains original commands until GPU admission is known. Each shared run
uses indirect submission after successful GPU selection, or its CPU-instanced/
direct fallback otherwise. Failed dispatch restores complete CPU runs without
mutating another set of group-enable flags.
Logical successful-draw counts stay conserved; `NativeDrawSubmissions` and
`ShadowNativeDrawSubmissions` count actual emitted commands. Fewer submissions
alone do not establish a frame-time improvement.

## Instance Culling Contexts

`FInstanceCullingManager` owns shared compute-resource readiness and registered
view snapshots. A context is associated with its manager when created;
`BuildRenderingCommands` does not accept a second manager. The manager does not
retain an unused context list. Each `FInstanceCullingContext` owns one pass's source selection,
scene-buffer references, candidate IDs and argument mapping. Resetting the
manager's view list does not invalidate contexts retained by pending graph
callbacks; the rendering service outlives their execution.

`CollectMeshDrawCommands` projects prepared providers into a
`FMeshCommandOneFrameArray`. Entries retain frame-local scene selection and
culling eligibility; `SetupDrawCommands` consumes this common visible-command
boundary, validates counted scene IDs and admits whole consecutive compatible
runs. Effective command, scene buffers, pass bindings and bias determine equality;
source description, binding and buffer-set wrapper addresses do not. Incompatible
or unsupported intervening commands split runs, preserving prepared order. `BuildRenderingCommands` creates graph-owned candidate, visibility and
argument buffers and declares the shared scene-data buffer's compute read. Candidates
are eight-byte ID/argument records; they contain no transforms or bounds.
Compute decodes bounds and origin high/low from the 18-float4 instance ABI and
translates them using the view's high/low origin. One dispatch consumes the shared
persistent/dynamic ID domain. A context rejects foreign scene-buffer domains.

The shared submission table resolves CPU/GPU selection. `SubmitDrawCommands`
uses the ordinary retained geometry command for direct, CPU-merged and indirect
drawing. Submission consumes the visible array, a start/count range and
`FMeshDrawCommandOverrideArgs`; it no longer reads prepared StaticMesh objects.
Overrides provide an instance buffer, indirect buffer and byte offsets.
`FMeshDrawCommandSceneArgs` carries the lowered per-draw arguments, with
`PrimitiveIdOffset` measured in uint32 ID elements for Durin's shader ABI.
Instance-buffer byte offsets select the bound buffer range. Direct CPU runs carry
their ID buffer on visible entries. Binding emission applies these overrides
without copying or mutating retained commands or pass-binding arrays. Draw params
publish RDG handles as `InstanceIdOffsetBuffer` and `DrawIndirectArgsBuffer`.
Counted frame/graph owners retain resources; override pointers only borrow them.
The compute dispatch must record successfully before indirect submission is
published. Pre-dispatch failures select complete CPU runs. Abandoned graphs,
allocation failures and graph rejection publish no GPU completion or indirect
selection; a retry uses a fresh context. Graph/submission failure after recording
is terminal and never redraws late.

Transient budget/telemetry charges candidate IDs, visible IDs and arguments
separately from `GPUCullingSceneCapacityBytes` and
`GPUCullingStandaloneSceneCapacityBytes`. Candidates and visible IDs cost 12 bytes
per instance; indexed arguments cost 20 bytes per group. Capacity observations
count each referenced scene's three attribute buffers once per view, including
its dynamic ranges; they are neither upload bytes nor
new allocations. Multiple views do not cause unchanged scene attributes to upload.
The default-disabled GBuffer policy and 64-candidate floor remain unchanged;
qualification limits are recorded in the [GPUScene plan](../../../Plans/UEAlignedGPUScene.md).

## Update and memory observations

`FGPUScene::GetStatistics()` is render-thread only. `UploadBytes`, `UploadRanges`
and `BufferRebuilds` accumulate successfully admitted update transactions;
failed/canceled/abandoned updates do not increase those committed counters.
`CapacityBytes` counts the current three native attribute buffers and
`MirrorCapacityBytes` counts capacity of the corresponding CPU attribute vectors.
They exclude historical generations awaiting retirement, allocator overhead,
lookup maps, geometry, pass attachments and transient ID/argument storage. They
are capacity observations, not total GPU residency or per-view traffic. Snapshot
differences include updates performed during preparation retries and allow
camera-only/additional-view zero-upload checks independently of uniform counters.

## Retirement and Recovery

`FRDGBuilder::GetGPUCompletion()` exposes a successful terminal graph join,
covering every using queue; it is absent before success. Free Primitive slots
and Instance ranges wait for the last using graph to complete. Submitted,
canceled and failed signals do not authorize reuse. Existing counted RHI
bindings and backend retirement retain replaced native allocations.

`ReleaseRHI` drops committed buffers, increments resource generation and marks
all mirrors dirty for reconstruction. Old-device retirement prerequisites are
cleared only at this ordered resource-release boundary. Source mirrors survive.
Dynamic allocation leases retain GPUScene independently of FScene. Frame and
graph storage retain these leases, and terminal drawing-graph use determines
range retirement. Release during an unfinished upload is deferred until that
transaction commits or is abandoned. Freed slots retain unreferenced contents
without clearing uploads; reuse waits for the last GPU use. Game-thread lease
destruction queues range release while render-command admission is running.
Scene teardown unregisters membership and releases its reference; leases keep
shared allocation storage alive and recorded commands retain native references.

## Related Contracts

- [Scene representation](SceneRepresentation.md)
- [Material custom data](../Materials/MaterialCustomData.md)
- [Frame preparation](../Pipeline/RendererFramePreparation.md)
- [Resource lifecycle](../Pipeline/RenderResourceLifecycle.md)
- [Buffer views and ordered writes](../RHI/RHIResourceViewsAndTransfers.md)
