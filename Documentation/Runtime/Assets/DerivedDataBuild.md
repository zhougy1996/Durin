# Derived Data Build Protocol

Summary: Define the local build service, immutable request inputs, canonical actions, structured completion, and optional cache persistence for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-10-11

## Ownership

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
reservations, latest-wins checks, and publication. TextureCompressor and MeshBuilder
remain pure recipe modules; ShaderBuild retains compiler scheduling, LRU, and
single-flight.

Texture, StaticMesh, and physics each lazily create a family-owned session from
the DDC production build. There is no Engine integration service or explicit
asset-build lifecycle. ShaderBuild creates an isolated service/session for its
module lifetime. Asset compilation is drained before producer modules unload.

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
2 and output schema 3 for structured mip metadata; other Texture, StaticMesh,
and physics output schemas use version 2. Shader uses version 3 to bind its
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
inputs and exposes cooperative cancellation and the working-set limit.
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
resolution/build. The cache validates and decodes persisted bytes before
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

The completion independently exposes its cache key and two `EBuildStatus` facts:
cache hit and local build. Its fields contain no query/store bookkeeping,
execution report, cache error, metric, timing, or internal phase identifier.
Optional observations live only in the returned cold output.

Deterministic producer errors are bounded output messages. Non-deterministic
producer logs have their own category, severity, and text and make the output
non-cacheable. Cache and input infrastructure failures are logged by their owning
layer and are not translated into request-level phase or cause objects.
Cancellation is a distinct terminal status.

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
on Core workers and inherits the submitting task scope. Its wait handle uses
Core cooperative waiting, including single-worker nested builds. A stopped
Core scheduler rejects admission without completion; the adapter does not start
or stop Core. Core scope closure remains a caller lifetime responsibility.

Schedulers opt into automatic
cache continuations with `SupportsContinuations`; custom schedulers may inject an
`IAsyncCache` through service options. Task sessions share one default
async cache per build service without changing existing or later inline sessions.
Inline sessions keep synchronous cache calls unless the service explicitly
injects an async adapter.

`IAsyncCache` admits Get/Put batches atomically. Rejected batches invoke no
callbacks; every accepted item delivers exactly one indexed result, including
backend exceptions, scheduling rejection, cancellation, and dropped work.
Completion order need not match input order. Selection IDs are copied and records
retain immutable storage; async Get ignores borrowed Stats pointers and returns
statistics by value in its callback. Captured cancellation and callback state must
remain valid until the request owner drains. Callers stop submitting before owner
destruction. An owner retains immutable priority, cancels current/future work,
and waits through callbacks and request-buffer/callable release. Same-owner and
pending same-cache waits anywhere in a nested completion stack return `WouldBlock`;
reentrant destruction still cancels and leaves shared accounting alive.

The default async adapter uses Core BlockingIO, creates no threads, and admits
at most 4,096 pending/active items with two active operations. Configurable
concurrency is clamped to 1..64 and capacity to 1..4096. Active slots cover I/O,
compression/decompression, and completion callbacks; queued owners are selected
by priority with FIFO ties. Each request retains its existing encoded/raw/workspace
budgets. This controls operation concurrency, not a new global asset memory
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

`FBuildRequestOwner` groups requests across sessions. Cancel affects existing
and future requests; destruction cancels and waits. Owner priority is immutable
and inherited at submission; the task adapter maps it to Core launch priority.
Schedule parameters expose the function name and existing working-set budget
without transferring reservation authority. Schedulers must copy the function
name if retaining it after `Schedule` returns. Owner wait includes request
callbacks, request resource release, dispatch return, and open submission
barriers. `FBuildRequestBarrier` keeps an owner non-idle while a submitting
thread may add requests. Wait from the same request execution/completion stack,
the same owner stack, or a thread that created an open owner barrier returns
`WouldBlock`. Reentrant owner destruction still cancels; shared accounting
survives until the active stack finishes. A completed owner can be reused until
canceled. Single requests may be submitted without an owner and waited directly.

`Close` stops admission and cancels accepted work. `Drain` waits for execution,
callbacks, dispatch return, and callable-owner release. Calling drain from the
same session execution/completion stack returns `WouldBlock`. Completed handles
and stale thunks retain no producer/resolver owners.

Engine asset families submit directly through their own inline sessions.
ShaderBuild does the same through its isolated service. Each synchronous caller owns a
request group and waits explicitly before typed assembly; it never assumes
that completion happened before `Build` returned. Captured input description
runs inside the accepted request. Producer scheduling carries the already-admitted
owner budget and adds no second asset reservation. These synchronous wrappers
retain their existing execution contract: scheduling continuations back onto an
already occupied asset worker can nest unrelated asset tasks and violate their
mailbox/barrier protocol. Task-scheduled build sessions release producer workers
while cache work runs on BlockingIO; migrating asset wrappers requires an
asynchronous bridge at the asset-compilation owner boundary.

## Family boundaries

Texture2D, TextureCube, and VolumeTexture use one texture-owned session for import,
detached build, manager work, PostLoad, and Cook. Completion supplies the cache
key and hit fact. Recipe timing stays inside producer-local qualification paths;
it is not part of DDC output or Engine request diagnostics. Their
version-2 shared-output schemas use fixed indexed value IDs and Compact Binary
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
an independent function version (2 for 2D, 1 for Cube/Volume); constants schemas
are 2 for 2D, 3 for Cube, and 2 for Volume. Shared-output schemas are 3 for 2D
and 2 for Cube/Volume. Cook recipe
dependencies include the builder identity, adapter function/constants versions,
shared-output schema, and payload schema. Algorithms and adapters independently
invalidate DDC/Cook production identity. TXPL schema 3 removes the producer word. Older payload schemas require recook;
algorithm identity does not participate in payload admission.
PhysicsCore similarly owns `FCollisionCookedData::GetBuildVersion()` for the linked
collision cooker. Engine's collision adapter uses its own function version 1,
constants schema 2 and shared-output schema 2. A nonzero 64-bit `BuilderVersion`
constant carries the cooker identity and is checked before recipe decoding.
DCOL compatibility uses payload schema 3, independent of algorithm identity;
the producer word is absent and older payload schemas require recook. StaticMesh
Cook dependencies include the cooker identity, adapter schemas and DCOL schema.

HDR Cube and Volume builders return complete products through `std::expected`,
so failed candidates remain local. Family entrypoints share one target-support
predicate, while panorama limits and decoded image types come directly from
Engine's recipe contracts. The BC encoder header depends on output data and
forward declarations rather than the mip-builder interface.
Texture2D and LDR Cube share a resolved mip request and private BC encoding;
Cube selects one format for all six faces. A single RGBA8 source mip generates
the complete chain, while supplied chains remain intact. Shared mip recipes
return detached platform data and metrics only on success, and compression tasks
drain before borrowed source storage is released. Texture2D, TextureCube, and
VolumeTexture return optional complete products by value and log failures inside
TextureCompressor; they accept no cancellation control. The external build system owns
cancellation and discards completed products for canceled requests. HDR Cube and Volume retain their
distinct filtering recipes.
Cube recipe and canonical pixel inputs distinguish LDR faces from HDR panorama
alternatives; HDR output mode, linear color policy, and panorama layout follow
the alternative rather than optional flags. Authoring metadata remains available
for source preservation and application without changing cached representations.

StaticMesh render and physics collision each use a family-owned session with
separate definitions/actions and request inputs. Source/reconciliation identity,
working-set admission, generation checks, typed construction, collision editor
gating, and publication remain unchanged. Serialized and native physics
validation protect different trust boundaries and remain separate.

ShaderBuild captures immutable dependency metadata into request inputs, reuses
one persistent service session, and keeps its compiler single-flight, LRU,
waiter cancellation, filesystem bounds, and dependency-content verification.
Its nonzero opaque 64-bit build identity composes private producer revisions and
compiler dependencies. A named `BuilderVersion` constant is checked before recipe
decoding; adapter version 3, constants schema 2 and output schema 6 are separate.
The output binds fixed entry IDs and Compact Binary metadata to the virtual shader
path. Cook production identity includes those versions, the library producer revision
and DSHD/DSLB schemas. Both runtime codecs use schema 5 with no producer
version word, allowing algorithm upgrades without rejecting compatible cooked data. Shader counters derive from completion status and build-status facts.

Cache success never means an asset is current, applied, GPU-ready, or
physics-ready. Family generation/latest-wins and object application checks remain
authoritative. Game uses cooked values and runtime codecs without authoring build
services. See [Asset Data Lifecycle](AssetDataLifecycle.md),
[Asset Compilation](AssetCompilation.md), [Static Mesh Building](StaticMeshBuilding.md),
and [Shader Cache](../Rendering/ShaderCache.md).
