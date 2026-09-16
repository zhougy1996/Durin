# Package Persistence Layering Plan

Summary: Move generic package persistence into CoreDObject while preserving Engine asset publication transactions and editor save workflows.

Last reviewed: 2026-09-16

Status: Active
Completed:

## Current Status

Planning only; implementation has not started. The uncommitted
`SavePackageAsync` facade, `DPackage` forwarding members, consumer migrations,
and their tests were discarded at the user's request. The committed asynchronous
staging implementation in `63b7b16f9` remains the baseline.

Today CoreDObject owns package identity, reflection, linker tables, and canonical
DAST encoding. Engine owns live graph capture, save admission, staging, file
publication, and asset catalog coordination. `FAsyncPackageSave` prepares on the
GameThread, writes detached bytes on BlockingIO workers, and requires explicit
GameThread completion to publish. Its current contract is documented in
[Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md#asynchronous-save-staging).

## Goal

A program depending on CoreDObject and its lower-level dependencies must be able
to persist and read back an ordinary reflected package without Engine or an
initialized asset registry. `DPackage::Save()` and `DPackage::SaveAsync()` must be
real CoreDObject APIs with lower-layer parameter, result, and operation types.
The default overloads may use a configured destination resolver; an explicit
destination must be usable without the asset runtime.

Engine asset saves must retain readiness checks, dependency policy, publication
coordination, and recovery behavior while consuming the same persistence core.
Preserve the existing file format and Delta/Complete serialization semantics.

## Selected Architecture

| Owner | Responsibility |
| --- | --- |
| Core | Generic byte I/O, bounded task scheduling, temporary files, replacement and rollback primitives; no reflected objects or catalog policy |
| CoreDObject | Object graph capture, reflected property serialization, package-local reference validation, package format and bulk output, save operation lifetime, package revision checks, and package persistence APIs |
| Engine | Concrete asset preparation, asset dependency validation, residency and live-load coordination, catalog publication, multi-asset transactions, and asset recovery policy |
| AssetRegistry | Metadata discovery, queries, and rebuildable catalog projection; no dependency on Engine to implement package persistence |
| Editor modules | Batch scheduling, progress, cancellation requests, retry presentation, and editor notifications |

Dependencies point downward. CoreDObject must not include Engine headers, expose
Engine result types, or require an Engine save provider for an ordinary package.
Use narrow extension points where asset-specific behavior is needed. Do not
move the entire asset runtime behind a provider interface and call that lowering.

### Package Persistence and Asset Publication

Separate two operations explicitly:

- Package persistence captures and validates a package, writes its output files,
  and reports the file commit result.
- Asset publication coordinates that work with asset readiness, catalog state,
  and any multi-package recovery policy. Its result may report committed content
  with a pending catalog projection.

CoreDObject must return package-specific results rather than `FAssetResult`.
Engine adapts these results and retains asset transaction identifiers and
recovery dispositions. The lower layer reports sufficient file commit and
rollback information without knowing the catalog's desired state.

Expose a staged persistence primitive so Engine can validate participants before
publication and coordinate file commit with its existing transaction. A simple
post-save notification cannot replace this coordination: catalog failure can
require rollback under the existing options.

Do not mechanically replace all asset `SavePackage` calls with package members.
Callers requiring asset publication must continue through the Engine coordinator.
The new member API denotes package persistence; its header and documentation must
make this distinction explicit. Keep existing Engine free-function behavior
compatible during migration and avoid signature collisions with lower-layer APIs.

### Object and Bulk Boundaries

Move generic live graph capture and reflected value encoding alongside the
existing CoreDObject linker and format code. Distinguish serializable object
reference integrity from catalog-level asset dependency policy.

CoreDObject accepts generic bulk payload descriptions and immutable data buffers.
Engine retains texture compilation, editor source-data policy, and asset-specific
bulk preparation. Audit the existing codec, archive, and bulk helpers individually;
move generic parts and introduce adapters for policy-bearing parts.

Keep format versions, object identities, reference ordering, property defaults,
Delta/Complete output, and bulk hashes unchanged during the ownership migration.
Read-back testing may use existing CoreDObject reader/linker facilities; a general
asset loader migration is outside this plan.

### Asynchronous Contract

Preparation and object access remain on the GameThread initially. Workers consume
owned immutable snapshots and paths, never live mutable reflected objects.
Snapshot buffers remain alive until I/O and cleanup finish without full payload
copies introduced by the new interfaces.

Model preparation, staging, commit, and completion as distinct states. Report
admission failure separately from persistence completion. The public asynchronous
operation must make final completion observable; worker staging readiness alone
must not be presented as a successfully saved package.

Resolve the exact public handle/task shape in Stage 0, including how ordinary
callers and non-ticking tools drive GameThread completion. Retain an explicit
staging/commit path for Engine. Never synchronously wait on GameThread work while
blocking the GameThread that must execute it.

Specify cancellation, abandonment, shutdown draining, callback reentrancy, and
package pin ownership. Preserve revision checks before commit and clear dirty
state only for the revision actually saved. Keep distinct outcomes for failure
before publication and failure after content commit.

## Implementation Stages

### Stage 0: Freeze Contracts and Dependency Boundaries

Dependencies: none. Outcome: an executable API and migration inventory.

- [ ] Audit `AssetPackageOperations.cpp`, `AssetRuntimeFacade.cpp`,
  `PackageSerialization.h`, codec/archive/value helpers, and bulk storage helpers.
- [ ] Classify each dependency as generic persistence or asset policy; record
  target ownership and required adapters before moving code.
- [ ] Specify package save options, result and async operation types, destination
  resolution, staged commit extension points, and compatibility names.
- [ ] Define GameThread completion for editor ticks and headless tools, operation
  lifetime during shutdown, and cancellation boundaries.
- [ ] Record handling of `.dasset`/`.dbulk` replacement and failure recovery;
  do not claim a pair of filesystem renames is crash-atomic.

Acceptance: the proposed dependency graph contains no CoreDObject-to-Engine or
CoreDObject-to-AssetRegistry edge; every existing asset publication policy has an
identified owner. Record any changed decision here before implementation.

### Stage 1: Extract Generic Package Capture

Dependencies: Stage 0. Outcome: Engine delegates generic serialization to
CoreDObject while retaining its existing public save behavior.

- [ ] Move generic reflected graph capture and value serialization into
  CoreDObject and adapt Engine-specific preparation and validation.
- [ ] Define generic bulk output and snapshot ownership without payload copies.
- [ ] Add package-only round-trip coverage for object references, defaults,
  Delta/Complete modes, and external bulk; compare output with the baseline.

Acceptance: ordinary reflected package capture and read-back work without Engine
or asset registry initialization; existing asset serialization behavior passes.

### Stage 2: Implement Package Save and Async Persistence

Dependencies: Stage 1. Outcome: independently usable CoreDObject save APIs.

- [ ] Extract reusable file staging/replacement primitives into Core only where
  they have no package-format or asset-policy dependency.
- [ ] Implement `DPackage::Save()` and `DPackage::SaveAsync()` using shared
  CoreDObject persistence logic and package-specific result types.
- [ ] Support explicit destinations and the defined default resolver contract.
- [ ] Implement staged commit integration and the async lifetime/completion
  contract; preserve destination conflict checks and dirty revision semantics.
- [ ] Verify failure, rollback, cancellation, abandonment, and shutdown cleanup.

Acceptance: a target linking CoreDObject and lower dependencies saves and reads
an ordinary package synchronously and asynchronously without Engine or a catalog.
Both paths produce equivalent persisted content and accurate completion results.

### Stage 3: Reconnect Asset Publication and Migrate Consumers

Dependencies: Stage 2. Outcome: Engine uses lower-layer persistence without
weakening its asset transaction guarantees.

- [ ] Adapt `FAsyncPackageSave` and `SavePackagesAtomically` to the staged core;
  eliminate duplicate capture, staging, and file publication implementations.
- [ ] Preserve participant validation, readiness checks, registry failure
  rollback, committed-content/pending-projection results, and exactly-once callbacks.
- [ ] Keep texture import progress, cancellation, retry retention, and successful
  unload behavior consistent with final asset publication.
- [ ] Search source and test roots for every project in `Durin.dworkspace`;
  migrate consumers according to package-only versus asset publication needs.
- [ ] Retain compatibility adapters until all affected consumers are validated.

Acceptance: Engine publication and editor workflows preserve existing behavior;
independent concurrent saves, stale edits, competing writers, stage corruption,
registry failure, and abandoned operations remain covered.

### Stage 4: Validate Boundaries and Publish Contracts

Dependencies: Stage 3. Outcome: validated migration and authoritative documentation.

- [ ] Verify lower-layer headers and link dependencies in an Engine-free target.
- [ ] Run owning-module tests and affected Engine, editor, Sandbox, and RoadWeaver
  targets following [Testing](../Agents/Testing.md).
- [ ] Complete the shared Engine API `all` build following
  [Build and Run](../Agents/BuildAndRun.md).
- [ ] Update module ownership, package persistence, asset mutation, and editor
  workflow contracts; remove transitional adapters when no consumers remain.
- [ ] Record validation evidence and complete the plan only after all gates pass.

Performance benchmarks and sampling remain separate qualification runs, never
part of routine correctness tests. Do not make performance claims from correctness
test timings. Document validation follows [Documentation](../Agents/Documentation.md).

## Scope Limits

This plan does not introduce parallel bulk import, fully concurrent object graph
serialization, a new package format, or a wholesale asset loading/registry rewrite.
It preserves the existing async save optimization while correcting its ownership.

## UE Reference

The official UE 5.8 API documentation places
[UPackage::SavePackage](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UPackage/SavePackage)
and [UPackage::SaveAsync](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UPackage/SaveAsync)
in CoreUObject, [IPackageWriter](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/IPackageWriter)
in Core, and [interactive checkout/save coordination](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FEditorFileUtils/PromptForCheckoutAndSave)
in UnrealEd. SaveAsync returns a task and may still require GameThread execution.
These are ownership references, not evidence that Durin should copy UE signatures
or that UE provides Durin's existing catalog rollback semantics.
