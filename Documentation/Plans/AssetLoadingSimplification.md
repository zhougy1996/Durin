# Asset Loading and Error Simplification Plan

Summary: Replace recursive asset error transport and closure-wide ordinary-load rollback with compact diagnostics, explicit load readiness, and scoped cleanup of incomplete graphs.

Last reviewed: 2026-09-18

Status: Completed
Completed: 2026-09-18

## Current Status

This plan replaces `Documentation/Plans/AssetObjectTypedErrors.md` by explicit
design choice. The former plan is superseded, not completed. Its remaining
cause-retention and rollback-preservation gates are withdrawn. Stage 0's baseline
audit and the subsequent implementation evidence are recorded below. All four stages are complete. Ordinary loading now owns incomplete completion groups and retains
independent successful dependencies.

The former work remains in Git history: baseline `12beed739`, subsequent
`6d317f0b5` and `d569de006`, and branch
`codex/backup-asset-typed-errors-before-squash-20260918`. Existing typed APIs
are inputs to an audit, not requirements to preserve or changes to blindly undo.
Prior test/build passes do not validate the newly selected behavior.

Code inspection identifies three coupled problems. `LoadAuthoredObject`
assembles Archive state, reflected-value causes, and a package-specific error,
then the linker embeds it in `FAssetResult` alongside formatted text. Root load
failure retires all `TransactionPackages`, including successful nested loads.
`FindResidentPackage` does not exclude loading skeletons, and ordinary packages
are Standalone: neither a resident lookup nor loss of the requesting scope is
proof of readiness or eligibility for GC.

Stage 1 validation (2026-09-18, Win64-Debug-DurinEditor):
`AssetPackageTests` passed 178 tests; `AssetPackageReloadTests` passed 14 tests;
`build --target all` passed, including Sandbox and RoadWeaver. After removing
message assertions per user direction, the four affected field-load/binding/retry
cases rebuilt and passed. Changed-document validation passed. Field application
now returns code/message directly, without the package-object wrapper or a
retained resolver operation; all workspace source/test consumers were migrated.

Stage 2 validation (2026-09-18, Win64-Debug-DurinEditor): final
`AssetPackageTests` passed 180 tests, `AssetPackageReloadTests` 14,
`CoreObjectTests` 96, and `CorePropertyValueSnapshotTests` 29. The 24-target
asset-package/material/texture/static-mesh/world/road-graph contract+feature
selection passed 23 targets; its sole failure asserted old dependency rollback.
After migrating that assertion, `MaterialPackageTests` passed all 6 tests.
`RoadSceneIntegrationTests` passed 5 and `SandboxGameplayTests` passed 13.
The final `build --target all` passed. Changed-document validation passed.

Acceptance coverage includes cyclic peer values and group readiness, rejected
public load/save before completion, independent reentrant loads and rejected
back-edges into PostLoad, failed cycle cleanup and same-path retry, serializer
and PostLoad exceptions, retained bulk dependency resources and explicit release,
plus existing private replacement/retirement coverage. Family validators now
reject persisted data before notifications. Defensive checks in directly invoked
family PostLoad methods remain; ordinary loading does not use them as a result
or a rollback vote. GC remains in failed-candidate cleanup. No GPU qualification
was requested or used as evidence for this object-lifetime change.

Stage 3 validation (2026-09-18, Win64-Debug-DurinEditor): final
`test affected --report` passed all 99 selected native targets, including
Core persistence/snapshots/replacement, asset load/save/reload, Cook, import,
compilation and editor workflows, plus Sandbox and RoadWeaver. The default
selection excludes characterization and qualification. The final
`build --target all` passed. Report:
`Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.

The broad run exposed a missed resident-object fallback in the new internal
dependency resolver: catalog refresh could make a completed resident parent
unresolvable even though public `LoadObject` still accepted it. Internal resolution
now preserves that fallback through the same phase/fence checks. The previously
failing `SceneImportTests` suite passed all 10 cases before the final full run.

Removed error-layout and prose assertions in the migrated tests; classification,
outputs, first failure, cancellation, rollback receipts, object lifetime and retry
coverage remain. Changed-document validation and all-plan validation accompany
this completion. Lasting contracts are in AssetPackages, AssetDataLifecycle and
Serialization, rather than relying on this plan as a second specification.

## Goal

Ordinary loading guarantees valid completed results and cleanup of incomplete
work. It does not promise restoration of the pre-request residency set or
reversal of arbitrary callback side effects. Return actionable diagnostics
without preserving a recursively owned tree of operation results. Keep stronger
replacement and persistence guarantees only at operations that require them.

This is a deliberate contract change across Engine and CoreDObject boundaries,
not a compatibility-adapter exercise. Migrate all workspace consumers, tests,
and owning documentation as each boundary changes.

## Selected Design

### Errors and diagnostics

- Ordinary load results carry a stable error classification and an owned,
  complete diagnostic message. Success has one authoritative representation.
  Reuse an existing suitable result rather than adding a parallel framework.
- Capture object/package identity, field route, dependency context, and the
  concrete reason while they are available, before candidate cleanup. A message
  surviving cleanup is sufficient; retaining typed nested causes is not a goal.
- Remove `FPackageObjectLoadError`, its result wrapper, and the corresponding
  `FAssetResult` cause link after migrating their producers and consumers.
  Serializers continue to return void and record the first Archive failure.
- Stop carrying a complete resolver operation result inside a load diagnostic.
  Preserve classification and useful text; actionable recovery or operation
  state belongs to an explicit owning-operation channel when actually needed.
- Audit the former plan's asset/object wrappers, including Capture, codecs,
  graph operations, property operations, Cook, and editor adapters. Keep typed
  fields only where a real consumer needs machine-readable distinctions.
  Formatting may occur at a module boundary; delaying all formatting to UI is
  no longer an invariant. Do not replace every error with an unclassified string.
- Separate load diagnostics from write disposition, recovery locations,
  transaction identity, and affected files. Do not remove recovery semantics
  from save, import, Cook publication, or replacement to simplify a load API.

### Load readiness and cyclic dependencies

- Distinguish internal skeleton availability, value restoration/validation,
  PostLoad execution, completed readiness, and failure. The exact representation
  must use one authoritative load state rather than competing residency maps.
- Internal reference fix-up may resolve a loading skeleton. Public load success
  and ordinary asset consumption require completed readiness. Define explicit
  reentrancy behavior instead of returning a skeleton as a completed package.
- Preserve supported cyclic hard references. Mutually dependent incomplete
  packages form a completion group; one member cannot be retained as an
  independent success while it references a failed member. Audit existing cycle
  support and select the smallest correct group implementation in Stage 0.
- A completed dependency outside the failing group remains valid and may stay
  resident. Root-request failure does not reverse all successful nested loads.

### Cleanup, callbacks, and residency

- A scoped owner retains incomplete objects, temporary registrations, resource
  registrations, and required references. On failure it removes discoverability
  and releases those owned items exactly once. It must survive early returns and
  callback exceptions without masking the original failure.
- Centralize cleanup; remove overlapping manual rollback calls, global
  transaction sweeps, and dependency rollback callbacks that exist solely to
  restore ordinary-load residency. Keep specialized ownership where required.
- Construction, serialization, and validation must not publish candidate
  references or perform external mutations requiring general compensation.
  Serialization modifies its target; dependency resolution uses loader bindings.
  Audit callbacks before relying on this restriction; it is not automatically
  enforced by C++ or by RAII.
- Complete recoverable data rejection and fallible publication preparation
  before PostLoad. PostLoad is a void initialization notification, not a
  recoverable transaction participant. Resource readiness may remain separate
  from successful object loading. Define reentrant loads and exceptional failure
  handling without promising rollback of already executed external side effects.
- Preserve explicit Standalone cache residency for completed ordinary packages
  initially. Successful independent dependencies retained after a failed request
  remain subject to ordinary unload/reference protection; do not claim that GC
  alone will reclaim them. No new automatic eviction subsystem is required.
- Make incomplete graphs inaccessible promptly; physical destruction may follow
  the object/resource retirement rules. Remove forced GC calls only after proving
  safe retry, path reuse, references, and deferred resource release.
- Hot reload, private graph replacement, and persistent writes retain their
  own candidate ownership and commit/abort guarantees. Ordinary dependency
  loading and operation-owned candidate cleanup must have distinct contracts.

## UE Reference Constraints

These sources motivate selected constraints; they do not establish that every
UE loader path is transaction-free or that Durin should copy UE internals.

- [Async loading guidance](https://dev.epicgames.com/documentation/unreal-engine/asynchronous-level-loading-in-unreal-engine)
  restricts custom serialization to its target and recommends deferring global
  interactions, delegate registration, and synchronous loads to PostLoad or later.
- [Object flags](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/EObjectFlags)
  distinguish load and PostLoad stages. Existence alone is not readiness.
- [PostLoad](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject/PostLoad)
  is void. [Rendering lifecycle](https://dev.epicgames.com/documentation/unreal-engine/threaded-rendering-in-unreal-engine)
  shows resource initialization and deferred destruction requiring separate care.
- [Streamable handles](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FStreamableHandle)
  distinguish request completion, per-entry failure, and reference retention.
  Releasing a handle stops its GC retention; it is not a general undo operation.
- [FIoStatus](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FIoStatus)
  demonstrates compact code/message status. UE's public contracts do not require
  the recursive asset/object cause structure introduced by the former plan.

## Implementation Stages

### Stage 0 audit record (2026-09-18)

The workspace inventory covers Engine, Sandbox and RoadWeaver source and native
test roots declared in `Durin.dworkspace`. The package-object wrapper has one
producer (`AssetPackageLoadArchive.cpp`), one production consumer
(`ApplyLinkerValues`, shared by live and private preparation), and assertions in
`AssetPackageTests`. Sandbox and RoadWeaver do not consume those wrapper fields.
RoadWeaver does own a `DRoadNet::PostLoad` data-validation callback.

#### State and completion ownership selected for Stage 2

- Keep a single load record per in-flight package in `FAssetLoadService`, with
  phases Constructing, Skeleton (including restoration), ValuesRestored, Validated, PostLoading, Ready and
  Failed. The record owns the candidate, resource registration, DFS index,
  low-link and pending completion work. Existing completed/created packages
  continue to use the Core package index; no second resident cache is needed.
- Separate loader-only dependency resolution from public `LoadPackage`,
  `LoadObject`, soft resolution and resident enumeration. Only loader bindings
  may return an in-flight skeleton. Public requests for an incomplete package
  return `InUse` with a null output; completed dependencies remain accessible.
  Constructors, serializers and validation must not invoke public live loading.
- Use synchronous DFS strongly connected components. Register the skeleton
  before following hard edges; record every edge admitted by package dependency
  traversal or object resolution. An edge to an active ancestor lowers low-link.
  A component closes at its DFS root after every member has restored values.
  Validate all member graphs at that point, then run completion work. Acyclic
  children close immediately, so a later parent failure does not discard them.
  Never validate a cyclic member against a peer with unrestored values.
- Finish fallible load/publication preparation for every member before the first
  PostLoad. Run dependencies before dependents; within a component use stable
  DFS completion order and preserve reverse object order within each package.
  Mark the whole component Ready only after all notifications return. No member
  is an independent success before that transition.
- During PostLoad, public access to Ready packages is allowed; access to the
  current incomplete component is `InUse`. An independent synchronous load may
  complete, but a new load that points back to a PostLoading component is rejected
  rather than joining a component whose notifications have started. Internal
  references already bound within the component remain usable; PostLoad must
  not assume a cyclic peer has already received its notification.
- One scoped owner discards each failed incomplete component: hide/mark all
  candidates first, retire its registered resources, release pins and temporary
  records once. Restore load depth, active report and traversal state on every
  exit, including exceptions. Convert callback exceptions at the public load
  boundary to an owned diagnostic; cleanup must not replace the original error.
  Propagate failure to unfinished dependents, never sweep completed components.
- Keep forced GC initially: `MarkObjectHierarchyAsGarbage` changes flags but does
  not remove the Outer index, and immediate same-path retry must be proven before
  changing physical retirement. Completed ordinary packages remain Standalone;
  `UnloadPackage` and `ReleasePackages` retain their saved/live-reference checks.
- Explicit `FAssetPackageLoadScope` records newly completed packages even if a
  later requested root fails. Its explicit release remains operation-owned.
  `PreparePackageGraphs`, `FPreparedPackageGraph::FState`, replacement resource
  receipts and reload commit/abort keep their private ownership. Direct linker
  ordinary loading no longer owns residency-restoration callbacks; explicit
  replacement policies remain separate.

Baseline code paths grounding the Stage 0 model: `LoadPackageFromPhysicalPath` owns
`TransactionPackages` and active reports; `LoadPackageInternal` registers bulk
resources and skeleton callbacks; `ApplyLivePackageLinker` owns local rollback and
PostLoad; `PreparePackageGraphs` creates pinned private graphs; `PackageReload`
runs private runtime preparation and explicit dependency release. Public
`FindResidentPackage` currently ignores `LoadingPackages`, enabling cyclic
references but also premature public success. Current root rollback retires
successful dependency resources, and load depth/report restoration lacks an
exception scope. This paragraph records the pre-implementation baseline; Stages 1 and 2
replace those paths and establish the guarantees above.

#### Callback migration and acceptance map

| Boundary | Required implementation and acceptance |
| --- | --- |
| Custom `Serialize` / `SerializeCooked`, native constructors, struct repair | Restrict writes to the candidate and resolve references through supplied bindings. Guard public loading; test ignored rejected calls and thrown callbacks. Material serializers already use graph validation; keep first Archive failure and its original message. |
| Texture and StaticMesh PostLoad | Move authored-source/slot and required cooked-field rejection into graph validation before notifications. Keep compilation/resource readiness separate; test invalid data never schedules work and retirement remains safe. |
| Material / MaterialFunction / MaterialInstance | Preserve existing material graph validation; move remaining static-property, parent-cycle and cooked-payload data rejection before notifications. Schema/cache initialization, compilation and graph notifications happen only after group validation. Test peer values and cyclic parent rejection. |
| Actor / Level / MeshComponent | Audit reconstruction and ownership/material-override repair against pre-notification validation. Reject malformed persisted ownership/overrides before callbacks; keep native reconstruction as initialization. Test child values and no notification after recoverable rejection. |
| Spline / SplineMesh / RoadNet | Spline updates and mesh requests stay initialization; RoadNet schema/definition rejection moves to graph validation. Validate RoadWeaver targets as well as Engine. |
| Linker injected publication / PostLoad failures | Move recoverable injected gates before notifications. Add explicit throwing-callback cases; test no partial public readiness and no promise to undo external callback effects. |
| Ordinary dependency graph | Test independent child retention and normal unload, failed cyclic group cleanup, pre-existing resident preservation, public reentry rejection, same-path retry and resource retirement. |
| Private preparation / reload | Keep candidate abort and old graph/resource validity tests; do not infer ordinary residency rollback from replacement ownership. |

#### Error boundary inventory

| Boundary | Decision and consumer-based reason |
| --- | --- |
| Package object load wrapper and `FAssetResult::PackageObjectLoadCause` | Remove. Reuse `FAssetResult` code/message for field application. The linker only adapts the wrapper; remaining field consumers assert representation in tests. Format object, Archive route, dependency, expected/actual and original reason before cleanup. |
| Resolver operation embedded in field load | Flatten immediately to code/message. Resolver operation id/disposition is not a field-load recovery protocol; update retention tests to assert durable context and independent requests. |
| Asset codec reader/writer and Capture cause adapters | Remove cause links from asset results; adapters already format them. Keep codec reason/offset and Capture reason-to-save-code mapping at their owning boundaries; flatten nested formatting-only causes there in Stage 3. |
| Asset graph validation, Registry, bulk storage and registration causes | Flatten at asset adaptation; source inspection found assignment/formatting propagation, not asset-level recovery branches. Preserve native validation/registration status needed by their owning APIs. |
| Private graph preparation and reload diagnostics | Keep status, stage, package/object identity, commit/abort and resource receipts. Replace embedded complete asset operations with diagnostic code/text; retain replacement-map and resource state actually used by coordinator control flow. |
| Core property snapshots, editable copy and graph operations | Keep error classification and first restoration failure as distinct outcomes. Editor property recovery and material graph edit sessions consume restoration success; flatten formatting-only nested diagnostic transport without weakening restore/abort behavior. |
| Cook input/dependency/contribution adapters | `FCookDependencyDiscovery::Fail(FAssetResult)` reads `CookInputCause` to recover cancellation/limit status: migrate to an explicit Cook-owned failure channel before removing this link. Dependency/contribution links are adapters; retain typed contributor/cancellation/provider outcomes, flatten display-only causes. |
| Save/import/editor operation adapters | Keep write disposition, operation identity, recovery location and affected files in the owning write outcome. `AssetOperationResultInternal.h` branches on disposition and copies recovery metadata; generic load failures must not become this channel. Preserve async terminal completion and cancellation. |
| Compilation and external source providers | Keep typed submission/task/provider states consumed by schedulers and completion handlers. Flatten nested causes used only by formatters; do not replace provider protocols with generic strings. |

Stage 1 acceptance targets are `AssetPackageTests` (field errors, external
resolution, cleanup/retry and private bindings), `AssetPackageReloadTests`
(private application) and an all build. Stage 2 additionally requires family
validation tests and RoadWeaver coverage. Stage 3 must search all declared source
and test roots again for each changed API and use affected-test selection plus
an all build. Audit-only documentation validation is not runtime validation.

### Stage 0: Audit consumers and fix the loading contract

Outcome: an implementation-ready ownership and state model grounded in current
code. This stage precedes removal of any rollback guarantees.

- [x] Trace ordinary load, cyclic references, reentrancy, custom serializers,
  PostLoad, resource registration, unload, private preparation, and reload.
- [x] Inventory error producers and real code/context consumers across all
  projects in `Durin.dworkspace`; distinguish behavior from tests of old layout.
- [x] Record the state transitions, completion-group algorithm, public/internal
  lookup rules, owner of each cleanup action, and callback restrictions here.
- [x] Resolve PostLoad reentrant-load ordering and exceptional-failure behavior;
  identify existing callbacks that must change before the boundary moves.
- [x] Map each former error boundary to retain, flatten, or remove, with a
  consumer-based reason; identify recovery metadata that needs a separate result.

Completion: every selected semantic change has affected consumers and concrete
acceptance cases; no unresolved ownership decision blocks Stages 1 or 2.

### Stage 1: Flatten ordinary load errors

Depends on Stage 0. Outcome: the full live/private field-load path uses compact
diagnostics without recursively embedding operation results.

- [x] Replace the package object error/result wrappers and recursive asset cause
  adapter across producers, private preparation, live loading, and callers.
- [x] Preserve first failure, codes, object/field/dependency context and original
  Archive messages; include useful expected/actual details at failure creation.
- [x] Remove obsolete formatter and retention-only tests. Test actionable errors
  after cleanup, retry, and independent requests without asserting full prose.
- [x] Pass affected tests and an all build; update authoritative contracts.

### Stage 2: Replace ordinary load transactions with scoped completion

Depends on Stage 0; integrate with Stage 1's diagnostics. Outcome: readiness and
cleanup are correct without reverting successful independent dependencies.

- [x] Introduce authoritative load phases and internal skeleton resolution;
  prevent public success for an incomplete object or package.
- [x] Implement completion/failure propagation for supported cyclic groups.
- [x] Move recoverable gates before PostLoad and migrate side-effecting callbacks;
  define and test reentrant requests and exception cleanup.
- [x] Replace root `TransactionPackages` rollback and redundant cleanup paths
  with explicit incomplete-group ownership; preserve existing resident packages.
- [x] Verify failure removes temporary discoverability/resources and allows retry;
  keep independent successful dependencies usable and normally unloadable.
- [x] Preserve private replacement abort and old live graph validity; update
  explicit dependency scopes without silently changing their operation contracts.
- [x] Pass affected tests and an all build; update load/lifetime contracts.

### Stage 3: Consolidate asset and object result boundaries

Depends on Stages 1 and 2. Outcome: obsolete typed-error architecture and
compatibility adapters no longer dictate asset subsystem design.

Implementation boundary decisions (2026-09-18): generic `FAssetResult` no longer
embeds typed causes. Existing save/import APIs compose `FAssetWriteOutcome` as
`WriteOutcome` so recovery metadata has one explicit owner without duplicating
all async/public result APIs. Ordinary field-load adaptation never copies it.
Cook discovery owns the first input failure and terminal status directly; the
coordinator copies that Cook-owned record to `InputDiagnostic`.

Codec, Capture, snapshots, editable copy, graph persistence/replacement, private
preparation and reload keep their domain code/reason and context, but materialize
display-only nested failures immediately. Editable-copy restoration failure
remains a separate optional outcome. Material diagnostics, compilation receipts,
Cook codec/publication protocols, source-provider statuses and editor job states
remain owned by those operations: removing their protocols is not part of
ordinary asset diagnostic consolidation. None is embedded back into a generic
asset result. Compiler registration formats startup failures immediately; import
validation no longer carries a module-owned polymorphic cause. Resource ownership,
cancellation and commit/abort are unchanged.

- [x] Execute Stage 0's retain/flatten/remove inventory across codec, Capture,
  property/graph operations, asset services, Cook, compilation, import, and UI.
- [x] Separate actionable write/recovery outcomes from ordinary diagnostics;
  preserve cancellation, async completion, and external provider semantics.
- [x] Remove obsolete cause members, headers, formatters, adapters, and old
  contract prose. Keep genuinely consumed typed status without a universal
  error registry, recursive result hierarchy, or mandatory formatting framework.
- [x] Migrate all workspace consumers and pass affected tests and an all build.
- [x] Document implemented contracts and close this plan only after all gates pass.

## Validation and Handoff

User clarification (2026-09-18): PostLoad is intrinsically non-transactional.
Cleanup must not imply compensation of its external effects. Tests should assert
classification, output/lifetime state and retry behavior, not diagnostic wording;
review diagnostic completeness at the implementation boundary instead.

Follow [native testing](../Agents/Testing.md) and
[build guidance](../Agents/BuildAndRun.md). Shared Engine API changes require an
all build and affected targets across Engine, Sandbox, and RoadWeaver. Run builds
serially and retain evidence per completed stage.

Required behavioral cases include truncated/invalid fields with useful context;
external resolution failure; failure followed by retry at the same path;
independent dependency success followed by root failure; a failing cyclic group;
pre-existing resident dependency preservation; rejection of premature public
success; PostLoad ordering/reentrancy; exception-safe cleanup; resource retirement;
normal unload of retained dependencies; and failed replacement preserving the
old live graph. Test semantics rather than the shape of diagnostic wrappers.

Update [asset packages](../Runtime/Assets/AssetPackages.md),
[serialization](../Runtime/Core/Serialization.md), and other affected owning
contracts only as behavior is implemented. Update this plan in the same commit
as each validated implementation batch, using its exact Plan and Stage trailers.
Plan replacement alone requires changed-document and all-plan validation, not
native builds; it must not mark implementation stages complete.
