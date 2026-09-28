# Derived Data Build Interface Alignment Plan

Summary: Align Durin's derived-data build interfaces and responsibilities with Unreal Engine while retaining a bounded local-only implementation.

Last reviewed: 2026-09-29

Status: Active
Completed:

## Current Status

Stage 0 records the selected public architecture and migration boundaries.
Implementation has not started. No build, native-test or performance result is
claimed by this plan. Stages 1-7 remain open.

The implemented contract remains the
[Derived Data Build Protocol](../Runtime/Assets/DerivedDataBuild.md) until each
stage migrates its consumers. This plan supersedes the unimplemented Derived
Data Outcome and Diagnostics plan and follows the completed
[Unified Derived Data Build Architecture](UnifiedDerivedDataBuildArchitecture.md)
and [Derived Data Build Sessions](DerivedDataBuildSessions.md) plans.

## Goal

Make Durin's core derived-data build API use the same concepts for the same
responsibilities as Unreal Engine: `IBuild` owns factories and registration,
`FBuildSession` groups related builds, definitions and actions are immutable
build descriptions, request inputs are distinct from input resolution, and
completion returns status, build facts, cache identity and output.

Keep the implementation proportional to current Durin requirements. The aligned
API is local-only and does not add remote workers, exported actions, transitive
build graphs, a second task scheduler or per-value policy before a production
consumer requires them.

## Scope and Evidence

The migration covers the public and private build APIs in `DerivedDataCache`,
Engine asset-build bootstrap, Texture2D, TextureCube, VolumeTexture, StaticMesh
render data, physics collision cooking, and ShaderBuild. It includes import,
PostLoad/recovery, compilation managers, synchronous tools and Cook consumers in
every project declared by `Durin.dworkspace`.

The current API has UE-derived type names but different responsibilities:

- Engine and ShaderBuild create an `FBuildSession` around one request-specific
  resolver, invoke `ExecuteInline` once and immediately drain or retain the
  otherwise completed session. No production derived-data caller uses the
  generic asynchronous `FBuildSession::Submit` entry.
- Existing asset and shader managers already own scheduling, admission,
  cancellation, latest-wins publication and shutdown. The generic session adds
  another asynchronous state machine without acting as their scheduler.
- `FBuildRunObserver` returns action keys and cache-hit state indirectly, exposes
  internal phase ordering, permits a request callback to replace default cache
  reporting, and mixes metrics with completion facts.
- `FBuildResult` represents cancellation as an error. Family callers inspect
  phase ordinals and categories to reconstruct business outcomes, and equivalent
  failures are classified differently across direct and session execution.
- `ECacheError::Miss` represents a normal lookup result as an error.
- StaticMesh and Shader repeat semantic layout scans between output construction,
  registered-function validation and typed assembly. Other apparent checks
  protect different trust boundaries and must not be removed by analogy.

Implementation anchors include `DerivedDataBuildDefinition.h`,
`DerivedDataBuildFunction.h`, `DerivedDataBuildExecution.h`,
`DerivedDataBuildSession.h`, their private implementations,
`AssetBuildService.cpp`, the five Engine family adapters, and
`ShaderBuildSession.cpp`, `ShaderBuilder.cpp` and `ShaderSharedOutput.cpp`.

## UE Compatibility Boundary

The following Epic public interfaces define the responsibility model, not a
requirement for source or binary compatibility:

- [`IBuild`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/IBuild)
  creates definitions, actions, inputs, outputs and sessions and exposes build
  function registration.
- [`FBuildSession`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/FBuildSession)
  groups related builds that share an input resolver and scheduler.
- [`FBuildAction`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildAction)
  fixes function version, constants and resolved inputs into an immutable keyed
  action.
- [`FBuildCompleteParams`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/FBuildCompleteParams)
  separates request status, detailed build status, cache key and output.
- [`EStatus`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/UE__DerivedData__EStatus)
  distinguishes success, error and cancellation.
- [`EBuildStatus`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/UE__DerivedData__EBuildStatus)
  reports cache and build facts independently from terminal status.
- [`FBuildPolicy`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/FBuildPolicy)
  controls allowed cache and build behavior.

Durin deliberately retains `std::expected` for binary validation and codec
operations, existing shared-buffer types, explicit byte and table limits,
XXH3-128 action identity, and current backend storage. Durin may use constrained
variants or private constructors to enforce completion invariants even where UE
exposes separate fields.

Do not introduce an `IRequestOwner`, generic request barriers, remote/export
status flags, worker registries, transitive definitions, Compact Binary or
per-value cache policy only to match UE names. Add those only with a separately
qualified production requirement. If a retained Durin type has materially
different responsibilities from the UE type with the same name, rename it.

## Selected Architecture

### Build service and session ownership

Add one public `IBuild` service in `DerivedDataCache`. It owns the frozen
function registry, construction factories, cache/execution services and session
creation. Engine and ShaderBuild obtain the service through their existing
explicit authoring bootstrap; no static registration or Runtime-to-Developer
dependency is introduced.

An `FBuildSession` is long-lived and groups builds that share a logical target,
input resolver and dispatch policy. Engine must not create a session per asset
request. ShaderBuild must not retain a list of one-shot completed sessions.
Shutdown closes admission and drains service-owned in-flight requests before
registered producer services unload.

Session dispatch remains an adapter over existing Core tasks and owner managers.
It creates no independent worker pool. Completion may run inline when the caller
is already on an admitted owner worker. A synchronous family entry may use a
private synchronous bridge over the same `Build` contract; `ExecuteInline` and
the standalone public executor are not retained as competing public protocols.

### Definitions, actions and request inputs

`FBuildDefinition` continues to name a function, normalized constants and input
references without containing live objects or a final cache key.
`FBuildAction` continues to bind a registered function/version and resolved
semantic input identities to canonical bytes and a cache key.

Introduce immutable `FBuildInputs` as the request-level container for already
captured metadata and data. Session `Build` accepts optional request inputs next
to the definition or action. Existing texture sources, authored mesh bulk,
prepared collision arrays and shader closures move through these owned inputs
rather than forcing one resolver and one session to be allocated per request.

`IBuildInputResolver` becomes a session-level service for inputs not supplied by
the request. It keeps distinct metadata and data resolution operations so a
valid cache hit does not read source payload bytes. It verifies resolved content
against action identity and never substitutes current mutable asset state or a
physical import file for captured input.

Definition/action/input/output construction uses validated builders or factories
that become unusable after finalization. Exact builder names may follow Durin
conventions, but construction must enforce the same immutability boundary as
the UE interfaces.

### Policy and completion

Replace request booleans and observer-derived facts with an immutable
`FBuildPolicy` and a constrained `FBuildCompleteParams`-style completion.
Initial policy supports only behavior Durin implements: cache query, local build,
store-on-build, force build, returned data and bounded execution limits.
Compression selection belongs to bucket/backend configuration unless a measured
family requirement proves it is request policy.

Completion exposes these independent concepts:

```cpp
enum class EStatus : uint8 { Ok, Error, Canceled };

enum class EBuildStatus : uint32
{
	None = 0,
	CacheKey = 1 << 0,
	CacheQuery = 1 << 1,
	CacheQueryHit = 1 << 2,
	BuildLocal = 1 << 3,
	CacheStore = 1 << 4,
	CacheStoreHit = 1 << 5,
};

struct FBuildCompleteParams
{
	EStatus GetStatus() const;
	EBuildStatus GetBuildStatus() const;
	const FCacheKey* GetCacheKey() const;
	const FBuildOutput* GetOutput() const;
	const FBuildFailure* GetFailure() const;
	const FBuildExecutionReport& GetReport() const;
};
```

The sketch fixes responsibilities, not final storage syntax. Construction must
make contradictory states impossible: `Ok` owns one validated output; `Error`
owns one stable failure and no output; `Canceled` owns neither publishable output
nor business failure. Cache key and execution report may be available for every
terminal state after action construction.

Submission rejects with a distinct `FBuildAdmissionError` and invokes no
completion callback. Accepted work completes exactly once as Ok, Error or
Canceled, including dropped dispatch work and shutdown cancellation. Request
handles may remain Durin's cancellation owner instead of adding UE's complete
request-owner framework.

`FBuildFailure` has a stable reason, diagnostic operation, bounded description
and optional producer-domain diagnostic. Operation or internal phase never
determines the business reason. Missing function, closed session and capacity
rejection are admission reasons; invalid input, unavailable input, producer
failure, invalid output, resource exhaustion and internal failure are execution
reasons.

### Remove request observers

Delete `FBuildRunObserver` and every request-level `OnAction`, `OnCacheHit`,
`OnPhase`, `OnCacheIssue` and `OnMetric` callback.

- Cache key, hit/miss, local-build and store facts belong to build status and the
  completion report.
- Internal phase transitions belong to tracing and cannot be a family business
  interface.
- Cache diagnostics have one service-configured reporting sink. Installing a
  metrics consumer cannot suppress default diagnostic delivery.
- Producer scalar metrics remain available through `FBuildContext`, backed by a
  service-level metrics/trace sink rather than a request callback.
- Backend and codec fault injection moves to service/executor construction or
  dedicated test fixtures, not ordinary request options.

Diagnostic and metric sinks are noexcept/best-effort observations. They cannot
alter terminal status, retain products, or participate in action identity.
Normal cache misses and cancellation do not emit error logs.

### Cache recovery and validation ownership

Cache lookup returns `expected<optional<Bytes>, CacheFailure>`: an empty optional
is a normal miss. The executor owns fallback policy. Corrupt cached data may be
rejected and rebuilt; environmental or resource failures are not reclassified
as corruption. Valid local output survives record, encode, compression and store
failure, with the failure recorded in the execution report.

Generic output envelope, schema and size limits belong to DerivedDataCache.
Family semantic validation belongs to the registered build function. Typed
construction and publication remain with the family owner. Captured metadata
checks, materialized identity verification and generation/latest-wins publication
checks protect different boundaries and remain.

Remove equivalent repeated scans only with evidence. StaticMesh and Shader are
the initial candidates. A trusted assembly entry or validation receipt must bind
the exact immutable output, action, validator version and applicable limits; a
forgeable boolean or request-local typed product hidden in a stateless build
function is forbidden. Physics retains its distinct serialized/native validation
passes unless tracing proves they are equivalent.

### Compatibility and identity

The migration changes API ownership, completion and execution plumbing. It does
not by itself change action canonical bytes, family function versions, DDC keys,
shared-output schemas, authored package bytes or Cook payloads. Record intentional
identity changes separately and update goldens only when the semantic input or
output contract changes.

Do not retain a permanent compatibility facade for `ExecuteBuildRequest`,
`ExecuteInline`, request-bound sessions, `FBuildRunObserver`, cancellation as an
error category or phase-order error translation. Temporary adapters must be
private, stage-bounded and removed before the owning stage closes.

## Implementation Stages

### Stage 0: Freeze the UE-aligned responsibility contract

- [x] Record the UE reference responsibilities and deliberate Durin subset.
- [x] Select long-lived sessions, request-owned `FBuildInputs`, structured
  completion and removal of all request observers.
- [x] Protect canonical identity, warm-hit source avoidance, family validation
  and publication boundaries.
- [x] Identify the current one-shot session and duplicate-validation evidence.

Outcome: this document is the selected architecture. It makes no implementation
or validation claim. Later deviations require a recorded rationale before code
continues.

### Stage 1: Introduce aligned core value and completion types

- [ ] Inventory every affected symbol and consumer across the source and test
  roots of all projects declared in `Durin.dworkspace`.
- [ ] Add immutable `FBuildInputs`, `FBuildPolicy`, three-state completion,
  `FBuildAdmissionError`, stable execution failures and build-status flags.
- [ ] Separate cache miss from backend failure and preserve binary codec result
  contracts.
- [ ] Add constrained factories/builders and tests for invalid state prevention,
  canonical ordering, limits and completion invariants.
- [ ] Keep existing action bytes, keys and family output schemas unchanged.

Depends on Stage 0. Complete when the new value types and behavior matrix have
focused tests and no new API permits contradictory status/output/failure state.

### Stage 2: Establish `IBuild` and persistent sessions

- [ ] Add the `IBuild` service, registry/factory ownership and persistent session
  creation at explicit authoring bootstrap.
- [ ] Change session `Build` to accept a definition or action plus optional
  request inputs, policy and completion.
- [ ] Convert input resolution to session-level metadata/data services and prove
  supplied request inputs bypass unnecessary resolver work.
- [ ] Centralize admission, cancellation arbitration, exception translation,
  shutdown and drain accounting in the service/session path.
- [ ] Provide a private synchronous bridge for already-admitted owner workers
  without creating a second scheduler or public execution protocol.
- [ ] Add lifecycle, inline-completion, rejection, dropped-work, reentrancy and
  shutdown tests before migrating family production callers.

Depends on Stage 1. Complete when one persistent test session safely executes
multiple independent requests and no lifecycle guarantee depends on a one-shot
resolver closure.

### Stage 3: Migrate the texture families and remove observers

- [ ] Migrate Texture2D as the first complete vertical slice through `IBuild`,
  persistent session, request inputs, policy and structured completion.
- [ ] Migrate TextureCube and VolumeTexture through the same family boundary.
- [ ] Replace `OnAction` and `OnCacheHit` captures with completion cache key and
  build-status flags.
- [ ] Remove phase-order classification and centralize texture failure mapping.
- [ ] Route metrics and cache diagnostics through service-level sinks; remove all
  texture request observers and observer-dependent tests.
- [ ] Prove warm hits perform no source-payload resolution and cold builds retain
  immutable captured input after live-object mutation.

Depends on Stage 2. Complete when all three texture families use the aligned API
for import, detached build, manager work, PostLoad and Cook, with unchanged keys
and payload schemas unless separately recorded.

### Stage 4: Migrate StaticMesh render and physics collision

- [ ] Replace request-bound Engine sessions with persistent build-service sessions
  and request-owned mesh/collision inputs.
- [ ] Preserve source/reconciliation identity separation and editor-gated direct
  runtime physics behavior.
- [ ] Translate completion once into typed family outcomes without inspecting
  generic phase ordinals.
- [ ] Preserve working-set admission, cancellation, generation checks and typed
  publication ownership.
- [ ] Qualify cold, warm, corrupt, cancellation, Cook and shutdown paths.

Depends on Stage 3. Complete when Engine has no production one-shot DDC sessions
and both families use the same aligned completion and reporting contract.

### Stage 5: Migrate ShaderBuild and retire duplicate scheduling

- [ ] Move captured shader closures into request inputs or the shared session
  resolver according to their actual ownership; retain bounded filesystem and
  dependency-content verification.
- [ ] Reuse a persistent ShaderBuild session and remove the retained vector of
  completed one-shot sessions.
- [ ] Preserve ShaderBuild's existing single-flight, LRU, owner scheduling and
  waiter cancellation without duplicating them in DerivedDataCache.
- [ ] Replace observer phase classification with build status and execution-report
  counters, preserving compiler-domain diagnostics.
- [ ] Qualify compile, DDC, LRU, corruption, force-build, cancellation, reload and
  shutdown behavior.

Depends on Stage 4. Complete when all six producer families use the same core
interface and DDC owns no second shader scheduling policy.

### Stage 6: Remove legacy surfaces and proven duplicate work

- [ ] Delete `FBuildRunObserver`, public `ExecuteBuildRequest`, `ExecuteInline`,
  per-request cache-operation overrides and unused generic `Submit` compatibility
  surfaces superseded by aligned `Build`.
- [ ] Search every declared project for old cancellation errors, observer hooks,
  phase-order mappings and request-bound session creation.
- [ ] Trace validation passes and remove only equivalent repeated scans, starting
  with StaticMesh and Shader.
- [ ] Add bound validation receipts or trusted family assembly entries only where
  scan-count and corruption tests prove safety and benefit.
- [ ] Measure cold and warm build overhead against the pre-migration baseline;
  investigate regressions before closing the stage.

Depends on Stage 5. Complete when no production or test consumer requires the
legacy protocol, no family obtains result facts through callbacks, and targeted
evidence proves the intended validation reduction.

### Stage 7: Qualify and publish the implemented contract

- [ ] Update the owning build protocol and affected family contract documents;
  keep implementation stages and historical rationale in this plan.
- [ ] Run relevant DerivedDataCache, build-session, texture, StaticMesh, physics,
  shader, asset-compilation and Cook native tests under the selected host profile.
- [ ] Validate affected project targets and complete the required shared Engine
  API `all` build.
- [ ] Run changed-document and all-plan validation and record exact receipts,
  host limitations and deferred platform gaps.
- [ ] Search every `Durin.dworkspace` project for obsolete API symbols and direct
  cache/build bypasses.
- [ ] Commit isolated changes with exact `Plan` and `Stage` trailers and mark the
  plan complete only after all gates pass.

Depends on Stage 6. Follow [build guidance](../Agents/BuildAndRun.md),
[native testing guidance](../Agents/Testing.md) and
[documentation guidance](../Agents/Documentation.md). Do not overlap native
build process trees or infer untested platform behavior.

## Behavioral Acceptance Matrix

| Scenario | Required observation |
| --- | --- |
| Persistent session | Multiple related requests reuse one session; no request creates and drains a session solely to execute once. |
| Request-supplied inputs | Immutable captured inputs execute without a request-specific resolver or live-object fallback. |
| Valid cache hit | Ok with key and `CacheQueryHit`; no source data resolution or producer invocation. |
| Normal miss | Cache query recorded, local build allowed, no error diagnostic for the miss. |
| Corrupt cache output | Rejected once, rebuilt when policy permits, rejection retained in the execution report. |
| Cache infrastructure failure | Actual cause is reported; fallback follows policy and is not mislabeled as corruption. |
| Valid build with store failure | Ok with usable output; store attempt/failure remains visible in the report. |
| Producer or output failure | Error with stable reason and no publishable output or cache store. |
| Cancellation | Canceled with no business failure and no publishable output; accepted work completes exactly once. |
| Admission rejection | Typed admission error and no completion callback. |
| Completion facts | Key, cache/build/store status and diagnostics are available without observers or phase inference. |
| Diagnostic/metric sink failure | Terminal outcome and accounting are unchanged. |
| Shutdown | Admission closes, accepted work terminates, callbacks retire and producer services unload only after drain. |
| Family publication | Generation, latest-wins, object application and GPU/physics readiness remain owner-controlled. |
| Validation optimization | Equivalent scans decrease; malformed cache output and mismatched receipts cannot reach trusted assembly. |
| Compatibility | Unchanged semantics retain existing action keys, output schemas, authored bytes and Cook payloads. |
