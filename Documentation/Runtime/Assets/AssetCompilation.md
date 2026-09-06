# Asset Compilation

Summary: Define the Engine-owned object-aware compilation aggregate, class routing, and compiler lifetime contract.

Modules: Engine, Launch, TextureBuild, StaticMeshBuild

Last reviewed: 2026-09-07

`FAssetCompilingManager` is the one process authority for asynchronous asset
compilation. Launch starts it after Core task scheduling and pumps it once per
normal GameThread frame. The aggregate owns compiler registration, reflected-class
routing, bounded processing, aggregate progress, successful
post-compile notification, and shutdown placement. It does not impose one
typeless compiler payload, DDC key, queue, or result-application policy;
each Engine-owned typed manager retains its values and invariants.

The built-in compilers are `Durin.Material`, routed from `DMaterial`, and
`Durin.Texture`, routed from `DTexture2D`, and `Durin.StaticMesh`, routed from
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
one synchronous value-only call. Only Texture2D places that call on an
Engine-owned worker; VolumeTexture and TextureCube remain synchronous.
Provider owner retirement closes
new admission and waits for calls already inside the feature gate; no provider
callback, task, deleter, or result lifetime escapes the call.

## Initial Compiling Managers

Material compilation remains Engine-owned. It preserves program-identity
single-flight sharing, retained program results, last-known-good visibility,
generation-safe admission, Renderer publication, reload behavior, and cooked
program rules. Remaining count is the number of live outstanding material
consumers rather than shared worker flights.

Texture compilation is Engine-owned. One `FTextureCompilingManager` owns
typed asset state, worker admission, priority fairness, memory budget,
cancellation, the completion mailbox, latest-wins request serials, GameThread
completion application, and exactly-once completion callbacks. Active records are keyed by
`FObjectHandle`; the manager owns request serials, active/last request ids,
failure state, and bounded terminal diagnostics. Active work ends at terminal
delivery, while completed asset diagnostics remain under a separate bound.
The deterministic input/provider identity remains separate from request serials
and GPU resource readiness.

Recipe providers, DDC ownership, and typed build application are defined by
[Asset Data Lifecycle](AssetDataLifecycle.md#serialization-and-production-ownership).
TextureCube and VolumeTexture stay synchronous and do not register class routes.

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
| Object identity | Carry an `FObjectHandle`; never use an asset path as live-object identity. |
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

`SubmitStaticMeshCompilation` accepts canonical source values and returns before
recipe work. Rejection does not supersede earlier work or call completion.
Accepted requests deliver one `Succeeded`, `Failed`, `Cancelled`, or `Superseded`
terminal result on GameThread. Worker captures contain no object bindings.
Owner records use generation-safe handles and recheck source, normalization,
ordered material bindings, body parameters/revision, provenance identity and
provider registration before applying the sealed candidate. Provider replacement
cannot reuse an older request even when its builder versions are unchanged.

The manager allows two workers, 32 outstanding records, 1 GiB total reservation
and at most 512 MiB per request. Admission uses checked arithmetic for
`1 MiB + 64 * canonical bytes + 1024 * mesh count + 32768 * slot count`.
Detached decoding/recipes/finalization check conservative working-set envelopes
before expansion; cache reads are bounded by the reservation and complete
candidate capacities are checked before mailbox publication. Recipe providers
must honor the borrowed working-set limit before allocating their products.
Cancellation delivery does not release a still-running task's record or bytes.
History retains at most 128 value-only diagnostics with 4096-byte messages.

Background and interactive queues are FIFO, with at most four interactive
dispatches before an eligible background request. Each aggregate pump admits
at most two StaticMesh terminals within a 2 ms soft deadline. `PumpIdentity`
shares this budget across aggregate quota-reclamation passes. Once application
starts, resource preparation and the atomic consumer refresh may exceed that
soft deadline. Explicit selected finish drains only matching records.

Accepted owner edits invalidate publication immediately; reflected changes are
also detected at application. Initial pending edits and stale reflected facts
requeue valid current input. Destruction cancels before resource release; queue
ownership does not prevent package GC. Stop-admission cancels accepted work;
shutdown drains worker scope and callbacks before releasing them. A drained typed
StaticMesh manager can restart; the process aggregate's terminal shutdown
contract is unchanged. Cooked residency remains a separate manager.

Authored `PostLoad` validates metadata and schedules background work without
acquiring canonical geometry. Repeated identical current requests join;
`BuildStaticMeshSynchronously` submits or joins through the same manager and
finishes only that mesh. Missing admission/provider capacity is an explicit
failure, with no inline recipe fallback. Interactive reimport prepares physical
input synchronously, then submits at interactive priority. Source, render,
collision, material bindings and prevalidated provenance become current within
one consumer-refresh boundary. No recipe or metadata validation runs after its
first live mutation. Cook finishes a pending source mutation only when needed,
then builds a detached target projection without publishing authored CPU data.

`GetStaticMeshCompilationDiagnostic` and
`GetStaticMeshCompilationManagerDiagnostics` are owner-thread, value-only reads.
They neither pump work nor perform source/cache I/O or initialize resources.
Request ID zero means no available observation, including evicted history.
A nonzero observation describes its captured source identity and provider
registration, not proof that the live asset still matches it. Match these facts
before presenting it as current. `Render` and `Collision` are optional completed
product observations with opaque DDC key, hit/rebuilt origin, payload bytes and
cache read/write durations; absent values mean unavailable, never a cache miss.
Persistence diagnostics survive successful publication. The retained text budget
is 4096 bytes per record including a producer identity capped at 256 bytes.
`CaptureNanoseconds`, `WorkerNanoseconds` and `PublicationNanoseconds` separate
owner capture, detached construction and owner application. Zero denotes an
unmeasured/not-reached phase. Publication includes provenance preparation,
resource preparation and consumer refresh; CPU completion is independent of GPU
readiness. Diagnostics own no source, payload, component or callback.

Initial-edit replacement waits for the retiring worker's storage to be released
before reclaiming that record's admission capacity. Explicit cancellation or a
new accepted request suppresses that replacement. Late cancellation remains
accounted until worker completion even after terminal delivery.
