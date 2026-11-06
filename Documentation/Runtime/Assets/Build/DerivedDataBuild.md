# Derived Data Build Protocol

Summary: Define the local build service, immutable request inputs, canonical actions, structured completion, and optional cache persistence for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-11-06

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
service or worker pool. Asset compilation cancels and joins request owners before
producer modules unload. Holders then release their session resources outside
acquisition locks; they have no close/reopen state or drain protocol. ShaderBuild retains
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

`FBuildInputsBuilder` collects already materialized immutable inputs. It retains
neither a resolver nor a deferred failure result. `FBuildSession::Build` accepts
the resulting value beside a definition or action.

A session may retain an `IBuildInputResolver` with immutable captured state.
`ResolveInputMeta` resolves identities before action preparation; `ResolveInputData`
materializes named blocks only after a miss when local build is allowed. Both
receive the caller's `IRequestOwner` and a move-only completion callback carrying
inputs and `EStatus`. They complete once, inline or through owner-tracked work.
Resolved data must match the frozen identities and must never substitute a live
asset or import file for captured state. Engine and ShaderBuild create captured
resolver sessions while reusing the persistent service, scheduler and cache.
Explicit materialized `FBuildInputs` may be supplied alongside a session resolver.

Before producer invocation, DDC sorts input/value tables, rejects invalid or
duplicate identifiers, verifies exact identities, and enforces metadata, value,
byte, and working-set bounds. `FBuildContext` borrows the action and materialized
inputs and exposes cooperative cancellation. Asset compilation owners retain
memory estimation, admission and reservations; producer context has no request
working-set budget. Cache requests retain independent byte and codec limits.
`IBuildFunction::Build` returns `void`; it reads only through
`FBuildContext` and publishes values, Compact Binary metadata, notes, warnings,
deterministic errors, and transient logs into the context-owned output builder.
Notes and warnings may be cached; error messages and transient logs are returned
only with the cold output and prevent cache storage. DDC freezes the output synchronously after
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

`FBuildPolicy` contains a uniform `EBuildPolicy` bitmask using the supported
UE semantics: `BuildLocal`, `CacheQuery`, and `CacheStoreOnBuild`, with `Build`,
`CacheStore`, `Cache`, and `Default` combinations. Default queries the cache,
builds locally on a miss, and stores successful output. `Build | CacheStore`
forces production by omitting `CacheQuery`; there is no separate force flag.
`CacheQuery` never runs a producer. Unsupported policy bits fail admission through
the normal error completion. Remote builds, cache promotion, policy keep-alive and
per-value overrides are not exposed until supported by the implementation.

`FBuildServiceOptions::BuildLimits` and internal session profiles bound input and output product validation,
independently of policy and action identity. `FBuildInputLimits` contains only
input byte, metadata, and value-count ceilings; output validation uses
`FBuildOutputLimits`. There is no separate persistence product limit or
request-selectable completion mode. `FBuildServiceOptions::CacheLimits` configures service-owned cache
record, encoded-byte, and codec working-set ceilings. An injected `ICache` keeps
its own configured ceilings. Public Get/Put requests carry no codec or workspace
options; private backend requests receive the immutable backend configuration.
Successful completion always returns its validated output. Compression and
encoding are cache implementation details, not build service or request policy.
Tests may replace the complete structured cache interface for deterministic
miss, hit, corruption, and store behavior.

The private synchronous backend returns `expected<optional<FCacheRecord>, FCacheError>`;
the public cache interface completes requests through status-bearing responses.
An empty optional is a normal miss and emits no diagnostic. Backend failure is a
real error and is never labeled corruption. When policy allows local build, a
miss, rejected record, or cache infrastructure failure falls through once to
resolution/build. Before accepting a hit, the session compares its output schema
name and version with the prepared action's function descriptor and rejects
records containing error messages. A mismatch or cached error takes the same
fallback path; cache-only requests fail without production or persistence.
The cache validates and decodes persisted bytes before
returning a record. Cache-query failures are logged by DDC before the build falls
back; their internal reason is not returned as completion data.

The private `BuildCacheRecordPrivate.h` adapter converts validated Build output
into a generic keyed record. Its opaque metadata stores the build format marker,
output schema/version, metadata table, and deterministic messages. The inverse
adapter validates this structure and retains existing immutable values without
rehashing their payloads. Transient logs and observations never enter the record.
Only the Build layer interprets these fields; cache storage validates generic
record/attachment integrity. Partial cache queries may inspect the private build
metadata view without claiming their unresolved values form a complete output.
The adapter submits the resulting record to the cache. The cache encodes, compresses, and stores it. Record or
cache-store failure cannot discard an already validated local output; completion
remains successful and DDC logs the store failure. Fresh invalid producer output
and outputs containing errors or transient logs never reach persistence.

The [cache storage contract](DerivedDataCacheStorage.md) owns the canonical CB
manifest, versioned attachment identities, full/metadata/selected queries,
byte/codec budgets, integrity validation, and retain-only atomic publication.
Build sessions retain complete-output validation and local-result preservation.

An output containing an error message discards its values, completes with
`EStatus::Error`, and is not persisted. The next request for the same action
queries the cache and can build again. Previously stored error records are
rejected and rebuilt when local execution is allowed. Infrastructure failures
and cancellation are never cached. Execution failures
may return an observation-only diagnostic output; partial values and metadata
are discarded. Cancellation returns a diagnostic output without product values.

## Completion and failure

Accepted work completes exactly once with `FBuildCompleteParams`:

- `EStatus::Ok` owns one validated output without error messages.
- `EStatus::Error` owns diagnostic output, including for infrastructure failure.
- `EStatus::Canceled` owns diagnostic output without product values.

There is no family-specific producer failure type or validation receipt in DDC.
Families interpret status, deterministic messages and their own structured
completion observations. Message and log text is display data, never control-flow identity. Typed
assemblers reparse and revalidate immutable output at their trust boundary.

The completion exposes `CacheKey`, `Output`, `BuildStatus` and `Status` directly.
There is no expected result or private completion adapter. Diagnostic construction
uses empty keys and invalid outputs directly, without optional wrappers. Final
owner cancellation preserves existing messages, logs, observations and schema while
removing product values and metadata. Cancellation does not synthesize an error log;
`Status` expresses cancellation independently of any existing producer errors.
An already diagnostic-only canceled output is reused without rebuilding it.
`BuildStatus` contains two `EBuildExecutionFlags` facts: cache hit and local build. These flags record the
execution path; `EStatus` independently records success, failure or cancellation. Its fields contain no query/store bookkeeping,
execution report, cache error, metric, timing, or internal phase identifier.
Optional observations live only in the returned cold output.

Deterministic producer errors are bounded output messages. Non-deterministic
producer logs have their own category, severity, and text and make the output
non-cacheable. `FailExecution` reports resource, environment or unclassified
recipe failures without a product or persistence. Existing observations may
accompany the failure; bounded diagnostic text lives in Output messages and logs,
with no separate completion error string. Only input-determined failures
use `AddError`; neither these errors nor budget and module failures become cache entries.
Adapter versions are advanced to invalidate previously misclassified failures. Cache and input infrastructure failures are logged by their owning
layer and are not translated into request-level phase or cause objects.
Cancellation is a distinct terminal status, controlled through `IRequestOwner`.
Request options contain no cancellation token or predicate. Build stages observe
the caller owner cancellation state directly; canceling one owner does not affect
requests submitted through another owner. Resolvers receive `IRequestOwner&`,
and build contexts query that same owner through `IsCanceled()`. Cancellation is execution state and never
becomes a deterministic producer error or cache entry. The asynchronous asset
layer bridges its cancellation source to a private nonblocking owner cancellation signal and decides
whether to accept completed products. Started synchronous Cook, mesh, and texture
algorithms may finish. Owner waiting still includes callback completion.

`FBuildSession::Build` takes an explicit `IRequestOwner` and returns void. Every
request invokes its completion exactly once, including missing functions, invalid
inputs, or dispatch rejection. These failures become
`EStatus::Error` completions with bounded diagnostic text. Scheduler admission
errors stay inside dispatch; registration validates function configuration.
Session creation returns a session handle directly and has no admission-result wrapper. An empty completion callback is accepted and normalized to a no-op.

## Persistent sessions and lifecycle

`IBuild` is implemented by the private `FBuild`, which owns its registry, cache
and configuration directly. Each session captures an immutable registry snapshot
when it is created; later registration affects later sessions without mutating
existing sessions. Registered functions and required services remain owned
until the last admitted request releases them.
`FBuildSession::Build` uses the UE parameter order: definition or action, optional
inputs, policy, request owner, and a move-only completion callback. Resolver and
resource limits are fixed by session/service configuration; there is no per-request
configuration or separate failure result.
One request owns that callback through admission, cancellation and completion;
there is no copied fallback callback or separate submission-result channel. The default producer scheduler uses Core workers; an explicit
`CreateInlineBuildScheduler` executes inline. `CreateTaskBuildScheduler` schedules
on Core workers in scheduler lifetime. Its wait handle uses
Core cooperative waiting, including single-worker nested builds. Scheduler
handles expose waiting only; cancellation belongs to owner-managed requests. Cache operations use
their request owner for cancellation and waiting, without returning batch handles.
Canceling a queued operation completes its callback. Its stale scheduler thunk may
subsequently run or drop without retaining producer resources or delaying logical
owner completion. Before Core starts
or after it stops, the standard scheduler executes inline without starting or
stopping Core. Asset producers explicitly nest their operation completion when parent completion must include it.

All sessions use one ResolveMeta, Prepare, CacheGet, ResolveData, BuildLocal,
CachePut, Complete pipeline. Action preparation runs once; local execution receives
the prepared action and resolved inputs. Local production runs on the selected
producer scheduler; warm hits skip data resolution and local production.

`CreateSession` accepts a name, input resolver and optional producer scheduler.
`GetName` exposes the immutable session identity. Cache dispatch is configured by
the cache implementation; sessions do not choose a cache execution mode. A build
service shares one cache across its sessions, using Core BlockingIO while Core is
running and inline execution during bootstrap. Injected `ICache` implementations
are consumed directly through their public Get/Put contract. Synchronous callers
submit through the same session and wait on their request owner.
Sessions are move-only resource handles, not request owners. Releasing a session
or build service does not cancel admitted work. The request retains the registry,
resolver, scheduler and cache until completion. Asset and shader managers cancel
and join their request owners before releasing module services. There are no
Session/IBuild Close or Drain APIs, request tables, or admission counters.

Public `ICache::Get` and `Put` accept batches, an owner, and a completion callback,
and return void. Every item receives one `FCacheGetResponse` or `FCachePutResponse`,
including batch rejection. Responses carry request name/user data, the record or key,
and `EStatus`. A miss is an error response; its key-bearing record contains no
requested data. Callers use Status to determine availability. Backend errors,
query statistics, and dispatch handles stay private. Empty batches do no work.
Completion order need not match input order, and callbacks may run inline.
The concrete cache keeps atomic batch admission and directly delivers public
responses. Rejection uses the same per-item callback path as ordinary completion.
There is no separate asynchronous cache interface or submission-result protocol.
Completion callbacks are move-only and retain one shared callable across a batch.
Ordinary inline batches advance iteratively; explicit nested inline submissions
complete synchronously before returning to the outer callback. Selection IDs and
request tags are copied and records retain immutable storage.
Captured cancellation and callback state must
remain valid until the request owner drains. Callers stop submitting before owner
destruction. Build and Cache accept the same `IRequestOwner`. An owner retains mutable priority, cancels current/future work,
and waits through callbacks and request-buffer/callable release. Same-owner and
pending same-cache waits anywhere in a nested completion stack are rejected;
reentrant destruction still cancels and leaves shared accounting alive.

`FRequestBarrier` registers a scoped request with the existing owner protocol.
It keeps `Poll` and external waits pending until submitting code publishes its
handles, including nested inline submissions. Cancellation cannot release the
barrier; the barrier must be released before an external wait can finish. A barrier
may also span a single-flight subscription. Priority changes propagate to active
requests and reorder queued native tasks under the queue lock, preserving their age.

Standalone `CreateAsyncCache` without a scheduler uses Core BlockingIO, creates no threads, and admits
at most 4,096 pending/active items with two active operations.
`FCacheDispatchLimits` configures concurrency (clamped to 1..64), capacity
(clamped to 1..4096), and queued data bytes (512 MiB by default). Active slots cover I/O,
compression/decompression, and completion callbacks. Priority selection has FIFO
ties; after 32 dispatch turns of waiting, the oldest pending item wins. This
fairness policy is internal, using admission/dispatch turn differences rather
than updating every queued item. An admitted pending item is dispatched within
32 plus capacity further dispatch turns,
provided active work completes. Mutable owner priority is sampled at dispatch.
Atomic item/byte admission rejection completes every item with `EStatus::Error`
(or Canceled for a canceled owner). Capacity and backend failure details remain
in cache diagnostics; request consumers do not branch on those reasons.

Queued-data charges cover owned value-policy override entries and distinct
payload/metadata backing capacities within each Put record. Shared policies are
charged conservatively per request. Fixed
request overhead uses item admission; record tables use existing record limits.
A uniform-policy Get therefore needs no queued-data bytes even when this budget
is zero. A slice charges its entire retained allocation;
shared allocations across separate requests are charged conservatively per request.
Charges remain through callback exit and are released when request buffers are released. Request objects, record value-descriptor tables, opaque callable captures,
allocator/container spare capacity, backend result data, and active codec workspace
are excluded; this is a coarse queued-data budget, not an RSS bound.
Each private backend request receives the configured record/encoded/workspace ceilings. This controls
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
bytes while still counting their own distinct-identity resolution/decoded bytes
and conservative retained workspace. The table holds at most 64 physical reads,
with at most 64 joined waiters per read; overflow proceeds independently without
retaining another table entry. Waiter cancellation stops only that wait, and a
canceled leader still publishes a completed physical read to its other waiters.
Synchronous filesystem I/O itself remains non-interruptible. Cross-request sharing
retains encoded reads only; decoding is request-local so its cancellation and
workspace budget remain independent.

Scheduled builds prepare action identity, submit a full async Get, and release
the Worker task. A usable full hit completes directly; misses/rejections resume
resolution and production on the captured producer scheduler. Metadata description
runs once, full output conversion remains mandatory, and owner priority, product limits, and backend resource ceilings are
preserved across continuations. A valid local output is retained through async
Put; write admission, serialization, dispatch, backend failure, or a dropped write
cannot discard it. Build has one completion boundary: a cache hit completes after
validation; a local build completes after its cache-store response, or immediately
when no store is required. Store admission, encoding, dispatch, and backend
failures preserve a successful local output. Error outputs complete without
submitting Put. Cancellation observed before completion returns Canceled without
output, including while persistence is queued or running. A completed store can
remain useful in the cache even if its canceled request discards the result.
There is no early-result notification or separate result-ready state.

The completion callback runs exactly once. Its return is not owner completion:
Owner Wait includes completion callbacks and release of their business captures
and cache request buffers. The caller owner is the cancellation and waiting boundary for admitted
work. Public Wait and Cancel return void and reject self-waits. Inline execution
may complete before Build returns. Private wait handles use Core helping.
Callbacks have no fixed thread affinity. Infrastructure failures after action
preparation retain the known key; deterministic messages and cache diagnostics
preserve their existing format. Materialized inputs are released after local
production; the session resolver remains available through build completion.

A scheduler executes or retains its thunk for at-most-once execution. There is no
separate dispatch-rejection result channel. Dropping queued build work completes with Error unless the owner was canceled.
Public builds expose no request handle: owner cancellation signals pending/running
work, and owner waiting joins the registered requests and their callbacks.
Internal scheduler wait handles support Core worker helping. Schedulers without
wait handles require external progress. Completed/canceled requests do not wait
for stale thunks that retain no producer/resolver resources. Callbacks run without
session locks; callback exceptions cannot break accounting.

`IRequest` exposes intrusive `AddRef`/`Release`, `Cancel`, `Wait` and priority
updates. `IRequestOwner` registers raw request pointers through `Begin` and returns
the retained `TRefCountPtr<IRequest>` through `End`. Its callback overload begins a
barrier, ends tracking, invokes the callback while retaining the request, then
releases the barrier. Barrier counters and nonblocking cancellation signals remain
private. Early failure callbacks use the same callback scope and barrier, and
partial admission failure ends every registered request before reporting errors.

`FRequestOwner` is a movable, noncopyable RAII facade, with conversions to
`IRequestOwner&` and `IRequest*`. It supplies cancellation, `Wait`, `Poll`, priority
updates and `LaunchTask`. Destruction cancels and waits unless `KeepAlive` was
called. KeepAlive leaves requests and barriers owning the internal state after the
facade is destroyed. Destroying or synchronously waiting on an owner from one of
its callbacks is rejected. Requests and barriers retain the internal state,
independently of the facade lifetime.

Owner completion means it has no active requests or barriers. Public
`IRequestOwner::LaunchTask` uses a native task request and a publication event:
Begin precedes execution, and the body calls End with the callback. Cancel waits
for that task; it does not execute an alternate cancellation body. Native waits
may help execute queued work. There is no retirement subscription or idle observer.
A cache operation is one owner-registered request and directly holds its scheduler
wait handle; cache dispatch does not register another request. Private build dispatch
and cache operations end at their logical callback boundary. Stale scheduler thunks
do not extend owner completion. Shader jobs end their
registered request through the group completion callback before publishing completion.

Each Build stage uses the caller's public owner protocol. The pipeline retains
session resources, data and its unique completion callback. Request data lives
directly in the build request state, without a separately shared data allocation.
An action supplied by the caller is owned once and validated in place. Definitions
alone go through action construction. Local execution takes action ownership and
releases definition/action metadata before cache persistence; the cache key remains
as the completion identity. Inputs and pending store output have independent lifetimes.
Continuations are submitted before the End callback barrier is released. Requests and barriers retain
the private concrete owner independently of its facade; implementations consuming
`IRequestOwner&` do not cast it to that concrete type. Begin requires a barrier
for registration after the first request, matching UE.

Priority propagation snapshots requests under the owner lock and invokes request
methods outside it. Priority barriers replay changes for late registrations;
forwarding and task-handle publication converge on the latest owner priority
without holding a lock across downstream calls. Callback completion and dispatch
publication have separate responsibilities.

`FBuildSession::Build` takes a definition/action, optional inputs, policy, owner and
move-only callback, and returns void. Service build budgets are configured through
`FBuildServiceOptions::BuildLimits`. Internal family session profiles capture their
resolver and limits before Build; the public Build call carries no request options.
The session resolver remains available through build completion. Captured inputs
may be supplied alongside a session resolver. Memory reservations and raw, encoded
and resident size limits remain independent protections. Typed synchronous
consumers receive one completion; async consumers receive it through their callback.
Product validation and assembly may still return domain-specific errors.

DDC admission does not capture a Core scope or allocate per-request scope anchors.
Engine asset producers use `FRequestOwner` directly. Producer task completion
establishes that no further submissions can occur; owner `Poll()` then confirms
completion of its registered requests and barriers.
`Wait()` joins the same registered-request and callback boundary.
Dispatch publication keeps an owner barrier until its scheduler has returned
and published the task request handle, even if its body completed synchronously. Owner polling
alone cannot close a producer that may still submit another inline request.
Queued requests need no Core worker admission slot, and callback-generated stages
remain admissible while requests are active. Owners allocate no Core completion event;
undispatched asset records therefore do not retain Core shutdown. Requests retain
internal owner state rather than the facade; facade destruction cancels and joins
its work. Asset managers drain producers and their owners before Core shutdown.

Task completion does not forward cancellation. Render and texture managers signal
their request owners; physics owner abort withdraws only unstarted cooks and lets
started work finish normally. Internal batch cancellation affects only that batch; public cancellation is scoped
to the request owner. LaunchTask reads
current owner priority and uses the process scheduler lifetime.
Schedulers and DDC callbacks must progress without GameThread pumping. Native
dispatch handles and the scheduling protocol live in private implementation headers;
public session configuration uses opaque scheduler factories. Private external
schedulers retain responsibility for their own dispatch lifetime.
Managers drain DDC before Core scheduler shutdown.

Inline cache dispatch permits callback reentry and does not use the asynchronous
queue's concurrency gate. Nested inline builds complete before the enclosing
callback returns. Asynchronous dispatch bounds backend IO concurrency and queued
request capacity separately. An operation releases its IO slot before invoking its
business callback, so another owner on the same cache can progress while that
callback is active. Request buffers and retained-byte accounting remain live until
the callback exits. Private operation wait handles cover only their own operations.
Waiting on another request is not rejected solely because it shares the cache.
Self-waits and draining a cache that includes the current callback remain rejected. Shader
callback wait checks likewise target the current request and its nested callback
ancestors; completed and independent requests do not inherit a thread-wide ban.

Request owners cancel and join accepted builds through registered request completion
and callback exit. Session release does not cancel requests. Stale
thunks retain no producer/resolver owners after their request completes.

Texture2D and TextureCube compilation owners submit through a persistent task-scheduled session.
They release their dispatch worker immediately and retain a build request owner
until callback completion, then apply its detached result on GameThread. Owner
priority follows the request; concurrency and estimated memory stay reserved
across asynchronous stages. See [Asset Compilation](AssetCompilation.md#initial-compiling-managers).
Material translation and generated Shader submission also own persistent task
sessions. Material normalization/generation, Shader preparation and CPU assembly
are separate detached continuations; neither producer waits for nested cache I/O.
Engine asset families and ShaderBuild each reuse one named session for synchronous
and asynchronous requests. Each synchronous caller owns a
request group and waits explicitly before typed assembly; it never assumes
that completion happened before `Build` returned. Captured input description
runs inside the accepted request. Producer scheduling adds no second asset reservation. Cache working-set limits
remain independent of producer execution and never affect action identity. These synchronous wrappers
retain their existing execution contract: scheduling continuations back onto an
already occupied asset worker can nest unrelated asset tasks and violate their
mailbox/barrier protocol. Task-scheduled build sessions release producer workers
while cache work runs on BlockingIO. Further family migrations require the same
asynchronous bridge at their asset-compilation owner boundary.
Engine family wrappers submit through their holder and deliver failures through
one completion channel. Texture, mesh, physics, material and shader asynchronous
adapters return void; synchronous DDC adapters return FBuildCompleteParams. Domain
product validation and assembly retain their own error results. Native fixture
guards replace services only after owner completion, publish fully registered
services, and release old resources outside the acquisition lock. Service
replacement applies to synchronous and asynchronous requests.

## Family boundaries

Native Cook class-schema versions describe object-family behavior, not DDC recipes. Unified
package preparation and source-only filtering preserve existing family action
projections, payload codecs and producer versions. The package publisher owns
CookState reuse identity separately through explicit serializer identity/version. Platform resolution supplies capabilities while the family boundary
validates its request profile.

Texture2D, TextureCube, and VolumeTexture use one texture-owned session for import,
detached build, Cook and platform-cache owner work (including authored PostLoad
and editor rebuilds). Noncanonical Cube inputs normalize before submission on the dispatch worker.
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
persistent task-backed session; synchronous Build and detached import retain
the same session with owner waiting. Cook executes its detached synchronous builder adapter on a
worker after owner-thread capture; package admission and teardown follow
[Cooking](Cooking.md#object-lifecycle-and-selection). Both share input validation, request limits and typed output
consumption. Owner callbacks retain detached build completions, and the GameThread
pump dispatches CPU assembly and ray preparation after build-owner completion.
The captured builder version defines the asynchronous request. Physics keeps its
independent collision session and worker path. Source/reconciliation
identity, working-set admission, generation checks, typed construction, collision editor
gating, and publication remain unchanged. Serialized and native physics
validation protect different trust boundaries and remain separate.

ShaderBuild captures immutable dependency metadata into request inputs, reuses
one persistent named service session, and keeps its compiler single-flight, LRU,
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
failure retain the common session policy. Material records retain a stable caller
owner for their own completion and a separate build owner for producer work. The
caller owner tracks the producer through the existing IRequest conversion. Joining
transfers that dependency under a publication barrier without replacing the caller
owner. Shared results end the dependency while publishing each caller's own completion
task; caller Wait/Poll does not include peer callbacks. Cancellation of shared work
and its priority propagation target the producer boundary. Managers drain production
before releasing family services. Complete
assembled material candidates remain in the shared bounded process cache only.

Cache success never means an asset is current, applied, GPU-ready, or
physics-ready. Family freshness and object application checks remain authoritative;
textures serialize mutation against their active request, while other families
retain their own generation rules. Game uses cooked values and runtime codecs without authoring build
services. See [Asset Data Lifecycle](../Packages/AssetDataLifecycle.md),
[Asset Compilation](AssetCompilation.md), [Static Mesh Building](../Families/StaticMeshBuilding.md),
and [Shader Cache](../../Rendering/Shaders/ShaderCache.md).
