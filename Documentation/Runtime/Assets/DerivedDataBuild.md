# Derived Data Build Protocol

Summary: Define the local build service, immutable request inputs, canonical actions, structured completion, and optional cache persistence for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-09-29

## Ownership

`DerivedDataCache` owns the local `IBuild` service, function registration,
immutable definitions/actions/inputs/outputs, persistent sessions, execution,
and backend-neutral byte storage. It depends only on Core. The service adds no
worker pool, remote execution, transitive graph, generic single-flight, or
publication policy. A session dispatches through its caller-supplied adapter or
completes inline.

Engine owns the registered Texture2D, TextureCube, VolumeTexture, StaticMesh
render, and physics collision functions. ShaderBuild owns its shader function.
Each family owns typed assembly, business-error translation, scheduling,
reservations, latest-wins checks, and publication. TextureBuild and MeshBuilder
remain pure recipe modules; ShaderBuild retains compiler scheduling, LRU, and
single-flight.

Engine creates one authoring-lifetime build service and one persistent session
after TextureBuild and MeshBuilder load. ShaderBuild creates one service/session
for its module lifetime. Shutdown closes admission and drains accepted work
before producer services unload.

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
and bucket. Constants are named bool, uint64, float, string, or XXH3-128 values.
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

`Describe` may inspect captured metadata only. `Resolve` runs only after a miss
when local build is allowed; it materializes named immutable blocks and verifies
them against the frozen identity. It must never substitute a current live asset
or import file for captured state. A session-level resolver is available for
truly shared inputs, but Engine and ShaderBuild production requests supply their
own `FBuildInputs` and reuse persistent sessions.

Before producer invocation, DDC sorts input/value tables, rejects invalid or
duplicate identifiers, verifies exact identities, and enforces metadata, value,
byte, and working-set bounds. `FBuildContext` borrows the action and materialized
inputs, exposes cooperative cancellation and the working-set limit, and records
scalar metrics. `IBuildFunction::Build` returns `void`; it reads only through
`FBuildContext` and publishes values, Compact Binary metadata, notes, warnings,
and deterministic errors into the context-owned output builder. DDC freezes the
output synchronously after return. Family semantic validation runs while
producing fresh output and again during typed assembly; DDC owns generic schema,
hash, ordering, and byte-bound validation.

Inputs retain names because they describe producer-facing captured blocks.
Outputs use `FValueId`, a stable 12-byte identity derived from a name or hash.
`MakeIndexed` reserves the final 24 bits for indices `0..0x00ffffff`. Production
families derive every output ID from a fixed family base and index; persisted
records contain no debug-name dependency. `FValue` owns raw XXH3-128, raw size,
and immutable data. Metadata is keyed by `FValueId` and stored as owned bounded
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
cache-operation overrides are service configuration, not request policy.

Cache lookup returns `expected<optional<FSharedByteBuffer>, FCacheError>`.
An empty optional is a normal miss and emits no diagnostic. Backend failure is a
real error and is never labeled corruption. When policy allows local build, a
miss, rejected record, or cache infrastructure failure falls through once to
resolution/build. A corrupt decoded or family-invalid cached output is recorded
in the execution report before rebuilding.

Persistence builds a keyed record, encodes it, optionally compresses it according
to service configuration, and stores it. Record, encode, compression, or store
failure cannot discard an already validated local output; the completion remains
successful and the bounded failure is retained in its report. Fresh invalid
producer output never reaches persistence.

Record envelope schema 2 is the only reader and writer. It persists the action
key, output type/schema, ordered 12-byte value and metadata IDs, value size/hash,
Compact Binary metadata size/hash, ordered messages, payloads, and an envelope
hash. Raw decode retains views into the record allocation; compressed decode
owns one inflated allocation. Schema-1 records are cold misses; there is no
compatibility reader or rewrite path.

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
decide only from status, output presence, bounded output messages, and execution
diagnostics. Message text is display data, never control-flow identity. Typed
assemblers reparse and revalidate immutable output at their trust boundary.

The completion independently exposes its cache key, `EBuildStatus` facts, and
`FBuildExecutionReport`. Status flags record key construction, cache query/hit,
local build, and store attempt. The report carries bounded cache diagnostics,
producer metrics, and persistence timing. Families obtain all build facts from
completion; there are no request observers and no phase-order inference.

Deterministic producer errors are bounded output messages. Cache/backend,
decode, resolve, and executor failures are bounded `FBuildDiagnostic` entries;
their `Operation` is diagnostic context and may select a family presentation
stage, but message text never selects behavior. Descriptions are capped at 4096
bytes. Cancellation is a distinct terminal status.

Admission rejection uses `FBuildAdmissionError` and invokes no completion.
Closed service/session, capacity, missing function, invalid request, dispatch
rejection, and internal admission failure are distinct reasons. Service-level
diagnostic and metric sinks are best-effort/noexcept observations; exceptions
from them do not alter completion or accounting.

## Persistent sessions and lifecycle

`IBuild` owns registration and freezes its internal registry on first session
creation. Registered functions and required services remain owned through drain.
`FBuildSession::Build` accepts a definition or action, optional request inputs,
policy, cancellation, and completion callback. It creates no scheduler. An empty
dispatcher completes inline; an owner dispatcher may run accepted work on its
existing workers.

A dispatcher rejects without invoking or retaining its thunk, or accepts it for
at-most-once execution. Dropping accepted work completes it as canceled. Request
handles can cancel pending/running work. Callbacks execute without session locks;
callback exceptions cannot break accounting.

`Close` stops admission and cancels accepted work. `Drain` waits for execution,
callbacks, dispatch return, and callable-owner release. Calling drain from the
same session execution/completion stack returns `WouldBlock`. Completed handles
and stale thunks retain no producer/resolver owners.

Engine and ShaderBuild use private synchronous bridges only because their caller
is already an admitted owner worker and their persistent sessions have inline
dispatch. There is no second public executor or compatibility submission API.

## Family boundaries

Texture2D, TextureCube, and VolumeTexture use one Engine session for import,
detached build, manager work, PostLoad, and Cook. Completion supplies the cache
key and hit fact. Recipe timing metrics are collected in the report. Their
version-2 shared-output schemas use fixed indexed value IDs and Compact Binary
metadata; package/Cook serialization is unchanged.

StaticMesh render and physics collision use separate definitions/actions and
request inputs through the same Engine session. Source/reconciliation identity,
working-set admission, generation checks, typed construction, collision editor
gating, and publication remain unchanged. Serialized and native physics
validation protect different trust boundaries and remain separate.

ShaderBuild captures immutable dependency metadata into request inputs, reuses
one persistent service session, and keeps its compiler single-flight, LRU,
waiter cancellation, filesystem bounds, and dependency-content verification.
Its schema-3 output binds fixed entry IDs and Compact Binary metadata to the
virtual shader path; schema 2 was an implementation-only intermediate and is
cold-invalidated. Shader counters derive from completion status/report rather
than callbacks.

Cache success never means an asset is current, applied, GPU-ready, or
physics-ready. Family generation/latest-wins and object application checks remain
authoritative. Game uses cooked values and runtime codecs without authoring build
services. See [Asset Data Lifecycle](AssetDataLifecycle.md),
[Asset Compilation](AssetCompilation.md), [Static Mesh Building](StaticMeshBuilding.md),
and [Shader Cache](../Rendering/ShaderCache.md).
