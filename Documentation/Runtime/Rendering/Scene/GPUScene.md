# Persistent GPU Scene Storage

Summary: Define scene-owned Primitive and Instance records, incremental uploads, selected-instance shader access and allocation retirement.
Modules: RenderCore, Engine, Renderer
Last reviewed: 2026-11-12

## Scope and Ownership

`FScene` owns one `FGPUScene`, registered as a render resource. Membership,
CPU mirrors and dirty requests are independent of view visibility. Built-in Local and Spline factories consume these records in direct and indirect
Forward, GBuffer, shadow and HitProxy drawing. GPU culling is independent of
scene-data access. Unsupported factories retain the explicit Primitive uniform
contract. CPU command merging uses ordered compatible runs. Persistent culling inputs remain later work in
the [GPUScene plan](../../../Plans/UEAlignedGPUScene.md).

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
produce inactive zero records. Invalid bounds retain an invalid marker and must
be treated conservatively by future culling consumers.

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

`FGPUSceneDynamicContext` uploads attributes only for dynamic records. Factories
opt in through `SupportsGPUScene`; unsupported factories retain uniform access.
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
`FCachedMeshDrawCommandInfo`, invalidation identity and canonical pipeline lifetime.
Weak lookup buckets are pruned when their last source entry is removed.

Pass preparation merges consecutive compatible opaque/masked Local runs in
Forward, retained Forward, GBuffer and shadows. It preserves prepared order;
Spline, sorted translucency, unsupported factories, original multi-instance draws
and instance-frequency vertex streams remain separate. Visible records carry
`StateBucketId`, `PrimitiveIdBufferOffset` (uint ID elements) and `NumInstances`.
Only emitted draw arguments receive the count/offset overrides. One counted
submission-owned ID buffer serves CPU runs across its passes and declares RDG reads.
`bEnableDynamicInstancing` defaults true and independently disables this path for
reference output comparisons.

GBuffer retains original commands until GPU admission is known. A CPU run with
any indirect member uses its original direct commands for the remainder, avoiding
partial-run duplication or omission. Failed dispatch enables complete CPU runs.
Logical successful-draw counts stay conserved; `NativeDrawSubmissions` and
`ShadowNativeDrawSubmissions` count actual emitted commands. Fewer submissions
alone do not establish a frame-time improvement.

## Retirement and Recovery

`FRDGBuilder::GetGPUCompletion()` exposes a successful terminal graph join,
covering every using queue; it is absent before success. Free Primitive slots
and Instance ranges wait for the last using graph to complete. Submitted,
canceled and failed signals do not authorize reuse. Existing counted RHI
bindings and backend retirement retain replaced native allocations.

`ReleaseRHI` drops committed buffers, increments resource generation and marks
all mirrors dirty for reconstruction. Old-device retirement prerequisites are
cleared only at this ordered resource-release boundary. Source mirrors survive.
Scene teardown unregisters membership and releases its owner; already-recorded
commands retain counted native references.

## Related Contracts

- [Scene representation](SceneRepresentation.md)
- [Material custom data](../Materials/MaterialCustomData.md)
- [Frame preparation](../Pipeline/RendererFramePreparation.md)
- [Resource lifecycle](../Pipeline/RenderResourceLifecycle.md)
- [Buffer views and ordered writes](../RHI/RHIResourceViewsAndTransfers.md)
