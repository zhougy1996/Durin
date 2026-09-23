# Asset Compilation

Summary: Define the Engine-owned object-aware compilation aggregate, class routing, and compiler lifetime contract.

Modules: Engine, Launch, TextureBuild, MeshBuilder

Last reviewed: 2026-09-22

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
code. Scheduler unavailability, undrained requests, missing state and task-scope
creation failures are distinct; undrained results retain pending-request count.
Registration retains this result as its nested startup cause. No string-output
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
TextureBuild or DerivedDataCache in Game: editor-enabled Engine optionally links
DDC, and authoring targets register a synchronous
family-specific `ITexture2DBuildProvider`, `IVolumeTextureBuildProvider`, and
`ITextureCubeBuildProvider` features, while game deployments retain Engine
runtime assets and simply have no provider-backed authoring work to submit.

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
VolumeTexture remains on its synchronous provider path.
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
[explicit module unload contract](../Core/ModularFeaturesAndModuleRetirement.md).
Typed-feature retirement does not suppress these cleanup calls.

Concrete managers retain their own Engine task scopes, cancellation sources,
concurrency and memory bounds, mailboxes, diagnostics, and timeout policy. The
aggregate does not add a compilation thread pool. Process shutdown completes
the aggregate before Core closes task admission.

Provider modules do not own those scopes or return concrete asynchronous tasks.
Engine enters the single family-specific Texture provider modular feature for
one synchronous value-only call. Texture2D and TextureCube PostLoad place that
call on an Engine-owned worker; VolumeTexture remains synchronous.
Provider owner retirement closes
new admission and waits for calls already inside the feature gate; no provider
callback, task, deleter, or result lifetime escapes the call.

## Initial Compiling Managers

Material compilation remains Engine-owned. It preserves program-identity
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
cancellation, the completion mailbox, latest-wins request serials, GameThread
completion application, and exactly-once completion callbacks. Active records are keyed by
`FObjectKey`; the manager owns request serials, active/last request ids,
failure state, and bounded terminal diagnostics. Active work ends at terminal
delivery, while completed asset diagnostics remain under a separate bound.
The deterministic input/provider identity remains separate from request serials
and GPU resource readiness.

Texture2D compute returns a unique `Tasks::TTask` result on the CPU executor,
with interactive/background priority forwarded to Core. LaunchTask accepts valid
work directly; manager request and memory limits remain separate from scheduler
queueing. The manager retains the
task in its request state and checks completion during owner-thread processing.
Success, framework failure and cancellation reach
domain handling on GameThread without submitting another deferred task. Task
completion means background computation has ended; asset completion is published
by the manager after result application. Diagnostics retain neither task handles
nor input/result payloads after delivery.

The default manager allows two active computes with a 1 GiB estimated working
budget and an oversized-single-compute rule. This is a scheduling estimate, not
a hard allocator limit. At most 1,024 requests may be pending, counting queued
inputs, running computes, results awaiting application and callbacks in progress.
The count is released after delivery returns; there is no separate retained-result
byte reservation. Owner processing and explicit waits reap completed computes
and admit the next requests with the existing interactive/background fairness.
Timed waits poll compute readiness and advance scheduling without applying
unrelated results. Shutdown stops admission, cancels queued and running work,
pumps all terminal results and joins the scope. Even a task canceled before its
body starts is finalized by the manager. Provider retirement remains governed by
the modular-feature call lifetime described above.

Recipe providers, DDC ownership, and typed build application are defined by
[Asset Data Lifecycle](AssetDataLifecycle.md#serialization-and-production-ownership).
The texture manager routes `DTexture`, sharing its queue and limits between
Texture2D and TextureCube. VolumeTexture currently builds synchronously and has
no outstanding compilation to finish. Authored PostLoad submits detached source
metadata; DDC reads, source payload reads/decompression and recipe work happen
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

`FTexture2DCompilationResult` identifies one terminal outcome as `Succeeded`,
`Failed`, `Canceled`, or `Superseded` and carries a bounded diagnostic. An
accepted request invokes its `FTexture2DCompilationCompletion` exactly once on
GameThread. Rejection before acceptance is returned synchronously without
invoking completion.

A new request for an object with active work cancels the old worker and
completes the old observer as `Superseded`. A late worker result for that
generation cannot publish or complete the observer again. Editor save and
compensation adapters consume this terminal contract through
[Async Asset Operations](../../Editor/Architecture/AsyncAssetOperations.md).

## Proven Reuse Boundary

Material and Texture2D prove a common lifecycle shape, not a common compiler
state object. Reuse stays at these Engine-owned boundaries:

| Stable boundary | Shared rule |
| --- | --- |
| Aggregate/compiler contract | Stop admission, process bounded completions, route object operations, finish accepted work, then shut down. |
| Object identity | Use `FObjectKey` for identity and weak references for deferred owner access; never use an asset path as live-object identity. |
| Freshness | Carry an independently named per-object completion epoch. Material uses authored/dependency revisions and generation; Texture2D uses request serial plus deterministic input/provider identity. |
| Detached completion | Workers produce family-owned value envelopes; only the GameThread resolves the owner and attempts result application. |
| Cancellation and terminal delivery | Cancellation is advisory, late results are consumed, and every accepted consumer reaches one typed terminal outcome. |
| Lifetime accounting | Active records end with terminal delivery; only explicitly bounded diagnostics or family caches may remain. |

These are contract conventions and existing Core/Engine primitives, not a new
typeless job framework. Material retains program-identity single-flight,
multiple consumers, retained programs, last-known-good behavior, Renderer
publication, and its authored/dependency checks. Texture2D retains its priority
queue, byte budget, synchronous provider/DDC boundary, mutation-aware CPU
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

- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Material System](../Rendering/MaterialSystem.md)
- [Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md)

## StaticMesh Completion

`Durin.StaticMesh` uses one task scope, bounded queue and owner-thread mailbox
for separate render and collision records. Ordinary Build/AsyncBuild, PostLoad
and import complete at asset commit; collision failure is reported through
`GetCollisionBuildStatus` and `GetCollisionBuildError`. Pending render work alone
does not imply pending collision. Render callbacks never wait for collision.
Asset-level finish drains render records and BodySetup records belonging to selected
assets. `FinishStaticMeshCompilation` can select Render, Collision, or All;
`DBodySetup::FinishPhysicsMeshes` selects only one setup. Render-only barriers never
wait for collision. Shutdown stops
admission, cancels both kinds and drains workers before releasing storage.

Render workers own detached source and slot metadata, without material object
bindings. The owner snapshot retains bindings and provenance until publication.
Application rechecks source identity, normalization, slot bindings and provenance.
The immutable render builder version is captured at admission for diagnostics; joining
and publication do not reread it while the session pins the same module generation. Render admission acquires an `FStaticMeshBuildSession` on the
module-control thread and retains it through publication and worker retirement.
The session pins the module generation; shutdown/unload is rejected until all
consumers have stopped admission, drained work and released their sessions. Source-changing operations are discarded after owner edits,
without automatic requeue. Current-source rebuilds may requeue stale render facts.
Synchronous Build cancels older work, builds CPU render data directly and publishes
it, returning owned error strings. AsyncBuild success means admission only; its
completion describes the render result. Import publishes source, render, prepared
slots and provenance in one refresh boundary. Collision failure does not undo it.

Collision records belong to BodySetup, not to its mesh. They own a detached
`FPhysicsMeshInputTask` and `FCookBodySetupInfo` settings snapshot. Source acquisition, normalization
and collision cooking run without render publication. A retry captures fresh source
and supersedes the earlier request. Publication checks the BodySetup object key,
settings revision and request generation,
then asks BodySetup to apply or reject the result. `FAssetBuildTaskContext` carries
only borrowed cancellation, checkpoint metrics and the working-set reservation.
Module sessions guard only render work. Collision admission, cooking and
installation do not acquire the render build module; its absence or unloading
cannot invalidate a physics request. Manager code never writes mesh
collision state or refreshes components directly.
Direct BodySetup settings changes and mesh setters share the same invalidation
boundary. Invalidation clears derived geometry and refreshes component physics
bodies immediately; accepted collision installation refreshes them again.
Authored primitives survive render replacement. Installing/clearing geometry does
not recursively schedule a request or invalidate render work.

The manager allows two workers, 32 outstanding records, 1 GiB total reservation
and 512 MiB per request. Render admission uses checked arithmetic for
`1 MiB + 64 * canonical bytes + 1024 * mesh count + 32768 * slot count`.
Authored collision reserves `1 MiB + 64 * canonical bytes + 1024 * mesh count`
without decoding on the owner thread. Source-less resident captures reserve
`1 MiB + 512 * positions + 192 * indices`. Provider captures declare the complete
working-set bound, and builders enforce construction envelopes. Canceled
records keep their count and byte charges until their task and retained products
retire. Collision-only work never changes render resource revision.

Background and interactive queues are FIFO with at most four interactive dispatches
before eligible background work. Each pump processes at most two terminals within
a shared 2 ms soft deadline. Resource preparation and consumer refresh can exceed
that soft deadline. History retains at most 128 render diagnostics. Collision
cache warnings are logged independently; successful collision work is not reported
as another successful render compilation.

Cook waits for a pending source mutation when needed, then independently builds
required detached projections without publishing authored CPU data. Cooked
residency remains a separate manager and invokes no editor recipe.

The request-ID overload of `GetStaticMeshCompilationDiagnostic` retrieves the exact
completion even when the owner has newer work or has been destroyed. An expired
history entry returns request ID zero.
`GetStaticMeshCompilationDiagnostic` and
`GetStaticMeshCompilationManagerDiagnostics` are owner-thread, value-only reads.
They neither pump work nor perform source/cache I/O or initialize resources.
Request ID zero means no available observation, including evicted history.
A nonzero observation describes its captured source identity, render builder version and module
generation, not proof that the live asset still matches it. Match these facts
before presenting it as current. Cache origin and DDC keys are implementation
details and are not exposed through completion diagnostics.
Nonfatal cache failures survive successful publication in a flat `CacheWarnings`
list. Each `FAssetBuildCacheWarning` identifies a cache read/decode/write operation,
with an owned message bounded to 960 bytes and `ToString()` below 1024 bytes.
Physics results use an independent `FPhysicsCookFailure` and the same Asset cache
warning type; the shared scheduler preserves each product's fatal error type.
There are at most two cache warnings per recipe (read or decode, followed by write);
clean cache outcomes create no records. Backend and codec causes are translated
at the cache boundary instead of retained as a nested diagnostic tree.
The optional `Error` retains the original pipeline failure without a completion
wrapper. `FormatStaticMeshCompilationDiagnostic` displays both fatal errors and
retained cache warnings, including when application fails after a successful build.
The presentation text budget
is 4096 bytes per record including a producer identity capped at 256 bytes.
Completion diagnostics retain no cache-origin or per-product observation records. Queue counts and reserved bytes remain for admission and lifecycle
accounting. CPU completion is independent of GPU readiness. Diagnostics own no
source, payload, component or callback.

Initial-edit replacement waits for the retiring worker's storage to be released
before reclaiming that record's admission capacity. Explicit cancellation or a
new accepted request suppresses that replacement. Late cancellation remains
accounted until worker completion even after terminal delivery.
