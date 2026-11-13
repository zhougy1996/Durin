# UE-Aligned Garbage Collection Plan

Summary: Align GC request, scheduling, execution, and host lifetime contracts with UE 5.8 while retaining room for incremental collection.

Last reviewed: 2026-11-10

Status: Archived
Completed: 2026-11-10

## Current Status

Completed all stages on 2026-11-10. Baseline:
`01e7bb22e7ac25096f17a6138f8f24093f0566ea`; audit commit `449b9ea73`,
API separation `776bbdb3f`, synchronous execution/host safety `c1533a6e3`.
Final validation uses `c1533a6e3` plus the SceneImportTests dependency-release
fixture correction included in the final handoff commit. That correction changes
only the test; final plan edits do not invalidate executable results.

Profile: `windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, MSVC 14.44,
14 build jobs. MSVC ran outside the sandbox after confirmed sandbox PDB-service
C1902 failures. No toolchain or runtime setting changes were required.

| Selection | Passed coverage |
| --- | --- |
| CoreObjectTests | 181 tests, including policy, request merging, Try refusal/readiness, destruction requests, synchronous-entry diagnosis and module-default draining. |
| CoreTransactionContractTests | 19 tests. |
| WorldTests | 240 tests, including standalone initialization/failure, retirement, Timer/Actor callbacks and null-World Tickable requests. |
| EditorOperationTests | 343 tests plus the subsequently added protected-factory regression (1 test). Existing test bodies and production inputs were unchanged after the full passing run. |
| ViewportTests | 119 tests, including Engine scheduling across hosts without Worlds. |
| AssetPackageTests | 236 tests, including synchronous package release and residency. |
| AssetRuntimeLifecycleTests | 1 test. |
| SceneImportTests | 33 unchanged tests from the full run, plus the corrected SavedGltfOutputsReloadWithoutSourcesAndRejectEveryReimportRoute case (1 test). |
| ExternalToolTests | 7 tests. |
| EngineFrameTimingTests | 1 test. |
| LaunchStartupCharacterizationTests | BoundedTickExitUsesNormalApplicationShutdown only, with `--mode characterization`; normal Project Browser startup and exit after two ticks passed. |

Reproduce ordinary selections with `.\DevTool.bat test <Target>`. The two
supplemental filters are
`DFactoryTests.FailedCandidateRequestsCollectionInsideProtectedCallback` and
`FSceneImportTests.SavedGltfOutputsReloadWithoutSourcesAndRejectEveryReimportRoute`.
The startup filter is
`FLaunchStartupCharacterizationTests.BoundedTickExitUsesNormalApplicationShutdown`.

The required `.\DevTool.bat build all` passed in 65.18 seconds, including
Sandbox and RoadWeaver consumers. Receipt:
`Build/.agent-state/logs/20261110-184349-596793-31676-cmake.log`.
The first startup attempt exposed stale MainFrame DLL imports; the all build
refreshed them and the same smoke then passed. Final smoke receipt:
`Build/.agent-state/logs/20261110-184511-379744-33964-ctest.log`.
Changed-document validation, all-plan lifecycle validation and diff checks passed.
No remaining acceptance gates; incremental/concurrent GC and GPU qualification
remain outside scope. The full startup characterization suite was not run.

### Implementation Decisions And Validation Corrections

Existing narrow collection scopes remain: population, failure deinitialization
and dynamic removal have independent entry paths, so frame scheduling does not
prove them redundant. World, transaction, reflection and toolkit scopes protect
independent storage boundaries. No scope was removed to bypass a diagnostic.

Failed asset candidates mark their hierarchy and request GC. Factory validation
also exposed a retained CreationOwner passed to ordinary UnloadPackage during
rollback; failed candidates now follow the same retirement path as duplicate and
import rollback. Package/module release retains synchronous completion semantics.
Fixture owners now retain Levels/source assets explicitly across collection and
allow dependent packages to be collected in the same release pass.

Launch formerly advanced GFrameCounter between host work and scheduled GC.
It now advances at admitted frame entry, so explicit startup/map-work collection
and frame-end scheduling share an identity. GC remains after rendering.
The affected World tests also contained one stale PrepareForShutdown call;
its idle owner boundary now uses the existing PreExit API.

### Caller Audit And Selected Boundaries

The audit searched Engine, Sandbox and RoadWeaver source/test roots from
`Durin.dworkspace`. Production synchronous callers are:

| Caller | Boundary and completion requirement |
| --- | --- |
| Launch EngineLoop | Scheduled frame end after rendering; shutdown explicitly performs GC, render flush, GC before code unload. |
| CoreDObject class/struct default publication failure | Failed construction batch has unwound; release failed defaults before reporting publication failure. |
| ReleaseClassDefaultObjectsForModule | Module owner stops users before unloading; synchronous drain determines whether native code can be released. |
| AssetRuntime UnloadPackage/ReleasePackages | Idle asset operation with loading/reference checks; synchronous reachability determines success and residency restoration. |
| AssetPackageRemoval | Validated removal operation; retire object graph before publishing disk/catalog removal. |
| CanonicalResave | Offline batch boundary after dropping source values. |
| DurinAssetTool | Services stop before final object drain and task shutdown. |
| AssetOperations and SceneDirectImport rollback | Mark failed candidate hierarchy and request GC; no completion-dependent result, may run inside editor callbacks. |

Package residency release already requests GC. Test fixture teardown and
standalone test-body collections are synchronous owner boundaries; callback
fixtures in reflection, World, subsystem, Tickable and Editor tests must request
collection instead. Sandbox and RoadWeaver calls are test teardown/body boundaries.
Independent serialization, reflection, construction, transaction and toolkit
execution scopes retain their local borrowed-storage protection. World operations
retain their complete boundary. Collection-local scopes may be removed only when
host coverage proves them redundant; initialization and dynamic mutation remain
independent boundaries until then.

Selected accounting: one process-wide request and collection record, using
`GFrameCounter` for Engine scheduling deduplication, including startup/no-World
frames and host replacement. Explicit collection updates this record. Failed Try
entry does not consume a request or frame opportunity. Standalone hosts may
collect explicitly after operations unwind; hosts using Conditional must advance
the process frame counter. No loop is required for request or execution APIs.
KeepFlags requests retain their existing bitwise union; servicing a request uses
NoFlags as the base, while explicit options union with pending request flags.
Requests made during destruction survive because the serviced request is removed
before callbacks begin.

The configured Win64-Debug-DurinEditor registry exposes CoreObjectTests,
WorldTests, AssetPackageTests, AssetRuntimeLifecycleTests, EditorOperationTests,
ExternalToolTests, EngineFrameTimingTests and LaunchStartupCharacterizationTests;
module unload has separate DynamicDllUnload qualification coverage. Existing
scheduler policy, nested deferral, World retirement, subsystem failure and package
release tests are reusable. Missing coverage: Engine Force/Conditional APIs,
process frame deduplication across hosts, Try execution refusal/result semantics,
and requests emitted during collector destruction. Final startup coverage selects
one normal bounded startup case, not the full characterization suite.

Local UE 5.8 UnrealEngine.cpp and GarbageCollection.cpp confirm deferred Force,
frame-gated Conditional, policy-only Delay, and bool execution Try semantics.
Durin keeps its existing options and single-threaded admission; it adds no ignored
full-purge flag, lock/retry policy, or incremental phase.

## Goal

Separate requesting GC, deciding when to run it, and executing it. Align interface
names and observable semantics with local UE 5.8, without importing unsupported
incremental mechanisms or silently changing synchronous callers into deferred work.
World, Engine, and Editor own safe execution phases and resource lifetime;
CoreDObject owns reference tracing, object destruction, and collector integrity.

Required references:

- [GC contract](../../../Runtime/ObjectSystem/GarbageCollection.md)
- [Runtime lifecycle](../../../Runtime/Application/RuntimeLifecycle.md)
- [Subsystem lifecycle](../../../Runtime/Engine/Subsystems.md)
- [Tickable objects](../../../Runtime/Engine/TickableObjects.md)
- [World subsystems](../../../Runtime/World/WorldSubsystems.md)
- [Build workflow](../../../Agents/BuildAndRun.md) and [test workflow](../../../Agents/Testing.md)

## UE Reference And Current Differences

Use `E:\Programs\Epic Games\UE_5.8\Engine\Source` as the local reference.
Verify symbols again against the implementation baseline:

| UE source and symbol | Semantics to preserve |
| --- | --- |
| `Runtime/Engine/Private/UnrealEngine.cpp`: `UEngine::ForceGarbageCollection` | Request collection at the next eligible opportunity; not synchronous collection. |
| Same file: `ConditionalCollectGarbage` | Own scheduling decisions and per-frame deduplication. |
| Same file: `DelayGarbageCollection` | A scheduling delay, not an object lifetime or callback protection scope. |
| `Runtime/CoreUObject/Public/UObject/UObjectGlobals.h` and `Private/UObject/GarbageCollection.cpp`: `CollectGarbage`, `TryCollectGarbage` | Execute or attempt collection independently of periodic scheduling; Try returns whether collection was actually attempted. |
| `Runtime/Engine/Private/World.cpp`: `UWorld::CleanupWorld` | World orchestrates subsystem teardown as part of resource cleanup. |

Durin's current `TryCollectGarbage(double)` instead evaluates scheduling policy
and returns a trigger reason. Its `CollectGarbage` silently queues a request when
a deferral scope is active. Both differ observably from the corresponding UE
execution interfaces. UE also has locks, loading restrictions, retry behavior,
and incremental phases; matching names alone does not implement those mechanisms.

## Selected Interface And Contract Decisions

### Requests And Scheduling

- Introduce `DEngine::ForceGarbageCollection()` as the normal Engine-facing
  request API, and `DEngine::ConditionalCollectGarbage()` as the scheduled entry.
  Force does not execute GC or bypass safety admission.
- Keep the existing frame-end orchestration point in EngineLoop unless the
  caller audit establishes a concrete ordering defect. Route it through the
  Engine scheduling entry; cover frames with no active World and startup frames.
- Retain the existing interval/backoff/pressure policy as an implementation
  component. Do not replace it with UE's server or memory heuristics in this work.
- Maintain one process-wide pending request and collection accounting source.
  Engine requests, Engine-free requests, and explicit collection must not acquire
  separate queues or lose requests when a host is replaced or retired.
- Retain `RequestGarbageCollection` for justified Engine-free callers. It is an
  explicit Durin extension, not an alternative scheduler. Preserve existing
  KeepFlags merging until the caller audit defines and tests any replacement.
- Retire the scheduling overload `TryCollectGarbage(double)` after migrating all
  consumers. Trigger reasons remain diagnostics/policy results, not execution
  success values. Policy helpers may continue to accept explicit time for testing.

### Collector Execution

- `CollectGarbage` is a synchronous execution entry at an admitted safe point.
  It must no longer silently turn an invalid synchronous call into a request.
  Diagnose illegal use; ordinary callbacks request collection instead.
- Provide a distinct `TryCollectGarbage` execution entry if needed by the Engine
  scheduler: it accepts collector options, not time, and returns false when
  collection cannot currently begin. True means an actual collection attempt,
  not that every pending object was destroyed. Do not invent UE-style lock
  contention or forced retry behavior for the current single-threaded collector.
- An unsuccessful attempt preserves pending requests. A successful attempt
  consumes only the request it services; requests issued by destruction callbacks
  survive for a later opportunity. Explicit collections update shared scheduling
  accounting so the next scheduled pass does not repeat work unnecessarily.
- Keep object readiness and deferred final destruction semantics intact. A
  synchronous collection does not promise to destroy externally retained objects
  or objects that are not ready for final destruction.
- Keep CoreDObject independent of Engine, World, and Editor. Collector reentrancy,
  valid-thread checks, and reference-retention requirements remain local contracts.

### Host Lifetime And Safe Points

- Ordinary subsystem, Tickable, gameplay, and external tool callbacks request GC;
  they do not directly perform host cleanup or synchronous collection.
- World owns complete operation boundaries and retains resources until those
  operations unwind. Engine/Editor close admission, stop consumers, drain or
  cancel owned work, and finish callbacks before teardown or module unload.
- Initialization, initialization failure, dynamic service mutation, null-World
  ticking, standalone Worlds, and shutdown all need explicit safe-point owners.
  A frame-end scheduler alone does not protect these paths.
- Explicit synchronous GC remains available to shutdown, package/module release,
  commandlet-like execution, and tests when their callers establish the boundary.
  Preserve callers that need destruction completed before proceeding or unloading
  code; never convert them to requests merely to remove an assertion.
- Remove collection-level GC deferral scopes only after their callers are covered
  by the host contract and tests. Retain narrow protection where a real independent
  boundary needs it. Do not add a generic host-operation wrapper or replace a
  lifetime scope with a one-frame delay flag.
- Preserve subsystem failure cleanup, visibility, ordering, collection reuse
  rules, and Tickable registration/thread contracts. Those are separate changes.

### Deferred Features And Extension Boundaries

Incremental marking/purge, per-frame GC time budgets, concurrent GC, async-loading
coordination beyond current requirements, server heuristics, and one-frame delay
policy are outside this plan. Keep scheduling policy separate from collector
execution so they can be added without changing request versus execution semantics.

Do not expose ignored `bFullPurge` flags or inert phase/budget APIs. Add UE-shaped
full-purge options when the collector can distinguish and honor them. An eventual
Try result must still mean that work was attempted, not that all incremental work
finished. If existing collector options remain, document this deliberate signature
difference instead of claiming exact UE API compatibility.

## Implementation Stages

### Stage 0: Audit Callers And Establish The Baseline

- [x] Confirm a resolved checkout baseline and record its revision.
- [x] Search GC APIs and deferral scopes across every source/test project declared
  in `Durin.dworkspace`; classify callers as request, scheduled execution, or
  required synchronous execution, with their safe-point owner.
- [x] Inspect scheduler, GC, asset/module unload, World, Editor, and Launch tests
  through the configured test registry. Identify coverage gaps before changes.
- [x] Resolve frame identity/accounting for no-World and standalone execution,
  startup/shutdown behavior without a running loop, and request KeepFlags handling.
  Record selected answers here before implementation.

Acceptance: every synchronous caller has a documented boundary and required
completion semantics; no ownership question is deferred to an assertion removal.

### Stage 1: Separate Scheduling From Execution APIs

Depends on Stage 0.

- [x] Introduce Engine Force/Conditional entries using existing policy and shared
  request state; migrate the frame-loop caller and scheduling-name consumers.
- [x] Introduce/use the collector Try entry with actual-attempt return semantics;
  retire the old time-taking overload without leaving permanent forwarding APIs.
- [x] Cover no-World frames, per-frame deduplication, explicit requests despite
  disabled automatic policy, failed attempts, and explicit-collection accounting.
- [x] Update the implemented GC and runtime lifecycle documentation; explicitly
  retain the old deferral behavior until Stage 2 changes its contract.

Acceptance: scheduling and execution are distinct, all consumers compile, and
request/trigger/result semantics have focused regression coverage.

### Stage 2: Enforce Synchronous Execution And Host Safety

Depends on Stage 1.

- [x] Migrate ordinary callback GC calls to requests and retain legitimate
  synchronous package/module/shutdown operations at established safe points.
- [x] Change CollectGarbage to diagnose invalid synchronous entry; test Try refusal
  and request preservation instead of silent conversion.
- [x] Cover requests during Initialize/Deinitialize/Tick/destruction, host shutdown
  within a callback, failed initialization, standalone World usage, Editor dynamic
  mutation, and module unload that requires synchronous object release.
- [x] Remove only proven redundant collection deferral scopes, updating the GC,
  subsystem, World, Tickable, and Editor contracts with the same change.

Acceptance: callbacks cannot cause premature destruction; required synchronous
callers observe their completion contract; no host/module code unloads while
owned callbacks or object destruction still need it.

### Stage 3: Final Validation And Handoff

Depends on Stage 2.

- [x] Run focused scheduler/collector tests and affected asset/module, World,
  Editor, and tool integration coverage selected by the caller audit.
- [x] Run one bounded normal startup/shutdown smoke because collection and teardown
  execution boundaries change; do not run the entire startup characterization suite.
- [x] Complete an `all` build once for the final shared CoreDObject/Engine API
  migration and cross-project consumer coverage. Reuse relevant passing results
  rather than repeating this gate after documentation-only edits.
- [x] Validate changed documents and plan lifecycle metadata. Record revision,
  profile, selections, results, and remaining limitations in Current Status.
- [x] Confirm no unused compatibility wrappers, ignored future options, or second
  scheduling/request state remain; move lasting rules into their owning contracts.

Each implementation stage should be a reviewable commit with its contract and
tests. Follow the repository plan/stage provenance requirements. Keep later
incremental-GC work outside these acceptance gates.
