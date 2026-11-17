# Persistent GPU Scene Storage

Summary: Define scene-owned Primitive and Instance records, incremental uploads, selected-instance shader access and allocation retirement.
Modules: RenderCore, Engine, Renderer
Last reviewed: 2026-11-18

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

`FPersistentPrimitiveIndex` follows UE's scene-scoped index contract: it survives
packed scene-list movement during a Primitive's lifetime, but removal permits
slot reuse. It carries no public generation and is not a permanent object ID.
`FGPUScene::GetPrimitiveSceneInfo` resolves the current occupant on the render
thread. Existing internal generation-bearing handles protect retained selections
and update requests; they do not change the public index representation.

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
over pending source work. SceneInfo validates its generation-checked source
reference before calling `AddPrimitiveToUpdate(FPersistentPrimitiveIndex, DirtyState)`.
The deduplicated `PrimitivesToUpdate` queue stamps each entry's generation;
removal/reuse cannot apply a queued request to another primitive. CPU refresh
visits queued entries only, including updates admitted between collector phases.
Packed Primitive/Instance/payload dirty membership uses sparse index lists.
Upload sorts only those indices to coalesce adjacent patches, and successful
commit clears only their membership. IsDirty is a constant-time queue check.
Growth and recovery remain full-initialization exceptions; failed or abandoned
transactions retain the sparse lists for retry. Each transaction has a scene revision and resource
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
Named owner parameters contain Primitive words, Instance words, payload words
and uint Instance IDs. Attribute buffer stride is 16 bytes. Compiler reflection
and accepted product maps own descriptor locations; scene/material lifetime
selection uses metadata scope. Instance-ID replacement retains its accepted location.

`FGPUSceneShaderParameters` borrows native ranges. `FGPUScene::GetShaderParameters(Graph)`
returns that graph's active update parameters, or registers the last committed
buffers, counts and generation outside the update. Another graph never receives
pending handles or uncommitted CPU counts. Transactions match the builder's
existing handle-owner ID, never its reusable memory address. Retained `FGPUSceneBufferSet` snapshots
use the same `FGPUSceneResourceParameters::Register` path. Registration is scoped
to the receiving graph; growth, abandonment and scene release cannot replace a
retained snapshot's counted buffers. Builder handles never enter cached commands.
The consuming owner still reports terminal completion through `RegisterGraphUse`.

Each resolved view retains one scene-buffer/direct-ID domain, validated during
`SetupMeshDrawCommands`; equivalent wrappers may share it, while conflicting
attribute or direct-ID buffers reject preparation. Its
`GetInstanceCullingDrawParams(Graph, DrawParams)` supplies the common `Scene`
parameters plus optional direct/dynamic instance-ID buffers, preserving any
culling-produced ID/indirect buffers and byte offsets in `DrawParams`.
`FInstanceCullingDrawParams` is the shared dependency block for direct, instanced
and indirect mesh passes. Scene attributes and all ID streams share the same
external-registration contract. `FVisibleMeshDrawCommand::SubmitDraw` selects the
command's direct/CPU-compacted IDs and logical offset for fallback draws; only an
admitted indirect override selects generated IDs and byte offsets. Mixed batches
retain both dependencies; culling does not erase fallback streams.

Imports validate external entrance/final access contracts on every call. Empty
optional resources declare no use; aliased fields remain truthful declarations,
and RDG combines compatible read access within the consuming pass. Shadow
features retain per-cascade draw-parameter snapshots because independently
prepared domains may own distinct buffers. There is no generic view/cascade
capacity limit, and translucent consumers declare their scene resources explicitly.

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
The unified resource phase publishes that context into each resolved view only
after a successful shared upload. Binding preparation requires the resolved
context, matching primitive count and uploaded buffer-set ownership; it never
collects or uploads missing GPUScene data. Unsupported-factory empty entries
remain valid. Shadow depth passes supply no placeholder lighting uniform.
The context concatenates their direct instance-ID lists into one upload and
assigns each primitive an offset in that shared list. Scene-owned ID buffers use
counted immutable leases: identical contents may be shared while reads are in
flight; different contents overwrite capacity only after all CPU leases are
released and the last consuming graph completes. Unpublished/failed use never
authorizes overwrite. Retained native ID capacity is bounded to 4 MiB and 32
entries per scene; excess uploads remain submission-owned. ReleaseRHI clears
the retained pool. Standalone requests preserve independent scene domains.
`FGPUScenePrimitiveCollector` is a stack-owned generic collector. `Add` accepts
an immutable dynamic record or a persistent primitive/instance selector plus an
instance count, returning a collector-local index. Argument-free `Commit()`
resolves those sources and publishes their selections; `GetPrimitiveData(index)`
is valid only after successful commit. Counts repeat the selected ID for existing
multi-instance geometry, preserving the current shader ABI. Missing persistent
selectors and ID-count overflow fail before dynamic allocation or ID publication.
Persistent `Add` captures the current scene's existing internal source handle on
the render thread. `Commit` validates that captured source before resolving its
stable Instance ID, rejecting removal or slot reuse between the two calls.
The StaticMesh adapter also rejects stale or foreign prepared SceneInfo references.
Callers do not supply an additional generation argument or public identity type.

Sharing a dynamic record reuses its allocation; distinct records remain distinct
even when their transported bytes match. A changed record is a new identity.
DynamicContext owns this shared allocation identity and the concatenated upload.
Buffer sets directly retain DynamicContext; the context tracks pending buffer
sets weakly. Collectors may be destroyed before upload, and dropping the last
buffer/context owner releases the scope without a collector/context ownership cycle.

`CollectGPUScenePrimitives` is the StaticMesh preparation adapter. It owns factory
eligibility, hardware-instance counts, persistent/dynamic source selection,
fallback record creation and projection into the prepared primitive order. Neither
the collector nor DynamicContext receives a prepared StaticMesh view.
All receiver/cascade collectors commit before the shared `Upload`, so growth
completes once and every view binds the same final attribute buffers.
`UpdateCPUData` refreshes membership/ID lookup without executing a graph.
Independent preview/picking requests explicitly collect and upload using their
own contexts. The submission registers terminal drawing-graph use once.
Durin still executes one dedicated attribute upload graph before native binding
construction; UE can author uploads in its drawing graph. This integration boundary
does not change range retirement or permit per-view attribute graph execution.
Factories opt in through `SupportsGPUScene`; unsupported factories retain uniform access.
The collector creates a direct ID list, repeating the selected ID for existing multi-instance
geometry. `InstanceIdOffset` overrides emitted `FirstInstance`; source draw
arguments stay unchanged. Both direct and indirect vertices index the ID list
with `SV_InstanceID + SV_StartInstanceLocation`. Fragments needing object-space
data receive a flat selected Instance ID; referenced custom channels can remain
flat slot varyings. Standalone HitProxy uses scoped records and retains its
per-draw picking uniform.

## CPU Dynamic Instancing

`FMeshDrawCommand::MatchesForDynamicInstancing` compares effective pipeline,
shader/layout/declaration, geometry, streams and bindings. The uint32
`GetDynamicInstancingHash` only narrows candidates; full equality remains required.
Wrapper addresses do not establish compatibility. Cached commands may share a
storage-scoped state bucket and canonical command/pipeline, while every primitive
retains independent `FCachedMeshDrawCommandInfo` invalidation metadata. Dynamic
commands use effective-state comparison. Frame bindings, scene buffers and bias
must also match. Invalidated sources are rejected before borrowed resources are read.

Cached publication requires a Pipeline owner matching the command pointer; the
state bucket retains it until source invalidation. Dynamic storage has no standalone
publication entrypoint; all builds use a draw-list context.

Pipeline identity includes pass, raster/depth/blend/targets, factory/layout,
native shaders and declaration elements. FMeshPassPipelineInitializer is mutable
construction input; FMeshPassPipeline owns an immutable copy exposed through
GetInitializer and builds its identity once at construction. PreparePipeline
publishes a const shared pipeline only on success, without mutating prior outputs.
Cache entries access commands through their state bucket rather than a duplicate
command owner. The state bucket retains its canonical pipeline until the
last source is removed; weak bucket lookups are pruned. `FindCachedCommand`
returns command memory and matching source info together, validates owner/keys/
optional input, and leaves material output untouched on failure. Retained command
memory does not retain invalidated source resources.

`SetupMeshDrawCommands` runs after final component admission. It compacts compatible
Local opaque/masked draws, uploads packed instance IDs and stores source mapping
on each pass. Original multi-instance, Spline, translucent and instance-frequency
inputs remain separate. `bEnableDynamicInstancing` independently controls this CPU
compaction. Source counts distinguish logical draws from actual native submissions.
Visible commands retain source ranges and submission overrides, never scene-domain
owners, preparation eligibility flags or borrowed CPU instance-ID spans.
BuildMeshDrawCommandCullingPayloads creates generic source-indexed preparation
records on demand. Culling Setup consumes them synchronously; neither final
commands nor contexts retain those preparation records or spans. CPU compaction uses source ranges to build the merged ID upload.

`FMeshPassProcessor` binds its draw-list context through CreateMeshPassProcessor
or SetDrawListContext. The context supplies the build's binding cache; AddMeshBatch
accepts per-draw batch/input and optional cached-command reuse. Local instances
and contexts are reused per pass/build destination. Contexts retain storage and
output lists, never per-command input/output references. AddCommand selects writable
build storage: dynamic contexts append a stable frame command, while cached contexts
reuse the single-command initializer before state bucketing. FinalizeCommand
publishes the stored dynamic command without moving it again, or deduplicates and
publishes the cached command. Only finalize a command returned by the same context,
once; only FinalizeCommand marks the command complete. SetDrawParameters prepares
geometry without finalizing; AddCommand starts an unfinished build even when seeded
from a completed command. Published commands are immutable. Failed builds publish
nothing, and unfinished dynamic entries expire with frame storage. Callers keep a
context alive across processor use, with no reverse binding or destructor
detach protocol. Shared prototypes serve read-only selection/PSO preparation and
never receive build contexts.

`FResolvedStaticMeshView` owns the pass collection. Dynamic command storage owns
append-only stable-address values. Context finalization publishes an aliasing const
command pointer that retains the allocation directly; there is no intermediate command
handle/lookup. Old-frame storage rejects new publication, and Pass::Draw rejects
expired-frame commands before accessing borrowed resources. Storage owns no
visible lists or cache metadata. A pass retains one dynamic allocation owner and
its cached command/info/frame-binding owners. Each pass also retains distinct
scene buffer-set owners and its merged instance-ID buffer once, preserving dynamic
allocation leases across direct drawing and failed-culling fallback. Dynamic
visible entries borrow the command's binding value. Const pass lookup never inserts; mutable lookup enters
preparation. Nonempty passes must be finalized before culling setup; empty passes
can be sealed there. Setup occurs once, and late publication is rejected.

`FInstanceCullingContext::SetupDrawCommands` compacts admitted GPU groups in the
visible array. Exactly one submission-info entry corresponds to each final command;
`Draw` start/count ranges index that final array. Source lookup and base-pass ranges
are rebuilt after compaction; groups never cross base-pass buckets. Merging multiple
commands retains scoped original sources for failed-dispatch fallback. Already
CPU-compacted singleton groups need no source copies. Invalidated sources reject
the GPU group before native submission.

`FParallelMeshDrawCommandPass::Draw` borrows the prepared span and delegates to its
owned culling context when present. Direct, CPU-instanced and indirect paths use
the same retained command. A rejected command does not suppress later commands.
Sorted translucency preserves its prepared order. Draw is serial within the supplied
command list; independent worker shadow recording owns separate lists.

RDG consumers declare nested `FInstanceCullingDrawParams` with instance-ID and
indirect-argument byte ranges. `GetMeshDrawCommandOverrideArgs` combines declared
and relative offsets once through Durin's explicit resolver, producing borrowed
native overrides. `FMeshDrawCommandSceneArgs` lowers per-draw selection without
mutating commands/bindings; `PrimitiveIdOffset` is measured in uint32 ID elements.

`FMeshDrawCommandPassResult` carries functional completion, source outcomes and
native/indirect submission counts. Optional detail belongs to
`FMeshDrawCommandStatsManager`: named CPU-draw/GPU-visibility requests allocate
pass blocks only on demand. A RenderView statistics output requests CPU detail;
a telemetry observer additionally requests GPU visibility. `DrawData` records
source/count/outcome facts for feature-owned classification and reduction.
Rendering decisions never depend on these observations.

`QueueDrawRDGIndirectArgsReadback` owns the typed rooted copy pass and query.
Features provide the indirect range and completion predicate; telemetry retains a
const result handle that may outlive the manager. Each statistics query represents
one observation and owns its readback directly. Observers receive only GetState
and GetCounts; transport mutation belongs to the statistics manager. RHI-object
reuse does not reuse an already parsed query. Failed
submission cancels the observation. Ready bytes are parsed between Lock/Unlock with exact shape/count
validation. RHI owns staging capacity/retirement; observations never wait for GPU.
See [buffer readback](../RHI/RHIResourceViewsAndTransfers.md#asynchronous-gpu-buffer-readback).

## Instance Culling Contexts

Resource preparation freezes the GBuffer's EInstanceCullingMode as Disabled,
Unsupported, ResourcesUnavailable or Enabled. Only Enabled enters culling setup.
Shader/PSO unavailability therefore creates no culling context, compacted GPU
groups, candidate/argument buffers or visibility readback. Graph construction
consumes the frozen decision and never retries resource creation. Pass/context
setup accepts admitted work only; it does not repeat requested/capability policy.

`FInstanceCullingManager` owns compute readiness and registered view snapshots;
graphs/passes retain their contexts. Construction requires a resource coordinator
for the manager and a manager for each context; these borrowed dependencies outlive
their users. Resetting registered views does not invalidate pending contexts.
Setup validates scene IDs and admits complete consecutive runs by effective
command, scene buffers, bindings and bias. Intervening incompatible
commands split runs; a context rejects foreign scene-buffer domains.

`BuildRenderingCommands` declares graph-owned candidate, visible-ID and argument
buffers plus the scene-data compute read. Each candidate is an eight-byte
Instance-ID/argument-index pair, without copied transforms or bounds. Compute
decodes the 18-float4 instance ABI and high/low origins. Pass draw parameters
publish ID and argument ranges; counted frame/graph owners retain their resources.

Statistics distinguish policy-disabled, unsupported, resources-unavailable,
no-eligible-group and dispatch-failure views. GPUCullingDirectDraws counts actual
local direct submissions in requested-culling views, including normal routing;
it is not an error count. No aggregate fallback counter aliases those reasons.

Only successful dispatch enables indirect selection. Pre-dispatch failure uses
complete original direct/CPU-instanced draws. Abandoned, rejected or allocation-
failed graphs publish no GPU completion. Retry uses a fresh context/builder;
post-recording submission failure is terminal and never redraws late.

Candidate IDs and visible IDs cost 12 bytes per instance; indexed arguments cost
20 bytes per group. These transient charges exclude scene attribute capacity,
which is observed once per referenced scene/view. Additional views do not upload
unchanged attributes. The default-disabled GBuffer policy and 64-candidate floor
remain unchanged; remaining qualification is tracked in the
[GPUScene plan](../../../Plans/UEAlignedGPUScene.md).

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
`InstanceIdBufferCreates`, `InstanceIdBufferReuses` and `InstanceIdUploadBytes`
separately count admitted ID allocations, immutable/storage reuse and recorded
ID writes; unlike attribute transaction counters they describe recording, not
successful graph completion. `InstanceIdCapacityBytes` counts retained native
ID-pool capacity and is separate from the three attribute buffers.
`CPUPrimitiveUpdateVisits` counts queue entries visited, including stale entries
discarded after removal. `UpdateQueueCapacityBytes` reports retained membership
bits, dirty index vectors, and primitive update/scratch queue capacity. This is
additional CPU storage, excluded from MirrorCapacityBytes; allocator/node overhead
and per-primitive source membership arrays remain excluded.

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
