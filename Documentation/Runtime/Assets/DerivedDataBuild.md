# Derived Data Build Protocol

Summary: Define the local build service, immutable request inputs, canonical actions, structured completion, and optional cache persistence for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-10-15

## Ownership

[TargetPlatform](TargetPlatforms.md) resolves immutable tool capabilities before
family execution. It owns no DDC function, action, session, scheduler or cache.
Families bind only relevant resolved settings to their existing identities;
moving target enums to Core does not change canonical action bytes. Material
translation and Shader compilation remain separate products. Physics capture
retains an explicit payload target; only the existing Win64 collision
representation is supported. Cook preparation consumes these family services
before serialization and keeps package publication outside DDC.

`DerivedDataCache` owns the process-wide production `IBuild`, function registry,
immutable definitions/actions/inputs/outputs, persistent sessions, execution,
and the backend-neutral structured-record cache. The cache boundary accepts and
returns `FCacheRecord`; serialization, compression, integrity validation, and
private byte storage remain below it. The module depends only on Core. The
service adds no worker pool, remote execution, transitive graph, generic
single-flight, or publication policy. Sessions accept an `IBuildScheduler`;
the inline adapter executes on the caller, while the task adapter borrows
the running Core task system. Neither adapter acquires asset memory reservations.

Engine owns the Texture2D, TextureCube, VolumeTexture, StaticMesh render, and
physics collision function implementations. TextureCompressor and MeshBuilder
register their Engine function adapters during module startup; physics registers
on first use. ShaderBuild owns its shader function.
Each family owns typed assembly, business-error translation, scheduling,
reservations, object mutation/freshness checks, and publication. TextureCompressor and MeshBuilder
remain pure recipe modules; ShaderBuild retains compiler scheduling, LRU, and
single-flight.

Texture, StaticMesh, and physics each own a small `FResourceBuildSessions` holder
that lazily acquires sessions from the DDC production build; it adds no build
service or worker pool. TextureCompressor and MeshBuilder close/drain their family
sessions at module shutdown; Engine does the same for physics. Asset compilation
finishes before producer modules unload. Holder close stops acquisition, drains
outside its lock, and retains sessions after a reentrant `WouldBlock` until a later
external drain. Startup reopens a previously drained holder. ShaderBuild retains
its isolated service/session so compiler environment and registration lifetime
remain tied to its module, independently of Engine resource holders.

## Definitions, actions, and identity

`FBuildDefinition` contains a function name, sorted constants, and named opaque
source references. It contains neither live objects nor a cache key.
`FBuildAction` binds the registered descriptor and resolved semantic input
identities. Its canonical schema-2 bytes determine the XXH3-128 cache key.

Definitions, actions, captured inputs, and outputs are created by move-only
builders. `IBuild` exposes all four factories; direct vector-based construction
is private. Builders own and bound their tables, reject duplicate or invalid
names/IDs, canonicalize ordering at freeze, and publish immutable values only
after successful validation. Constants retain typed bool, uint64, float, string,
and XXH3-128 overloads.

The descriptor records function/version, constants schema, output type/schema,
and bucket. Engine resource families use stable, flat bucket names: `Texture2D`,
`TextureCube`, `VolumeTexture`, `StaticMesh`, `StaticMeshCollision`, and `Shader`.
Individual assets, build parameters and versions are represented by action keys.
Renaming a bucket invalidates prior cache lookups; old cache directories are not
automatically migrated or removed. Constants are named bool, uint64, float, string,
or XXH3-128 values.
Input references record a name, semantic content identity, identity
scheme/version, and representation/version. Source paths, object addresses,
cancellation, scheduling, persistence policy, and compression do not enter
identity.

Action encoding uses explicit little-endian widths, length-prefixed strings,
constant tags, and sorted names. Negative float zero is normalized; non-finite
floats, zero versions/hashes, invalid names, duplicates, and excessive tables
are rejected. Action encoding remains schema 2. Texture2D uses function version
3 and output schema 3 for structured mip metadata; other Texture, StaticMesh,
and physics output schemas use version 2. Shader uses version 4 to bind its
request path as well. These transitions intentionally
cold-invalidate old records while authored and Cook payload bytes remain
unchanged.

## Request inputs and resolution

`FBuildInputsBuilder` captures request metadata through
`IBuildInputResolver::Describe` and retains the immutable resolver/captured
generation for possible payload materialization. The resulting request value is
immutable. `FBuildSession::Build` accepts it beside a definition or action.
Alternatively, request options may retain a captured `InputResolver`; its
`Describe` runs inside admitted execution and failures complete through the
normal status-only completion. The resolver must already own immutable captured
state when submitted.

`Describe` may inspect captured metadata only. `Resolve` runs only after a miss
when local build is allowed; it materializes named immutable blocks and verifies
them against the frozen identity. It must never substitute a current live asset
or import file for captured state. A session-level resolver is available for
truly shared inputs, but Engine and ShaderBuild production requests supply their
own captured request resolvers and reuse persistent sessions. Explicit frozen
`FBuildInputs` remain supported and are mutually exclusive with a request resolver.

Before producer invocation, DDC sorts input/value tables, rejects invalid or
duplicate identifiers, verifies exact identities, and enforces metadata, value,
byte, and working-set bounds. `FBuildContext` borrows the action and materialized
inputs and exposes cooperative cancellation. Asset compilation owners retain
memory estimation, admission and reservations; producer context has no request
working-set budget. Cache requests retain independent byte and codec limits.
`IBuildFunction::Build` returns `void`; it reads only through
`FBuildContext` and publishes values, Compact Binary metadata, notes, warnings,
deterministic errors, and transient logs into the context-owned output builder.
Deterministic messages may be cached; transient logs are returned only with the
cold output and prevent cache storage. DDC freezes the output synchronously after
return. Family semantic validation runs while
producing fresh output and again during typed assembly; DDC owns generic schema,
hash, ordering, and byte-bound validation.

Inputs retain names because they describe producer-facing captured blocks.
Outputs use `FValueId`, a stable 12-byte identity derived from a name or hash.
`MakeIndexed` reserves the final 24 bits for indices `0..0x00ffffff`. Production
families derive every output ID from a fixed family base and index; persisted
records contain no debug-name dependency. `FValue` owns raw XXH3-128, raw size,
and optional immutable data. A default value has no data; a constructed empty
payload has data even though its size is zero. `RemoveData` retains hash and size
as a descriptor-only reference. Complete build outputs require resident data for
every value. Value construction computes the hash once; persisted decode
compares that hash with the value descriptor. Output limit checks and record/output
conversions validate structure and budgets without rehashing immutable values.
Cache records own independent immutable schema, value, metadata, and message
tables, retaining shared payload allocations rather than wrapping build outputs.
`RemoveValueData` produces a descriptor-only record without changing its source.
Such a record cannot convert to a complete build output or encode with the
schema-2 codec; full-record cache requests continue to return resident values.
Metadata is keyed by `FValueId` and stored as owned bounded
Compact Binary objects.
Assembly metadata is deterministic and persisted. `AddObservation` records
optional bounded CB measurements in a separate table returned by
`GetObservations`; they do not enter cache records, manifests, action identity,
or warm output assembly. IDs use a separate key space, with at most 128 objects
and 1 MiB of canonical CB bytes by default; output total-byte limits still apply.
Transient diagnostic logs retain their separate non-cacheable contract.
Texture2D metadata exposes `Platform`, `Profile`, stable `Format`, and a `Mips`
array of `Width`, `Height`, `RowPitch`, and raw `Size`. Full and selected mip
consumers validate these fields directly without unpacking a binary wrapper.
Other families keep bounded binary metadata where assembly consumes its whole
typed product.

| Producer | Request input | Identity-specific fields |
| --- | --- | --- |
| Texture2D | Torn-off `FTextureSource`; shared mip blocks materialize after a miss | Usage, resolved sRGB, quality, alpha policy, maximum resolution, target/profile |
| TextureCube | Canonical faces or HDR panorama retained by capture | Layout, sRGB, builder identity, dimension/exposure, target/profile |
| VolumeTexture | Torn-off volume source; voxel view materializes after a miss | Shape, format/filter, source schema, target/profile |
| StaticMesh render | Captured authored bulk and reconciliation metadata | Source/reconciliation identities, slot count, producer/output versions, target |
| Physics collision | Owned positions/indices captured before the request | Mode, query policy, weld settings, producer/output versions, target |
| Shader | Metadata-bound immutable source closure | Portable variant, environment, file contents, macros, ordered entries/stages |

A valid warm hit does not invoke request payload resolution or a producer.
Cold output retains recipe allocations without a required encode/decode round
trip. Typed assembly and publication remain owner-controlled.

## Policy and cache behavior

`FBuildPolicy` controls cache query, local build, store-on-build, force build,
input/output/persistence limits, encoded-byte limits, and maximum working set.
Successful completion always returns its validated output. Compression and
encoding are cache implementation details, not build service or request policy.
Tests may replace the complete structured cache interface for deterministic
miss, hit, corruption, and store behavior.

Cache lookup returns `expected<optional<FCacheRecord>, FCacheError>`.
An empty optional is a normal miss and emits no diagnostic. Backend failure is a
real error and is never labeled corruption. When policy allows local build, a
miss, rejected record, or cache infrastructure failure falls through once to
resolution/build. Before accepting either a positive or deterministic negative
hit, the session compares its output schema name and version with the prepared
action's function descriptor. A mismatch rejects the record and takes the same
fallback path; cache-only requests fail without production or persistence.
The cache validates and decodes persisted bytes before
returning a record. Cache-query failures are logged by DDC before the build falls
back; their internal reason is not returned as completion data.

Persistence converts the validated build output into a keyed record and submits
that record to the cache. The cache encodes, compresses, and stores it. Record or
cache-store failure cannot discard an already validated local output; completion
remains successful and DDC logs the store failure. Fresh invalid producer output
and outputs containing transient logs never reach persistence.

The [cache storage contract](DerivedDataCacheStorage.md) owns the canonical CB
manifest, versioned attachment identities, full/metadata/selected queries,
byte/codec budgets, integrity validation, and retain-only atomic publication.
Build sessions retain complete-output validation and local-result preservation.

An output containing an error message is a deterministic negative result: its
values are discarded, it completes with `EStatus::Error`, and it is persisted
and reused under the same action key. A warm negative hit does not resolve
inputs or invoke the producer. There is no time expiry; key or version changes
invalidate it. Infrastructure failures and cancellation have no output and are
never cached.

## Completion and failure

Accepted work completes exactly once with `FBuildCompleteParams`:

- `EStatus::Ok` owns one validated output without error messages.
- `EStatus::Error` owns either a deterministic error output, or no output for an
  infrastructure failure.
- `EStatus::Canceled` owns no output.

There is no public typed producer failure and no validation receipt. Families
decide only from status, output presence, and bounded deterministic output
messages. Message and log text is display data, never control-flow identity. Typed
assemblers reparse and revalidate immutable output at their trust boundary.

The completion independently exposes its cache key and two `EBuildExecutionFlags` facts
through `GetExecutionFlags`: cache hit and local build. These flags record the
execution path; `EStatus` independently records success, failure or cancellation. Its fields contain no query/store bookkeeping,
execution report, cache error, metric, timing, or internal phase identifier.
Optional observations live only in the returned cold output.

Deterministic producer errors are bounded output messages. Non-deterministic
producer logs have their own category, severity, and text and make the output
non-cacheable. `FailExecution` reports resource, environment or unclassified
recipe failures without an output or persistence; `GetDiagnostic` retains bounded
completion-only error text for callers. Only input-determined failures
use `AddError`; budget and module availability never become negative cache entries.
Adapter versions are advanced to invalidate previously misclassified failures. Cache and input infrastructure failures are logged by their owning
layer and are not translated into request-level phase or cause objects.
Cancellation is a distinct terminal status. Request options carry the Core
`FCancellationToken` (`FBuildCancellation` is its DDC alias). Copies share a
monotonic flag, including observers of request-owner cancellation. Cancelling a
request, or completing it as `Canceled`, latches the caller token before notification.
The token owns no completion event: callback retirement and request waiting remain
separate lifecycle guarantees. Observer callbacks are serialized, must not reenter
their token, and must retain valid captures for every token copy.

Admission rejection uses `FBuildAdmissionError` and invokes no completion.
Closed service/session, capacity, missing function, invalid request, dispatch
rejection, and internal admission failure are distinct reasons.

## Persistent sessions and lifecycle

`IBuild` owns registration. Each session captures an immutable registry snapshot
when it is created; later registration affects later sessions without mutating
existing sessions. Registered functions and required services remain owned
through drain.
`FBuildSession::Build` accepts a definition or action, optional request inputs,
policy, cancellation, and completion callback. An omitted scheduler or
`CreateInlineBuildScheduler` executes inline. `CreateTaskBuildScheduler` schedules
on Core workers in scheduler lifetime. Its wait handle uses
Core cooperative waiting, including single-worker nested builds. A stopped
Core scheduler rejects admission without completion; the adapter does not start
or stop Core. Asset producers explicitly nest their operation completion when parent completion must include it.

All sessions use one Prepare, CacheGet, BuildLocal, CachePut, Complete pipeline.
Action preparation runs once; local execution receives the prepared action and
owns no cache policy. Skipping the query runs local production on the initial
producer dispatch without submitting a redundant task. Cold reads resume local
production on the selected producer scheduler; warm hits skip it.

`CreateSession` accepts a producer scheduler and explicit `EBuildCacheExecution`.
Both default to inline execution. Task consumers select `CreateTaskBuildScheduler`
and `EBuildCacheExecution::Task`; all task-cache sessions of an `IBuild` share one
service-owned dispatcher using Core BlockingIO. Scheduler injection belongs to
`CreateAsyncCache`; services accept that configured `IAsyncCache` as one explicit
dependency instead of exposing a second cache-scheduler override. There is no
running-Core mode selection.
Inline sessions share a separate service-owned inline dispatcher over the same
backend. Its item/byte admission is independent of the task queue; synchronous
callers never wait on task saturation or help execute sibling asset work. Inline
dispatch does not apply the task concurrency limit and permits synchronous nested
submissions. It retains the same bounded item/byte admission through retirement.
Injected `IAsyncCache` takes precedence over either cache mode and remains
caller-configured. Session Close cancels only its own requests; service Close/Drain
also closes/drains its owned dispatchers, without globally closing an injected cache.

`IAsyncCache` admits Get/Put batches atomically. Rejected batches invoke no
callbacks; every accepted item delivers exactly one indexed result, including
backend exceptions, scheduling rejection, cancellation, and dropped work.
Completion order need not match input order. Ordinary inline batches advance
iteratively from their dispatch pump rather than recursively pumping from each
completion. Explicit nested inline submissions still complete synchronously before
returning to the outer callback.
Selection IDs are copied and records retain immutable storage; async Get ignores
borrowed Stats pointers and returns statistics by value in its callback.
Captured cancellation and callback state must
remain valid until the request owner drains. Callers stop submitting before owner
destruction. Build and Cache accept the same `IRequestOwner`. An owner retains mutable priority, cancels current/future work,
and waits through callbacks and request-buffer/callable release. Same-owner and
pending same-cache waits anywhere in a nested completion stack return `WouldBlock`;
reentrant destruction still cancels and leaves shared accounting alive.

`FRequestBarrier` registers a scoped request with the existing owner protocol.
It keeps `Poll` and external waits pending until submitting code publishes its
handles, including nested inline submissions. Cancellation cannot release the
barrier; its submitting-thread scope must end. Waiting from that scope returns
`WouldBlock`. Priority remains mutable and is sampled by newly dispatched work.

Standalone `CreateAsyncCache` without a scheduler uses Core BlockingIO, creates no threads, and admits
at most 4,096 pending/active items with two active operations.
`FCacheDispatchLimits` configures concurrency (clamped to 1..64), capacity
(clamped to 1..4096), and queued data bytes (512 MiB by default). Active slots cover I/O,
compression/decompression, and completion callbacks. Priority selection has FIFO
ties; after 32 dispatch turns of waiting, the oldest pending item wins. This
fairness policy is internal, using admission/dispatch turn differences rather
than updating every queued item. An admitted pending item is dispatched within
32 plus capacity further dispatch turns,
provided active work retires. Mutable owner priority is sampled at dispatch.
Atomic item/byte admission rejection returns `ECacheError::Capacity`, distinct
from backend `StorageFailure`, without callbacks or owner accounting.

Queued-data charges cover copied selection bytes, schema/message lengths, and
distinct payload/metadata backing capacities within each Put record. Fixed
request overhead uses item admission; record tables use existing record limits.
A selection-free Get therefore needs no queued-data bytes even when this budget
is zero. A slice charges its entire retained allocation;
shared allocations across separate requests are charged conservatively per request.
Charges remain through callbacks and dispatch publication and are released on request
retirement. Request objects, record tables, opaque callable captures,
allocator/container spare capacity, backend result data, and active codec workspace
are excluded; this is a coarse queued-data budget, not an RSS bound.
Each request retains its existing encoded/raw/workspace budgets. This controls
operation concurrency and retained admission, not a new global asset memory
reservation. `Close` stops admission and cancels accepted items; `Drain` waits
for them without closing admission itself. Adapter destruction closes and drains;
drain from its completion stack returns `WouldBlock`. Cooperative cancellation
cannot interrupt an already running filesystem or Zstd call. Put checks between
values and before manifest publication; canceled replacements preserve the old
manifest and may leave retain-only orphan blobs. A final atomic write already
started may finish after cancellation.

Content reads with the same cache root, bucket/hash, and encoded bound share an
in-flight immutable blob allocation. The read is forgotten immediately after
completion, including failures; this is not a resident hot cache. Each consumer
still validates the blob and inflates under its own budget. Query read-byte
statistics charge physical reads to the leader; followers charge zero blob-read
bytes while still counting logical value resolution/decoded bytes and their
conservative retained workspace.

Scheduled builds prepare action identity, submit a full async Get, and release
the Worker task. A usable full hit completes directly; misses/rejections resume
resolution and production on the captured producer scheduler. Metadata description
runs once, full output conversion remains mandatory, and priority/budget are
preserved across continuations. A valid local output is retained through async
Put; write admission, serialization, dispatch, backend failure, or a dropped write
cannot discard it. Request cancellation still clears the final output. Wait handles
follow each stage and use Core helping; close/drain spans every stage and callback.
Callbacks have no fixed thread affinity. Infrastructure failures after action
preparation retain the known key; deterministic messages and cache diagnostics
preserve their existing format. Request inputs/resolvers are released after local
production, before persistence completion.

A scheduler rejects without invoking or retaining its thunk, or accepts it for
at-most-once execution. Dropping accepted work completes it as canceled. Request
handles can cancel pending/running work. `FBuildRequest::Wait` waits for callback
completion, request resource release, and dispatch return without closing the
session. While execution is pending it first uses the scheduler wait handle,
allowing Core worker helping. Unsupported task-thread waits return `WouldBlock`;
an empty request handle returns `InvalidRequest`. Schedulers without wait handles
require external progress. Completed or canceled
requests do not wait for retained stale thunks, which own no producer/resolver
resources. Callbacks execute without session locks;
callback exceptions cannot break accounting.

`IRequest` defines cancellation and waiting. `IRequestOwner` tracks Build and
Cache requests through paired `Begin`/`End`, and owns task dispatch through
`LaunchTask`. `Begin` returns a shared state lease; registration and owner-field
initialization hold the request lock. `FRequestOwner` supplies cancellation,
`Wait`, `Poll`, and mutable priority. Destruction cancels and joins all requests.
Destroying an owner from its callback is rejected: move that
destruction outside the callback and drain first. Reentrant waits return
`WouldBlock`. Callbacks use nonblocking `RequestCancel`; synchronous `Cancel`
rejects self-waits and returns only after request end.

Result readiness means a terminal result is available for consumers. Request end
means submission has returned, callbacks have exited, buffers/callables have been
released, and registered native work has ended. Build and Cache share this
protocol. Owner `Wait` joins request end; `Poll` observes the same boundary.
`Cancel` signals cancellation and waits for request end; owner cancellation signals all
requests before joining any of them. Internal propagation and session/cache
`Close` use nonblocking `RequestCancel`. Queued cache requests retire immediately,
including their callback, without waiting for another owner's I/O slot. Already
canceled owners do not dispatch cache work; stale dispatch thunks do no backend
work. Owner wait joins requests already admitted; concurrent submission windows
must be protected by submission barriers before publishing handles.
`TrackTask` includes accepted native handles in this boundary; cancellation stays
with the task's source. `TrackDependency` includes external handles such as Shader
subscribers, including cancellation for dependencies registered after cancel.
Dependency queries and waits execute outside owner locks. A wait rechecks the
owner after joining its snapshot, including work added by a continuation.

The complete Build request, its Get/Put stages, and each dispatched task register
with the same owner. Dispatch tasks own the queued/running transition and dropped
callable handling. Canceling queued producer continuations retires their callback
without requiring the producer queue to advance; stale thunks perform no work.
Cache batch handles wait only for their own operations, avoiding a wait on the
enclosing build. Requests without an explicit owner retain private owner state
until retirement. Queued cache selection and subsequent stages read the owner's
latest priority; dispatched Core tasks retain their launch priority. There is no
per-request priority mirror or scheduler working-set budget. Memory reservations
and cache raw/encoded/resident size limits remain independent protections.
Sessions keep their own close/drain authority.

DDC admission does not capture a Core scope or allocate per-request scope anchors.
Engine asset producers use `FRequestOwner` directly. Producer task completion
establishes that no further submissions can occur; owner `Poll()` then confirms
request end and completion of registered native dispatch handles.
`Wait()` joins the same boundary, including native dispatch capture destruction.
Dispatch publication keeps a request registered until its scheduler has returned
the native handle, even if its callback completed synchronously. Owner polling
alone cannot close a producer that may still submit another inline request.
Queued requests need no Core worker admission slot, and callback-generated stages
remain admissible while requests are active. Owners allocate no completion event;
undispatched asset records therefore do not retain Core shutdown. Requests retain
internal owner state rather than the facade; facade destruction cancels and joins
its work. Asset managers drain producers and their owners before Core shutdown.

Task completion does not forward cancellation. Render and texture managers signal
their request owners; physics owner abort withdraws only unstarted cooks and lets
started work retire normally. Batch cancellation affects only
that batch, including when several producers use one owner. LaunchTask reads
current owner priority and uses the process scheduler lifetime.
Schedulers and DDC callbacks must progress without GameThread pumping. Native
schedulers expose their dispatch handle through `IBuildScheduledWork::GetTaskHandle`;
custom external schedulers retain responsibility for their own dispatch lifetime.
Managers drain DDC before Core scheduler shutdown.

Inline cache dispatch permits callback reentry and does not use the asynchronous
queue's concurrency gate. Nested inline builds complete before the enclosing
callback returns. Asynchronous dispatch retains bounded concurrency and queue
capacity, and batch handles continue to wait only for their own operations.

`Close` stops admission and cancels accepted work. `Drain` waits for execution,
callbacks, dispatch return, and callable-owner release. Calling drain from the
same session execution/completion stack returns `WouldBlock`. Completed handles
and stale thunks retain no producer/resolver owners.

Texture2D and TextureCube compilation owners submit through a persistent task-scheduled session.
They release their dispatch worker immediately and retain a build request owner
until callback retirement, then apply its detached result on GameThread. Owner
priority follows the request; concurrency and estimated memory stay reserved
across asynchronous stages. See [Asset Compilation](AssetCompilation.md#initial-compiling-managers).
Material translation and generated Shader submission also own persistent task
sessions. Material normalization/generation, Shader preparation and CPU assembly
are separate detached continuations; neither producer waits for nested cache I/O.
Other Engine asset families and synchronous texture adapters use inline sessions.
ShaderBuild retains a separate inline session for explicit synchronous adapters. Each synchronous caller owns a
request group and waits explicitly before typed assembly; it never assumes
that completion happened before `Build` returned. Captured input description
runs inside the accepted request. Producer scheduling adds no second asset reservation. Cache working-set limits
remain independent of producer execution and never affect action identity. These synchronous wrappers
retain their existing execution contract: scheduling continuations back onto an
already occupied asset worker can nest unrelated asset tasks and violate their
mailbox/barrier protocol. Task-scheduled build sessions release producer workers
while cache work runs on BlockingIO. Further family migrations require the same
asynchronous bridge at their asset-compilation owner boundary.
Engine family wrappers share inline submission through their holder and preserve
`FBuildAdmissionError` until business-error translation. Texture task submission
also preserves typed rejection instead of returning bool. Native fixture guards
scope task-cache overrides through owner retirement; replacement services register
their functions before publication and old sessions/services drain outside the
acquisition lock. Test overrides do not replace synchronous production sessions.

## Family boundaries

Cook registration versions describe family behavior, not DDC recipes. Unified
package preparation and source-only filtering preserve existing family action
projections, payload codecs and producer versions. The package publisher owns
CookState reuse identity separately; its legacy producer-version word is fixed
to 1. Platform resolution supplies capabilities while the family boundary
validates its request profile.

Texture2D, TextureCube, and VolumeTexture use a texture-owned inline session for
import, detached build and Cook. Texture2D owner work (including authored PostLoad
and editor rebuilds) and Cube platform-cache owner work use a separate task-scheduled
session. Noncanonical Cube inputs normalize before submission on the dispatch worker.
Completion supplies the cache
key and hit fact. Recipe timing stays inside producer-local qualification paths;
it is not part of DDC output or Engine request diagnostics. Their
shared-output schemas use fixed indexed value IDs and Compact Binary
metadata; package/Cook serialization is unchanged.

TextureCompressor family entrypoints validate recipe values and select output formats.
Internally, `TextureMipBuilder` generates and compresses the shared 2D/LDR Cube
mip chain, `TextureCubeBuilder` owns panorama projection and HDR Cube filtering,
and `VolumeTextureMipGenerator` generates uncompressed three-axis mips. The
family entrypoint files are `Texture2DBuildOperations`, `TextureCubeBuildOperations`,
and `VolumeTextureBuildOperations`; `TextureBCEncoder` owns BC block encoding.
Naming follows the [UE texture compressor interface](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/TextureCompressor/ITextureCompressorModule):
`BuildTextureMips` includes mip generation and compression, `GenerateMipChain`
generates uncompressed Volume mips, and `CompressTextureMip` encodes one BC mip.
`ITextureCompressorModule::GetBuildVersion(ETextureBuildKind)` returns an opaque
nonzero `uint64` identity for each of Texture2D, TextureCube, and VolumeTexture.
Identities are immutable for the editor lifetime and compared only for equality.
Private recipe revisions and stable implementation identifiers are canonically
encoded and hashed with XXH3-64. Texture2D and Cube additionally include shared
color/mip revisions, GLM version, and the managed bc7enc/rgbcx source revision;
Cube includes its private projection revision. Volume remains independent of
BC encoder revisions. Future output-affecting dependencies belong in the relevant
composition, not in Engine's public payload contract.

Engine records the identity as a `BuilderVersion` action constant and checks it
against the active module before invoking any recipe. Each Engine adapter has
an independent function version (3 for 2D, 2 for Cube/Volume); constants schemas
are 2 for 2D, 3 for Cube, and 2 for Volume. Shared-output schemas are 3 for 2D
and 2 for Cube/Volume. Cook recipe
dependencies include the builder identity, adapter function/constants versions,
shared-output schema, and payload schema. Algorithms and adapters independently
invalidate DDC/Cook production identity. TXPL schema 3 removes the producer word. Older payload schemas require recook;
algorithm identity does not participate in payload admission.
PhysicsCore similarly owns `FCollisionCookedData::GetBuildVersion()` for the linked
collision cooker. Engine's collision adapter uses its own function version 2,
constants schema 2 and shared-output schema 2. A nonzero 64-bit `BuilderVersion`
constant carries the cooker identity and is checked before recipe decoding.
DCOL compatibility uses payload schema 3, independent of algorithm identity;
the producer word is absent and older payload schemas require recook. StaticMesh
Cook dependencies include the cooker identity, adapter schemas and DCOL schema.

HDR Cube and Volume builders return complete products through `std::expected`
with diagnostic strings, so failed candidates remain local. Cube normalization
retains structured input errors. Family entrypoints share one target-support
predicate, while panorama limits and decoded image types come directly from
Engine's recipe contracts. The BC encoder header depends on output data and
forward declarations rather than the mip-builder interface.
Texture2D and LDR Cube share a resolved mip request and private BC encoding;
Cube selects one format for all six faces. A single RGBA8 source mip generates
the complete chain, while supplied chains remain intact. Shared mip recipes
return detached platform data and metrics only on success; their result and timing
types stay private to TextureCompressor. Compression tasks drain before borrowed
source storage is released. Texture2D, TextureCube, and VolumeTexture module
entrypoints return optional platform data by value and log failures inside
TextureCompressor; they accept no cancellation control. The external build system owns
cancellation and discards completed products for canceled requests. HDR Cube and Volume retain their
distinct filtering recipes.
Cube recipe and canonical pixel inputs distinguish LDR faces from HDR panorama
alternatives; HDR output mode, linear color policy, and panorama layout follow
the alternative rather than optional flags. Authoring metadata remains available
for source preservation and application without changing cached representations.

StaticMesh render and physics collision each use a family-owned session with
separate definitions/actions and request inputs. StaticMesh render owners use a
persistent task-backed session; synchronous Build, detached import and Cook retain
an inline session. Both share input validation, request limits and typed output
consumption. Owner callbacks retain detached build completions, and the GameThread
pump dispatches CPU assembly and ray preparation after build-owner retirement.
The captured builder version defines the asynchronous request. Physics keeps its
independent synchronous session and collision worker path. Source/reconciliation
identity, working-set admission, generation checks, typed construction, collision editor
gating, and publication remain unchanged. Serialized and native physics
validation protect different trust boundaries and remain separate.

ShaderBuild captures immutable dependency metadata into request inputs, reuses
persistent inline and task service sessions, and keeps its compiler single-flight, LRU,
waiter cancellation, filesystem bounds, and dependency-content verification.
Its nonzero opaque 64-bit build identity composes private producer revisions and
compiler dependencies. A named `BuilderVersion` constant is checked before recipe
decoding; adapter version 4, constants schema 2 and output schema 6 are separate.
The output binds fixed entry IDs and Compact Binary metadata to the virtual shader
path. Cook production identity includes those versions, the library producer revision
and DSHD/DSLB schemas. Both runtime codecs use schema 5 with no producer
version word, allowing algorithm upgrades without rejecting compatible cooked data. Shader counters derive from completion status and build-status facts.

Engine registers `Material.Translate` before family session acquisition. Its versioned
canonical input and one bounded `Product` output persist normalization/generation,
active schemas/layout, requirements and typed semantic diagnostics, independently
of Shader target/compiler/source identities. Locations use input-relative ordinals;
requesting snapshots restore consumer GUIDs, paths and collection defaults. Typed
validation rejects schema, identity, MIR/layout or requirements disagreement and
rebuilds a bad hit once without cache query. Force, cancellation and persistence
failure retain the common session policy. Material operations retain their DDC
owners through retirement and close family sessions after operation drain. Complete
assembled material candidates remain in the shared bounded process cache only.

Cache success never means an asset is current, applied, GPU-ready, or
physics-ready. Family freshness and object application checks remain authoritative;
textures serialize mutation against their active request, while other families
retain their own generation rules. Game uses cooked values and runtime codecs without authoring build
services. See [Asset Data Lifecycle](AssetDataLifecycle.md),
[Asset Compilation](AssetCompilation.md), [Static Mesh Building](StaticMeshBuilding.md),
and [Shader Cache](../Rendering/ShaderCache.md).
