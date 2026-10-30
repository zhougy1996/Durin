# Asset Compilation

Summary: Define the Engine-owned object-aware compilation aggregate, class routing, and compiler lifetime contract.

Modules: Engine, Launch, TextureCompressor, MeshBuilder

Last reviewed: 2026-10-20

`FAssetCompilingManager` is the one process authority for asynchronous asset
compilation. Launch starts it after Core task scheduling and pumps it once per
normal GameThread frame. The aggregate owns compiler registration, reflected-class
routing, bounded processing, aggregate progress, successful
post-compile notification, and shutdown placement. It does not impose one
typeless compiler payload, DDC key, queue, or result-application policy;
each Engine-owned typed manager retains its values and invariants.

Aggregate `Start` returns void and is idempotent while running. It requires the
GameThread once thread identity is established and cannot run after terminal
shutdown. Those violations are process contracts; individual compiler `Start`
operations return `FAssetCompilerStartResult`, deriving success from an error
code. Scheduler unavailability, undrained requests and missing state are distinct;
undrained results retain pending-request count.
Registration retains the error code and pending-request count in its structured
`StartCause`; initialization preserves that cause without formatting it into text.
Only diagnostic presentation calls the error formatter. No string-output
Start overload remains. `RegisterCompiler` returns a typed result containing the
move-only registration handle and an error code; invalid inputs, wrong thread,
stopped admission, duplicate name/class and startup rejection remain distinct.
Errors own compiler/class identity. A rejection after startup stops and shuts
down the private manager before returning its cause, without publishing routes.
Logging formats the error after it leaves the registration boundary.
`InitializeAssetCompilingManager` returns a typed initialization result retaining
the complete failed registration cause after aggregate shutdown. Successful
initialization still transfers built-in handle ownership to the aggregate.
Aggregate diagnostic snapshots contain counters, lifecycle state and compiler
observations; they have no unused message collection.

The built-in compilers are `Durin.Material`, routed from `DMaterial` and `DMaterialInstance`, and
`Durin.Texture`, routed from `DTexture`, and `Durin.StaticMesh`, routed from
`DStaticMesh`. Optional modules may register additional
compilers and class routes while the aggregate is accepting requests. Runtime Engine does not require
TextureCompressor or DerivedDataCache in Game: editor-enabled Engine optionally links
DDC, and authoring targets load `TextureCompressor` through the Engine-declared
`ITextureCompressorModule` contract. Game deployments retain Engine runtime assets
without admitting authored texture builds.

## Compiler Registration Contract

Each `FAssetCompilingManagerRegistration` supplies one canonical unique compiler
name, one `IAssetCompilingManager`, and one or more unique non-null reflected
asset classes. Compiler names identify diagnostics and post-compile events;
they do not route objects. Two live registrations cannot claim the same exact
class. Duplicate compiler names, duplicate submitted classes, and exact-class
conflicts reject the complete registration without publishing any route.

One compiler may own several root classes without adding another lifecycle
entry. Independent compilers use canonical-name order; shutdown first closes
every compiler's admission, then finishes and shuts them down in reverse order. Compiler calls and
post-compile listener dispatch never hold the aggregate registry mutex.

Normal processing has a process completion limit of 64. The aggregate gives
each ready compiler a bounded first opportunity, rotates peers, and
reclaims quota unused by idle compilers. Concrete managers report consumed
completions separately from successfully published live objects.

## Object Operations and Result Application

Selected finish and cancellation resolve each valid object's exact `DClass`, then
walk its superclass chain to the nearest registered route. Resolved objects are
grouped by compiler while retaining their per-compiler input order, and each
compiler is invoked once with only its group. Unregistered objects are no-ops.
Cancellation is advisory and does not imply quiescence; a caller requiring an
asset-visible terminal state follows it with selected finish. Finish-all is
reserved for actual global barriers.

Package reload applies that exact sequence to only the old target graph before
candidate construction. Candidate objects have new handles and therefore cannot
consume late results addressed to the old generation. Texture2D uses the typed
asynchronous route for ordinary edits and PostLoad. Reload explicitly finishes
the candidate's platform cache before checking its render resource;
VolumeTexture remains on its synchronous module build path.
Material candidates use their independent request generations and selected finish.
Reload never drains unrelated compiler work through `FinishAllCompilation`.

Concrete managers retain generation-safe object handles and independently named
request serial, authored/build identity, target, and dependency qualifiers.
They admit results only on GameThread after every family-specific qualifier
still matches. Workers receive detached immutable values and never resolve or
mutate managed objects. A successful, current result application is returned to the
aggregate as a weak object identity. Failed, canceled, superseded, destroyed,
and stale results do not emit success.

The aggregate coalesces duplicate successful object reports per compiler and
broadcasts `FAssetPostCompileData` outside compiler calls and the registry mutex.
Listeners may submit future work, but event dispatch does not recursively enter
a compiler callback.

## Module and Task Lifetime

Built-in and external registration use the same move-only
`FAssetCompilerRegistrationHandle`. Reset removes the compiler and its routes
from future snapshots, stops admission, finishes accepted work, invokes compiler
shutdown, and releases the registered provider. Registration and reset run on
GameThread outside compiler callbacks. Owners release remaining provider and
callback copies before DLL unload under the
[explicit module unload contract](../../Core/ModularFeaturesAndModuleRetirement.md).
Typed-feature retirement does not suppress these cleanup calls.

Concrete managers retain their own task handles, cancellation sources,
concurrency and memory bounds, mailboxes, diagnostics, and timeout policy. The
aggregate does not add a compilation thread pool. Process shutdown completes
the aggregate before Core closes task admission.

TextureCompressor does not own those task handles or return asynchronous tasks.
Engine calls its fixed module interface for synchronous value-only build operations.
The interface includes `Texture2DBuildTypes.h`, `TextureCubeBuildTypes.h`, and
`VolumeTextureBuildTypes.h`; Cube and Volume CPU data live in their respective
`Data.h` headers, independently of the texture object classes. Cube normalization
borrows a `FTextureCubeNormalizeRequest` containing input values and target settings.
Cache policy, cache diagnostics, and object application remain in the Engine
orchestration headers and do not enter the module request.
Module inputs use `FTexture2DBuildInput`, `FTextureCubeBuildInput`, and
`FVolumeTextureBuildInput`. Cube and Volume return platform-data ownership directly;
Texture2D returns `FTexture2DBuildOutput` with platform data and build timings.
The module exposes algorithm versions directly, including the separate Cube
projection version. DDC keys and Cook inputs retain these versions without a
producer descriptor or identity string. Volume detached construction returns its
Engine build product directly, without a separate value wrapper.
Texture2D and TextureCube PostLoad place build operation work on an Engine-owned worker;
VolumeTexture remains synchronous. TextureCompressor stays resident throughout the editor
lifetime, so queue entries, platform-cache inputs and scene import workers need no
module sessions. Consumers stop admission and drain workers before normal module
shutdown. Texture2D and TextureCube owners dispatch task-scheduled shared-output
builds, releasing their worker during cache I/O; Volume uses the protocol
synchronously. TextureCompressor registers
the Engine function adapters during module startup. Engine lazily creates the
texture-family session from the process-wide production `GetBuild()` service;
there is no explicit Engine asset-build service initialization. Consumers stop
admission and drain compilation before producer modules unload and Core task
services stop. See [Derived Data Build Protocol](DerivedDataBuild.md#ownership).

## Initial Compiling Managers

Material compilation remains Engine-owned. Its operation captures on GameThread,
submits detached cached translation, merges normalized program flights, subscribes
to asynchronous ShaderBuild, and assembles a candidate on CPU before mailbox
publication. Cancellation is consumer-scoped; last-consumer cancellation latches
the operation and prevents reattachment. Selected finish raises downstream DDC
and Shader priority and joins all dispatch publication, cache owners, subscribers
and CPU tasks. Shutdown closes admission before draining those controls; progress
before final application requires no GameThread pump. Count and byte reservations
remain held until retirement. It preserves program-identity
single-flight sharing, retained program results, last-known-good visibility,
generation-safe admission, Renderer publication, reload behavior, and cooked
program rules. Remaining count is the number of live outstanding material
consumers plus owners deferred by capacity, rather than shared worker flights.
Both material asset kinds use the same owner state, selected finish/cancel, reload
and offline preparation boundary. Deferred owners retain only retry intent; each
pump retries at most 256 owners in rotating handle order using fresh snapshots.
Stop-admission and cancellation clear that intent. Each owner admits a complete
program, parameter contract and render configuration together; equal compiler
inputs share immutable code while retaining independent status and admission
tokens. Cooked runtime does not submit material compiler work.

Texture compilation is Engine-owned. One `FTextureCompilingManager` owns
typed asset state, worker admission, priority fairness, memory budget,
cancellation, the completion mailbox, per-texture mutation barriers, GameThread
completion application, and exactly-once completion callbacks. Active records are keyed by
`FObjectKey`; the manager owns active/last request ids,
failure state, and bounded terminal diagnostics. Active work is erased after terminal
delivery, while completed asset diagnostics remain under a separate bound.
The work record and object share one progress snapshot; final application writes
its phase and diagnostic before notifying the observer. Texture2D validates the
complete candidate, then commits source/settings/platform data through a private
member without reopening the public setter mutation barrier.
Builder identity and target/profile remain at the DDC boundary. Object completion
carries only the selected source identity and settings needed for application,
independently of GPU resource readiness. `BuildElapsedNanoseconds` measures wall time from
worker dispatch start to build completion, including asynchronous cache I/O;
it is not CPU execution time. Queue wait and owner completion timing are separate.
Working-set estimates read dimensions from captured source metadata. There is no
preparation timer around the move of that request value. Synchronous platform-data
builds return a complete product or an error without output parameters. The
texture DDC session seam returns `FBuildCompleteParams` directly, translating an
admission failure into `EStatus::Error` rather than wrapping completion in another
expected result.

Texture2D and TextureCube dispatch run in a `Tasks::TTask<void>` on the CPU executor and submit
to a persistent task-scheduled build session. Interactive/background priority is
forwarded to both Core dispatch and the build request owner. LaunchTask accepts
valid work directly; manager request and memory limits remain separate from
scheduler queueing. Dispatch returns without waiting for cache I/O or a producer.
The callback owns its pending CPU candidate; the build callback fills it and publishes
a mailbox result. Application transfers that same candidate allocation to the texture. Work results explicitly carry status, a product and diagnostics.
All work results travel through the mailbox; dispatch tasks return no product.
The GameThread also reaps skipped or failed dispatch bodies into that mailbox
after task completion. Posting a result alone does not permit delivery. The
producer task must complete and its DDC owner's `Poll()` must confirm callback
and native dispatch capture retirement; see [Derived Data Build](DerivedDataBuild.md#ownership).
Dispatch
failure or cancellation before its body starts also reaches terminal delivery.
The GameThread pump applies results and invokes observers without another deferred
task. Diagnostics retain no task handles, build owners or input/result payloads
after delivery. Synchronous detached/import/Cook adapters retain inline sessions.

Retirement has two boundaries. Compute slots and estimated working bytes are
released only after producer completion and DDC owner `Poll()` confirms callback
and native capture retirement. Pending-request capacity remains charged through the
GameThread observer callback; its return retires the request's retained inputs,
mailbox product, task handle and build owner. Cancellation requests neither imply
quiescence nor bypass these boundaries. Mutation waits for the selected request
to pass both boundaries before changing the texture.

Cube platform-cache inputs dispatch through the same build request owner. Canonical
six-face and HDR panorama sources can query cache before reading pixels.
Noncanonical panoramas acquire source bytes and normalize on the dispatch worker,
then submit their canonical source to the asynchronous session. Source validation,
normalization and output assembly are shared with synchronous Cube adapters.
Current-asset caching and full-object edits use the common texture entry points;
there is no Cube-only rebuild boundary. Cube result objects
apply only after successful work and live-owner validation. Mutations finish the
previous request before changing source or settings.

The default manager allows two active computes with a 1 GiB estimated working
budget and an oversized-single-compute rule. This is a scheduling estimate, not
a hard allocator limit. An admitted compute retains its slot and estimated bytes
through cache query, producer continuation, persistence and callback retirement,
even while no worker is occupied. At most 1,024 requests may be pending, counting queued
inputs, running computes, results awaiting application and callbacks in progress.
The count is released after delivery returns; there is no separate retained-result
byte reservation. Owner processing and explicit waits reap completed computes
and admit the next requests with the existing interactive/background fairness.
Timed waits poll compute readiness and advance scheduling without applying
unrelated results. Shutdown stops admission, cancels queued and running work,
pumps all terminal results and joins the scope. Build cancellation runs outside
the queue mutex because queued builds may finish synchronously during cancellation.
Shutdown drains both dispatch tasks and asynchronous build owners before module
unload. Even a task canceled before its
body starts is finalized by the manager. Completed request records release their
module sessions while retaining bounded diagnostics.
If Core task scheduling has already stopped, processing cancels queued requests
instead of attempting a new dispatch. Normal shutdown still drains owners before Core.

Build operations, DDC ownership, and typed build application are defined by
[Asset Data Lifecycle](../Packages/AssetDataLifecycle.md#serialization-and-production-ownership).
The texture manager routes `DTexture`, sharing its queue and limits between
Texture2D and TextureCube. VolumeTexture currently builds synchronously and has
no outstanding compilation to finish. Authored PostLoad submits detached source
metadata; DDC reads, source payload reads/decompression and build operation work happen
on workers. Warm supported cache hits do not recover the source pixels.

`DTexture::BeginCachePlatformData()` starts caching and is idempotent for an
installed or pending current request. `IsAsyncCacheComplete()` observes terminal
CPU state, including failure, and does not imply GPU readiness.
`FinishCachePlatformData()` explicitly waits and applies this object's result,
returning whether usable CPU platform data is available. Cook and reload use
this blocking boundary; thumbnails poll and retain their placeholder instead.

The editor calls `ProcessAsyncTasks(true)` with a shared 2 ms deadline and the
normal completion-count limit. Texture delivery checks that deadline between
results. A single apply cannot be preempted, so this is a cooperative budget.
Selected finish applies only the selected request, even when unrelated results
are ready.

### Texture2D Completion

Work completion and object completion are distinct boundaries. Texture work uses
the existing DDC `EStatus` semantics: `Ok` carries a CPU product, `Error` carries
diagnostics without a product, and `Canceled` discards the product. Error codes
describe causes; neither diagnostic text nor UI phases determine request status.
This follows [UE request status](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/UE__DerivedData__EStatus).
Cache-hit/build flags remain execution facts, not success or failure outcomes.

The GameThread validates the owner and active request, then applies successful
work. One finalization path records the object terminal result before notifying
the observer. A successful worker can still fail object validation or application.
`Building` includes worker preparation and DDC execution. `AwaitingApply` means
a work result is posted, including failed or canceled work; terminal phases are
published only by the GameThread owner. `Ready` requires successful application. Cancellation is a separate terminal
outcome. A texture has at most one active request; replacing a request first
cancels, waits for callback retirement, and delivers its canceled terminal result.

`FTextureOperationResult` identifies one terminal outcome with
`ETextureCompilationStatus` (`Succeeded`, `Failed`, or `Canceled`) and one owned
diagnostic. An accepted request invokes its `FTextureCompilationCompletion` exactly once on
GameThread. Rejection before acceptance is returned synchronously without
invoking completion.

Compiler phases and detailed diagnostics remain private; the `AssetPrivate`
test hook exposes a bounded snapshot for qualification and regression coverage.
Its `Ready` phase means CPU platform data was applied, not GPU readiness.
Editor availability and retry presentation use installed data and completion
queries, without retaining a second object-level build status. Object consumers use
`IsAsyncCacheComplete` for completion (including failure/cancellation), and
`FinishCachePlatformData(TimeoutSeconds)` for an explicit selected wait and Apply.
The aggregate `FAssetCompilingManager::OnAssetPostCompile` emits successful
GameThread applications with compiler name `Durin.Texture` and weak object
identities. Failure and cancellation publish diagnostics without a success event.
A full-object `PostEditChange` retires prior work and triggers compilation;
named-property notifications retain their validated application path and do not
restart a completed build.

`DTexture::BlockOnAnyAsyncBuild` finishes the selected texture through the asset
compiling manager, including GameThread result application. It does not cancel,
clear terminal diagnostics, start another build, or wait for GPU initialization.
Source/settings setters and synchronous replacement builds use the existing
private mutation barrier: it cancels and finishes only that texture, delivers
the old observer as `Canceled`, and retires its request before returning.
Cancellation is not an error and carries no cancellation-reason code.
Texture2D imports commit Source/settings/import metadata before starting DDC work.
Build failure or cancellation does not undo that authored commit. The import
result describes the commit, while compiler status describes platform-data
readiness. Importer teardown retains committed assets and drains current work;
batch cancellation before commit discards captured input.
Same-owner submissions from inside this barrier are rejected. New submissions
then start with no active predecessor; no latest-wins serial is needed. Storage-only
source recompression preserves semantic identity and retained read handles.
Editor save and compensation adapters consume this terminal contract through
[Async Asset Operations](../../../Editor/Architecture/Assets/AsyncAssetOperations.md).

## Proven Reuse Boundary

Material and Texture2D prove a common lifecycle shape, not a common compiler
state object. Reuse stays at these Engine-owned boundaries:

| Stable boundary | Shared rule |
| --- | --- |
| Aggregate/compiler contract | Stop admission, process bounded completions, route object operations, finish accepted work, then shut down. |
| Object identity | Use `FObjectKey` for identity and weak references for deferred owner access; never use an asset path as live-object identity. |
| Freshness | Carry an independently named per-object completion epoch. Material uses authored/dependency revisions and generation; Texture2D finishes the previous request before mutation or replacement. |
| Detached completion | Workers produce family-owned value envelopes; only the GameThread resolves the owner and attempts result application. |
| Cancellation and terminal delivery | Cancellation is advisory, late results are consumed, and every accepted consumer reaches one typed terminal outcome. |
| Lifetime accounting | Active records end with terminal delivery; only explicitly bounded diagnostics or family caches may remain. |

These are contract conventions and existing Core/Engine primitives, not a new
typeless job framework. Material retains program-identity single-flight,
multiple consumers, retained programs, last-known-good behavior, Renderer
publication, and its authored/dependency checks. Texture2D retains its priority
queue, byte budget, synchronous module/DDC boundary, mutation-aware CPU
payload result application, and separate GPU resource enqueue. Their managers remain
typed because a shared state bag would hide rather than enforce those
invariants.

Catalog revision, package format/schema version, object load generation, build
and DDC producer identity, cook target/profile, CPU payload readiness, GPU or
physics resource readiness, mutation transaction state, and
cancellation/shutdown/module-owner state remain orthogonal. Package loading is
not an asset compiler, and Durin has no global composite asset-status
enum. Another asset-family migration requires its own plan once
its owner, producer boundary, publication transaction, and readiness semantics
are known.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Packages/AssetDataLifecycle.md)
- [Runtime Lifecycle](../../Application/RuntimeLifecycle.md)
- [Texture System](../Families/TextureSystem.md)
- [Material System](../../Rendering/Materials/MaterialSystem.md)
- [Modular Features And Module Retirement](../../Core/ModularFeaturesAndModuleRetirement.md)

## StaticMesh Completion

`Durin.StaticMesh` owns render requests; `Durin.PhysicsMesh` owns BodySetup cooking
requests. Each has its own task handles, typed records and owner-thread completion
path. The render manager has no collision payload variant or physics terminal
conversion. Physics scheduling lives under Engine/Physics and does not require
the StaticMesh manager or MeshBuilder. Ordinary Build/BuildFromSource, PostLoad
complete at render commit; standalone import completes at authored-source commit.
Collision failure is reported through
`GetCollisionBuildStatus` and `GetCollisionBuildError`. Pending render work alone
does not imply pending collision. Render callbacks never wait for collision.
`DStaticMesh::IsCompiling`, `FinishCompilation` and `CancelCompilation` describe
render work only. `DBodySetup::IsCompiling`, `FinishPhysicsMeshes` and
`AbortPhysicsMeshAsyncCreation` describe physics work, including cancelled requests
awaiting retirement. Whole-source mutation uses `BlockOnAnyAsyncBuild` to cancel
and retire rendering plus the selected asset's owned/current BodySetup work. Saving
and importer cleanup explicitly wait for both where needed. Aggregate finish-all
repeats until every compiler is drained, including physics work submitted during
render publication. Aggregate shutdown stops admission and retires both families;
local render-manager shutdown leaves the physics service available.

Render owners dispatch a persistent asynchronous DDC session and release their
Core worker during cache reads and persistence. Each record owns a plain DDC
`FRequestOwner` for priority, cancellation and retirement. DDC callbacks publish only
detached completion data. Dispatch releases its asset execution slot when its CPU
function returns, even while its DDC requests remain pending. The asset scheduler
requires both producer task completion and owner `Poll()` before readmitting output
assembly, metadata restoration and ray-query preparation on a Core worker. Any typed
compiler pump or explicit finish can advance this scheduler without applying unrelated
assets. No render worker waits for
cache I/O or another render task. Shutdown drains these suspended requests before
closing the manager scope. Queued records cancel without dispatch if Core has
already stopped.

Render workers own detached source and slot metadata, without material object
bindings. The owner snapshot retains bindings and captured provenance identity until publication.
Application rechecks source identity, normalization, slot bindings and provenance.
The immutable render builder version is captured at admission for diagnostics; joining
and publication do not reread it. MeshBuilder remains resident throughout the editor
lifetime. Render workers borrow its interface directly; consumers stop admission
and drain work before normal module shutdown. Source-changing operations are discarded after owner edits,
without automatic requeue. Current-source rebuilds may requeue stale render facts.
Synchronous Build cancels and retires older work for the selected mesh, builds CPU
render data directly and publishes it, returning `expected<void, string>`.
AsyncBuild success means admission only; it has no per-request completion callback
or separate result envelope. Object queries and aggregate `OnAssetPostCompile`
notifications describe completion. Detailed request identity and bounded failure
text remain in the diagnostic history. Standalone import commits Source, slots
and provenance together, clears old render/collision projections, then triggers
`PostEditChange`. Compilation failure never rolls back that authored commit.
Source-changing compilation retains candidate Source and slots until render
publication. Scene independently submits prepared render/source/slots/provenance
through its direct commit; compiler requests have no `PreparePublication` callback. Collision failure does not undo publication.

`DStaticMesh::IsCompiling` queries render work without pumping, including canceled
records whose native workers or DDC callbacks have not retired. Collision readiness
remains a separate BodySetup query. `BlockOnAnyAsyncBuild` cancels and retires the
selected mesh's render and collision work before authored mutation. The retirement
barrier rejects same-owner resubmission from callbacks and leaves unrelated assets
unapplied. `CancelCompilation` requests cancellation; it does not itself wait.
Object finish uses the aggregate `OnAssetPostCompile` notification for successful
render publication; canceled, stale and failed work emit no success event.

Cancellation requests suppress publication immediately but do not finalize a running
record. Every terminal, including cancellation and supersession, waits for the CPU
task completion and DDC owner retirement. The owner removes the record from its queue
before application/notification, preventing recursive finish or pump from delivering
it again. After retirement, GameThread-local outcomes and any remaining DDC completion
take ownership from the worker before publication or notification. Ordinary local
destruction releases those outputs even if publication or an observer throws. There is no delivered record
waiting for a second cleanup pass. Pending diagnostic snapshots are not terminal;
terminal history is recorded only at this retirement boundary.

Render work has one private phase: Queued, Running, AwaitingDerivedData, or Complete.
Producer task completion closes submission; owner `Poll()` joins the DDC mailbox's
callbacks and registered native tasks. No separate completion event,
completion-ready or asynchronous-dispatch boolean is needed. CPU assembly changes
back to Running before scheduling, so a canceled/rejected continuation cannot be
mistaken for another DDC dispatch. The shared Core cancellation token controls terminal classification. Native task
and DDC request-owner observers latch into the same flag throughout dispatch and
CPU consumption. Detached render/texture APIs retain their token-based cancellation
contract. Physics `Capture`, `CookCaptured`, and `Cook` run synchronously to
completion and accept only a working-set byte budget, with no cancellation token
or callback. They return owned input/products or bounded ordinary failure text;
a successful empty physics product means no collision was requested. The physics
manager competes with worker entry to withdraw queued work. Once started, input
capture, the DDC recipe and CPU assembly complete without caller cancellation;
the manager decides whether to accept their result at publication. DDC request
options do not inherit owner/native-task cancellation from a started physics cook.
The manager clears canceled output and retains no canceled failure diagnostic.
Superseded remains a detailed stale-publication reason and physics disposition,
independently of whether its underlying work was canceled.


Collision records belong to BodySetup, not to its mesh. They own a detached
`FPhysicsMeshInputTask` and `FCookBodySetupInfo` settings snapshot. Source acquisition, normalization
and collision cooking run without render publication. A retry captures fresh source
and supersedes the earlier request. Publication checks the BodySetup object key,
settings revision and request generation,
then asks BodySetup to apply or reject the result. `FAssetBuildTaskContext` carries
shared cancellation, borrowed worker-local checkpoint metrics and the working-set
bound. Cancellation does not imply task or DDC callback retirement.
Module sessions guard only render work. Collision admission, cooking and
installation do not acquire the render build module; its absence or unloading
cannot invalidate a physics request. Manager code never writes mesh
collision state or refreshes components directly.
Direct BodySetup settings changes and mesh setters share the same invalidation
boundary. Invalidation clears derived geometry and refreshes component physics
bodies immediately; accepted collision installation refreshes them again.
Authored primitives survive render replacement. Installing/clearing geometry does
not recursively schedule a request or invalidate render work.

Both managers use one asset-layer execution scheduler and priority queue. It
admits at most two CPU stages and uses a default 1 GiB estimated execution-memory
budget. Requests queue when another executing stage leaves insufficient budget;
they do not fail admission due to temporary memory pressure. With no executing
stage, one oversized estimate may run alone to prevent starvation. The constructor
accepts concurrency and memory-policy limits. This is a scheduling heuristic, not
a hard allocator limit or a cap on retained sources, DDC outputs or notifications.
There is no separate mesh reservation, record quota or per-manager accounting ledger.

Each request still has a checked 512 MiB construction envelope. Render estimates
use `1 MiB + 64 * canonical bytes + 1024 * mesh count + 32768 * slot count`.
Authored collision estimates use `1 MiB + 64 * canonical bytes + 1024 * mesh count`
without decoding on the owner thread. Source-less resident captures estimate
`1 MiB + 512 * positions + 192 * indices`. Provider captures declare the complete
working-set bound, and builders enforce construction envelopes. Typed diagnostic
`EstimatedWorkingSetBytes` values sum pending request estimates; they are not
budget charges and exclude records already removed for notification. The asset
scheduler's scheduled-memory observation counts only leased execution capacity.

Render and physics dispatch release execution capacity and its memory estimate
while waiting for DDC. CPU consumption reacquires both resources only when
producer task completion and DDC owner `Poll()` confirm callback/native-task retirement. Cancellation cleanup requires
retirement but no execution capacity. Physics finish advances only the generic
asset scheduler, with no render-manager progress callback and no application of
unrelated render results. Execution slots are owned only by callable captures.
Core destroys the callable after body exit, before terminal publication; a queued
cancellation request retains its slot until the body executes and retires. The
slot destructor releases both resources exactly once. The asset
scheduler retains no running-task table or explicit release flag. Owners retain
task handles for DDC prerequisites and terminal retirement. Records retain detached
input and products independently of scheduling estimates. Finalization removes the
record before notifying owner code and transfers its outputs to local ownership
through notification return, including exceptional exits; recursive finish cannot notify twice.
Explicit mesh/physics finish uses owner `Wait()` only after the producer finishes,
when no further submission can race that wait. Shared private helpers observe
native task cancellation and join request owners; typed workers retain compensation,
result classification and publication. Mesh publication explicitly receives the
local outcome instead of reading the worker mailbox. No output cleanup guard remains.
There is no asset-specific DDC event bridge or additional build framework.
Retired DDC requests that need no CPU consumption use the same owner cleanup path
whether reached from continuation admission or an explicit owner pump.
Collision-only work never changes render resource revision.

Background and interactive queues are FIFO with at most four interactive dispatches
before eligible background work. Completion pumps honor the aggregate manager's
completion count and optional deadline; there is no separate mesh completion budget
or pump identity. A time-limited aggregate pump supplies a 2 ms soft deadline.
Resource preparation and consumer refresh can exceed that soft deadline.
History retains at most 128 render diagnostics. Collision
cache warnings are logged independently; successful collision work is not reported
as another successful render compilation.

Cook waits for a pending source mutation when needed, then independently builds
required detached projections without publishing authored CPU data. Cooked
residency remains a separate manager and invokes no editor build operation.

The request-ID overload of `GetStaticMeshCompilationDiagnostic` retrieves the exact
completion even when the owner has newer work or has been destroyed. An expired
history entry returns request ID zero.
`DStaticMesh::GetCompilationDiagnostic`, the request-ID history lookup and
`GetStaticMeshCompilationManagerDiagnostics` are owner-thread, value-only reads.
They neither pump work nor perform source/cache I/O or initialize resources.
Request ID zero means no available observation, including evicted history.
A nonzero observation describes its captured source identity and render builder
version, not proof that the live asset still matches it. Match these facts
before presenting it as current. Cache origin and DDC keys are implementation
details and are not exposed through completion diagnostics.
Nonfatal cache failures are logged once at the Engine execution boundary with
function, key, stage and bounded original cause. They do not enter worker results,
completion callbacks or retained history. Reporting is independent of successful
publication, so a later construction or application failure cannot hide a cache
issue. Clean hits and misses produce no warnings.
Physics results use bounded diagnostic strings and physics completion status,
with no intermediate StaticMesh diagnostic or status conversion. Input/admission
failures notify inline; accepted work notifies after retirement. The physics record
stores cancellation in its token and forms terminal status locally during finalization.
Physics owner cancellation withdraws only unstarted cooks. Started cooks continue
through DDC and CPU consumption; owner generations suppress stale publication,
which reports Superseded. Owner abort does not cancel a running DDC request.
Selected finish and shutdown retain the native-task and callback retirement barrier.
The asset scheduler owns no products, snapshots or completion callbacks.
StaticMesh diagnostic `Status` is absent until terminal delivery; pending work
does not report failure. `Error` retains bounded owned text only for a failed
terminal, without a cancellation bit or completion wrapper. Successful, cancelled
and superseded terminals have empty errors. `FormatStaticMeshCompilationDiagnostic`
displays that failure text only.
The presentation text budget
is 4096 bytes per record including a producer identity capped at 256 bytes.
Completion diagnostics retain no cache-origin or per-product observation records. Queue counts and reserved bytes remain for admission and lifecycle
accounting. CPU completion is independent of GPU readiness. Diagnostics own no
source, payload, component or callback.

Initial-edit replacement waits for the retiring worker's storage to be released
before reclaiming that record's admission capacity. Explicit cancellation or a
new accepted request suppresses that replacement. Late cancellation remains
accounted until worker/callback retirement and the single terminal notification.
