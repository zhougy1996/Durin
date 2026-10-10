# Derived Data Build Protocol

Summary: Define the local build service, immutable request inputs, canonical actions, structured completion, and optional cache persistence for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-10-10

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
physics collision function implementations. TextureBuild and MeshBuilder
register their Engine function adapters during module startup; physics registers
on first use. ShaderBuild owns its shader function.
Each family owns typed assembly, business-error translation, scheduling,
reservations, latest-wins checks, and publication. TextureBuild and MeshBuilder
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
are rejected. The migration does not change action schema 2 or family function
versions. Texture, StaticMesh, and physics output schemas move from version 1
to version 2 because value IDs and metadata encoding change. Shader moves to
version 3 to bind its request path as well. These transitions intentionally
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
and immutable data. Value construction computes the hash once; persisted decode
compares that hash with the value descriptor. Output limit checks and record/output
conversions validate structure and budgets without rehashing immutable values.
Metadata is keyed by `FValueId` and stored as owned bounded
Compact Binary objects.

| Producer | Request input | Identity-specific fields |
| --- | --- | --- |
| Texture2D | Torn-off `FTextureSource`; shared mip blocks materialize after a miss | Usage, resolved sRGB, quality, alpha policy, maximum resolution, target/profile |
| TextureCube | Canonical faces or HDR panorama retained by capture | Layout, sRGB, projection version, dimension/exposure, target/profile |
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

Record envelope schema 2 is the only reader and writer. It persists the action
key, output type/schema, ordered 12-byte value and metadata IDs, value size/hash,
Compact Binary metadata size/hash, ordered messages, payloads, and an envelope
hash. Raw decode retains views into the record allocation; compressed decode
owns one inflated allocation. Schema-1 records are cold misses; there is no
compatibility reader or rewrite path.

The filesystem backend atomically stores encoded record bytes directly, with
path containment and byte limits but no separate size/hash envelope. The record
codec owns envelope and value/metadata integrity checks. Compressed records retain
both the compressed envelope hash and the raw record hash; decoding validates the
compressed envelope before inflation and the raw record afterward. Legacy filesystem
envelopes are rejected and use the normal cache-failure rebuild path.

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
cache hit and local build. It contains no query/store bookkeeping, execution
report, cache error, metric, timing, or internal phase identifier.

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

Engine asset families submit directly through their own synchronous, inline
sessions because their callers are already admitted owner workers. ShaderBuild
does the same through its isolated session. Each synchronous caller owns a
request group and waits explicitly before typed assembly; it never assumes
that completion happened before `Build` returned. Captured input description
runs inside the accepted request. There is no cross-family submission bridge
or second public executor.

## Family boundaries

Texture2D, TextureCube, and VolumeTexture use one texture-owned session for import,
detached build, manager work, PostLoad, and Cook. Completion supplies the cache
key and hit fact. Recipe timing stays inside producer-local qualification paths;
it is not part of DDC output or Engine request diagnostics. Their
version-2 shared-output schemas use fixed indexed value IDs and Compact Binary
metadata; package/Cook serialization is unchanged.

TextureBuild family entrypoints validate recipe values and select output formats.
Texture2D and LDR Cube share a resolved mip request and private BC encoding;
Cube selects one format for all six faces. A single RGBA8 source mip generates
the complete chain, while supplied chains remain intact. Shared mip recipes
return detached platform data and metrics only on success, and compression tasks
drain before borrowed source storage is released. In-progress metrics observation
is diagnostic only. HDR Cube and Volume retain their distinct filtering recipes.
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
Its schema-3 output binds fixed entry IDs and Compact Binary metadata to the
virtual shader path; schema 2 was an implementation-only intermediate and is
cold-invalidated. Shader counters derive from completion status and build-status facts.

Cache success never means an asset is current, applied, GPU-ready, or
physics-ready. Family generation/latest-wins and object application checks remain
authoritative. Game uses cooked values and runtime codecs without authoring build
services. See [Asset Data Lifecycle](AssetDataLifecycle.md),
[Asset Compilation](AssetCompilation.md), [Static Mesh Building](StaticMeshBuilding.md),
and [Shader Cache](../Rendering/ShaderCache.md).
