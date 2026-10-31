# UE-Aligned Override State and Object Loading Plan

Summary: Align property traversal and override-state interfaces with UE responsibilities while eliminating redundant object captures during validated package loading.

Last reviewed: 2026-10-31

Status: Active
Completed:

## Current Status

Planning only. No implementation stage in this plan has started. Existing
startup batch budgets, on-demand post-processing preparation, and detailed
object-load timing are prerequisites already implemented, not unfinished work
in this plan. All checklists below remain open until their evidence exists.

The selected first optimization is to restore complete-field override intent
from validated load traversal, rather than recapture each loaded object twice.
UE-style names must represent equivalent responsibilities; renaming the current
ledger alone does not satisfy this plan. Broader UE container-operation semantics
are outside this iteration.

### Measured baseline

Instrumentation revision: `b14b142b2c28f5bfbade8268cb4e576bc3e206ed`.
One diagnostic Win64 Debug editor startup loaded Sandbox
`/Game/Levels/GrayboxStage15`, using `--exit-after-ticks=3`.
Raw local artifacts are `Build/ObjectLoadTiming/final.log` and
`Build/ObjectLoadTiming/report.json`; they are ignored build artifacts, not
portable committed fixtures. Preserve or upload them before cleaning the build
tree. This single run locates costs; it is not a statistical performance baseline.

| Measurement | Time / count |
| --- | --- |
| Entire object application, including dependencies | 212.942 ms |
| Root package own time | 117.585 ms |
| Dependency package own time, combined | 95.350 ms |
| Packages / serialized exports / top-level property tags | 6 / 28 / 167 |
| ApplyOverrideLedger, additive exclusive time | 68.592 ms |
| SerializeExports, additive exclusive time | 40.950 ms |
| DecodePackage, additive exclusive time | 29.101 ms |
| ValidateSchema, additive exclusive time | 19.027 ms |
| InspectBulk, additive exclusive time | 18.707 ms |
| CollectCompatibilityEvidence, additive exclusive time | 10.610 ms |
| BuildOverrideLedger, additive exclusive time | 4.901 ms |
| PostLoad, additive exclusive time | 2.444 ms |

Only `exclusive_ms` is additive across nested phases. `self_ms` removes nested
dependency-package time but still includes same-package children. The recorded
exclusive sum is 212.930 ms, leaving 0.012 ms outside the measured phase tree.
Do not sum inclusive phase totals or interpret visit counts as unique fields.

`ReplaceAuthoredOverrides` currently calls `ValidateAuthoredOverrideEntries`,
which captures Discovery and AuthoredPackage logical value trees, compares their
shape, and validates paths against captured values before publication. This
explains a concrete source of repeated work, but the entire 68.592 ms cannot be
claimed as removable: normalization, validation, allocation, and publication
still have to happen. Object creation and PostLoad are not the dominant measured
costs.

## Goal

1. Provide UE-style serialized-property chains, transient visitor paths,
   per-object override state, and a manager facade with precise Durin contracts.
2. Preserve existing serialized content, explicit user intent, compatibility
   diagnostics, and transactional load behavior.
3. Remove full logical-value recaptures from eligible package-load restoration;
   retain strict validation for general callers and unproven custom serializers.
4. Reuse load-local field bindings across schema checks, serialization,
   compatibility evidence, and override restoration where their contexts match.
5. Prove the improvement with work counters, semantic tests, and comparable
   editor-startup measurements rather than shifting cost beyond Ready.

## Scope and Ownership

| Owner | Responsibility / principal starting points |
| --- | --- |
| CoreDObject | `DObject/Archive.h`, `AuthoredOverrideLedger.h`, `AuthoredOverrideLedger.cpp`, `DefaultDeltaPlan.cpp`; traversal paths, validation contracts, immutable object state, public facade |
| Engine | `AssetPackageLinkerSchema.h`, `AssetPackageLoadArchive.cpp`, `AssetPackageLinkerLoader.cpp`; package bindings, compatibility routing, validated restore context, publication integration |
| Editor and other consumers | Override editing/querying, transactions, duplication, reload, save/cook, and custom serializer consumers discovered by symbol search |
| Native tests | Owning CoreDObject and Engine suites, asset package/reload tests, affected editor and project consumers |

CoreDObject must remain independent of Engine package formats. Engine owns tag
interpretation and version handling; CoreDObject owns generic state and the
validated publication mechanism. Existing public-header include and module
dependency contracts still apply.

Search every source and test root of all projects in `Durin.dworkspace`
(Engine, Sandbox, RoadWeaver) before changing shared APIs. Record discovered
consumers and their validation targets in the relevant stage evidence.

This iteration does not redesign GPU startup policy, move live object graph
mutation onto worker threads, introduce unversioned editor package loading, or
implement UE's full experimental override feature set. Sharing parsed bulk/decode
metadata is a follow-up only if final measurements justify a separate plan.

## Selected Design

### Semantic compatibility before interface migration

The authoritative current behavior is documented in
[Serialization: authored override intent](../Runtime/ObjectSystem/Serialization.md#authored-override-intent)
and [property compatibility routing](../Runtime/ObjectSystem/PropertyReflection.md#serialized-property-aliases).
The first release retains those semantics and the current package format.

| Situation | Required behavior in this iteration |
| --- | --- |
| Ordinary loaded value / Explicit tag | Restore value without creating persistent override intent |
| Forced field equal to its default | Preserve complete-field replacement intent through save, load, duplication, and default changes |
| Forced provenance versus operation | Keep authoring provenance separate from the effective replacement operation; do not cast one enum into the other |
| Parent replacement with a child query | Report the effective replacement and whether it is inherited from an ancestor; do not manufacture a stored child entry |
| Nested Struct replacement | Preserve its boundary; emitted ancestor Explicit tags are not independent replacements |
| Array, fixed array, Map indexed route | Validate actual index/key, then normalize to complete owning-container replacement; no persistent per-element operation |
| Clear / subtree clear / reset | Change intent only; clearing a child cannot create an exception inside a replaced parent |
| Duplicate input paths | Reject transactionally before normalization/coalescing, matching current behavior |
| Alias / deprecated / removed field | Canonicalize aliases, preserve typed compatibility routing and evidence; migration code explicitly transfers intent from deprecated fields |
| NoDelta / cooked loading | Complete emission does not invent intent; cooked runtime loads do not restore authored state |

UE's `Modified`, `Add`, `Remove`, and `SubObjectsShadowing` operations are not
automatically supported by exposing similar enum values. Stage 0 must define the
supported operation/query result model, including structural ancestors versus
effective inherited replacement. Unsupported mutation operations must be absent
or return an explicit typed unsupported result. Do not represent an ordinary
value difference as an authored operation.

### Transient traversal and durable identity are separate

- Introduce `FArchiveSerializedPropertyChain` and
  `FArchive::GetSerializedPropertyChain`-style access at Durin's object-aware
  archive layer. The exact owning class/signature is finalized in Stage 0:
  primitive byte archives must not acquire a CoreDObject dependency.
- Maintain the chain through existing object, field, fixed-array, array, and Map
  enter/leave scopes. Inspection must avoid allocating/copying a vector on every
  field. Scope unwinding must restore the chain after errors and exceptions.
- Introduce `FPropertyVisitorPath`-style transient queries for serialization and
  editor callers. Reflected segments may use scoped `FProperty` bindings;
  native custom fields retain `FArchiveFieldDescriptor` logical identity when no
  reflected property exists. Do not manufacture reflection objects for them.
- Keep persisted paths pointer-free and canonical: declaring type, current field
  name, and the existing validated token representation. Transient container
  selectors are not a promise of stable persistent element identity.
- Never persist a property pointer or let a traversal view outlive its owning
  archive/metadata snapshot. Reject stale bindings after reflection replacement
  or reload; the manager stores no process-global raw-property cache.

### Override state and manager facade

Introduce `FOverriddenPropertySet` for per-object state and
`FOverridableManager` as the entry point for query, override, clear/reset, and
restoration responsibilities. Method families should follow UE where their
semantics match, including `GetOverriddenPropertyOperation`, `OverrideProperty`,
`ClearOverriddenProperty`, and `RestoreOverriddenPropertyOperation`-style
restoration. These are target responsibilities, not a claim of ABI or exact
signature compatibility with UE.

The set preserves lazy allocation, immutable shared snapshots, pointer-free
durable identities, and atomic replacement. General edit APIs retain full input
validation. Load restoration is a distinct restricted protocol that consumes
validated traversal evidence, not a public `bTrusted` or `bSkipValidation` flag.
No state becomes visible before its complete validation succeeds. Preserve
existing reader/thread ownership rules; atomic snapshots do not authorize
concurrent live-object edits.

Existing DObject entry points may temporarily forward into the manager. Avoid
dual writable state or two independent semantic implementations. Migrate all
consumers and remove transitional entry points only after the consumer inventory
and tests are complete. The existing ledger may initially be the set's internal
representation; rename storage only when it helps the final contract.

### Load-local bindings and restoration

Create one bounded load binding context keyed by serialized schema identity,
declaring type, and the current reflection snapshot. Resolve current names,
aliases, deprecated routes, removed fields, logical kind/signature, and optional
`FProperty` once per distinct schema/field binding. Reuse the same validated
result in compatible consumers. Consumed-field bits, container positions, and
object-specific facts remain per traversal/object and cannot be cached as schema
facts. Keep present ambiguity rejection, limits, and detailed diagnostics.

During successful `FPackageLoadArchive` traversal, record only the facts needed
to validate replacement boundaries: canonical field identity, type/route,
container bounds or canonical key checks where required, and validated scope
completion. Avoid retaining full values, bulk bytes, or unbounded observations
for all fields merely to replace one capture tree with another.

For reflected traversal, build candidate operations from Forced tags and the
observed canonical routes. Normalize/coalesce, validate, and atomically restore
through the set at the existing loader boundary. Do not infer overrides from
every visited field, default differences, or structural Explicit ancestors.

The existing Discovery/AuthoredPackage shape invariant also detects custom
serializer inconsistencies. Observation of a successful load alone does not
prove this invariant. Stage 0 must choose an explicit eligibility proof for the
initial reflected path; native/custom serializers without equivalent evidence
retain the existing validator. Cover data-dependent serializers, inherited base
walks, omitted/duplicate fields, and purpose-dependent shape differences. Never
cache a type-level proof when validity depends on an instance's values.

Candidate restoration remains load-scoped. Respect reference fixup, SCC graph
validation, compatibility callbacks, PostDeserialize/PostLoad, migration-owned
intent edits, notifications, cancellation, and rollback ordering. Record the
actual ordering before changing it. If a later callback invalidates observations,
revalidate the affected routes or use strict fallback; do not publish stale
observations. Failure must leave prior published state intact and release load
contexts. Load evidence cannot escape as a reusable validation capability.

### Instrumentation and attribution

Preserve the existing
[object-load timing contract](../Runtime/Assets/Packages/AssetDataLifecycle.md#object-load-timing).
Add bounded counters for binding builds/reuse, restoration path visits,
fast-path/fallback objects, fallback reasons, and full captures attributable to
restoration. Separate observation, validation/normalization, and publication
time if needed; retain aggregate comparability with the old ApplyOverrideLedger
and BuildOverrideLedger boundaries. Counter collection must not emit per-field
logs or perform a second object walk.

## Implementation Stages

### Stage 0: Freeze semantic and API contracts

Dependencies: current timing implementation and existing serialization contracts.

- [ ] Inventory all override, traversal, load/restore, save/cook, transaction,
  duplication, and reload consumers across the three workspace projects.
- [ ] Pin the UE reference revision/API surface used for alignment; record
  intentional differences and experimental API limitations.
- [ ] Define operation/query semantics, inherited-result reporting, unsupported
  operations, native-field path representation, and metadata lifetime rules.
- [ ] Trace current restore/fixup/callback/publication ordering and select a
  reflected-path eligibility proof that preserves the shape invariant.
- [ ] Add focused semantic regression fixtures for the matrix above, including
  currently supported malformed-route rejection and rollback behavior.
- [ ] Capture at least three comparable pre-change startup samples at the
  instrumented implementation base; record settings and artifact locations.

Completion: reviewed design decisions resolve the choices above; baseline
semantic tests pass and raw baseline samples are retrievable. Record actual
target selection and fixture coverage before proceeding.

### Stage 1: Add traversal chains and transient visitor paths

Dependencies: Stage 0 contracts.

- [ ] Implement scoped serialized-property chains and non-allocating access.
- [ ] Add visitor-path construction/query and durable canonical conversion,
  preserving logical descriptors for native fields.
- [ ] Integrate existing archive callbacks without extra Serialize invocations.
- [ ] Test nested Struct/container traversal, inherited fields, canonical Map
  keys, missing reflection bindings, stale-view boundaries, and unwind cleanup.

Completion: existing archive behavior and diagnostics remain equivalent; chains
are balanced on success/failure, and steady traversal adds no per-field heap
allocation. Validate the owning module and discovered affected consumers.

### Stage 2: Introduce override set and manager interfaces

Dependencies: Stage 1 paths and Stage 0 semantic tests.

- [ ] Implement the set and manager with one underlying immutable state source.
- [ ] Implement effective-operation queries with ancestor/inherited reporting,
  validated edit/clear/reset, and restricted transactional restore staging.
- [ ] Forward existing entry points temporarily and migrate runtime/editor
  consumers, including transaction, duplication, and default-relative planning.
- [ ] Test equal-default Forced values, nested boundaries, indexed normalization,
  duplicate rejection, template rejection, empty/lazy state, snapshot readers,
  failures, and default changes.

Completion: semantic tests pass through the new facade; all workspace consumers
are accounted for. Saved content and cooked behavior remain unchanged. No new
unchecked mutation route or second authoritative state exists.

### Stage 3: Share load-local field bindings

Dependencies: Stage 1; may be implemented before Stage 2 completes, but changes
in this checkout remain sequential under the single-writer rule.

- [ ] Introduce scoped schema/reflection bindings with bounded ownership and
  no global property-pointer cache.
- [ ] Reuse bindings in schema validation, field application, compatibility
  evidence, and candidate override construction where contexts are equivalent.
- [ ] Preserve per-instance consumed bits, validation limits, typed errors,
  alias collision detection, deprecated routing, and removed-field policy.
- [ ] Test shared schemas across objects, different archive/filter contexts,
  schema rejection, reload invalidation, and native serializer fallbacks.

Completion: counters show one resolution per distinct eligible binding rather
than repeated consumer lookups; compatibility diagnostics and loaded values are
unchanged. Existing relevant package tests and new binding tests pass.

### Stage 4: Restore overrides from validated load traversal

Dependencies: Stages 2 and 3; Stage 0 eligibility proof.

- [ ] Collect minimal observations and Forced candidates in the load archive.
- [ ] Implement validated normalization and atomic restore at the established
  boundary, preserving subsequent migration-owned changes and rollback.
- [ ] Route eligible reflected loads through observation-based restoration;
  retain strict capture validation with explicit reasons for unproven cases.
- [ ] Keep general edit/duplication validation strict unless separately proven;
  do not broaden eligibility merely to improve startup statistics.
- [ ] Add counters and test SCC dependencies, references, callback rejection,
  cancellation/reload failure, nested custom serialization, and malformed tags.
- [ ] Compare fast and strict results with test-only differential fixtures:
  values, canonical paths, effective operations, errors, and publication state.

Completion: eligible restoration performs zero full Discovery/AuthoredPackage
captures attributable to override restoration. Fallback cases remain correct
and visible; diagnostics and failure atomicity pass differential coverage.

### Stage 5: Finish migration and verify startup improvement

Dependencies: Stages 0-4 acceptance evidence.

- [ ] Remove redundant load-time walks and transitional API implementations
  proven unnecessary; retain strict validation wherever still required.
- [ ] Complete the cross-project symbol inventory and affected-consumer builds.
- [ ] Run the final semantic/package/reload regression selection and editor
  startup comparison described below.
- [ ] Publish lasting API, semantic, lifetime, compatibility, and load-order
  contracts in the owning runtime documents; keep only execution history here.
- [ ] Record final evidence, remaining fallback reasons, risks, and follow-up
  candidates. Close the plan only after every required gate passes.

Completion: one supported interface/state model remains, final evidence covers
the shipped inputs, and no unsupported UE operation has silently changed content
semantics or package interpretation.

## Validation and Performance Gates

Follow [build/run selection](../Agents/BuildAndRun.md) and
[native-test selection and result reuse](../Agents/Testing.md); do not copy stale
target commands into this plan. Read those workflows before execution.

Correctness coverage must include save/load/resave and duplication intent;
NoDelta/cooked behavior; aliases/deprecated conversion and intent transfer;
current/removed/incompatible schemas; container bounds and canonical keys;
custom serializers and shape mismatch; failure atomicity; callback ordering;
dependency cycles; reload and existing transaction consumers. Run the owning
CoreDObject tests, relevant Engine asset/package/reload tests, and affected editor
or project tests selected from the consumer inventory. Prior timing-stage
validation (207 asset-package tests plus 2 timing tests) is baseline evidence,
not validation of the new APIs.

Build the owning modules and affected consumer targets in Engine, Sandbox, and
RoadWeaver where the inventory identifies use. Because this plan may change
foundational public archive/object headers, Stage 0 must assess whether bounded
coverage can reliably cover ABI/include impact. If it cannot, require an `all`
build once at final integration and record the concrete uncovered risk before
running it. Do not repeat broad builds at each stage. A platform-specific path
must either be validated on that platform or recorded as an unresolved gate;
passing Win64 tests do not establish cross-platform coverage.

For timing, run at least three valid samples before and three after with the
same machine, build configuration, project/map, startup exit condition, cache
state, GPU policy, and timing instrumentation. Use warm caches for the primary
comparison; report cold starts separately if collected. A stale historical
binary or changed configuration requires a fresh baseline. Record revision,
settings, package/object/property counts, warnings/errors, raw logs, summary
script/output, and medians/ranges under ignored `Build/` or durable CI artifacts.

Report object application wall time, root own/dependency own time, additive
phase totals, restore captures/fallbacks, first shell frame, HostReady, and
post-commit readiness tail. Keep diagnostic overhead equivalent between arms.
Do not compare the sum of nested phase times or count deferred work as savings.

Acceptance requires semantic equivalence, zero redundant restoration captures
for eligible objects, and a repeatable reduction in restoration and total
object-application time. First shell frame and HostReady must show no repeatable
regression beyond observed run variability. If distributions overlap or a
regression appears, investigate and collect more samples before claiming a win;
report inconclusive results explicitly. There is no precommitted millisecond
saving based on the single 68.592 ms observation.

For documentation, run changed-document validation and all-plan validation when
adding or completing this plan, as required by the
[plan lifecycle rules](AGENTS.md). Plan authoring itself requires no native build.
Each implementation commit updates stage evidence and uses exact `Plan` and
`Stage` trailers per the repository handoff rules; creating this unexecuted plan
does not establish completion of Stage 0.

## Rollout, Compatibility, and Recovery

The initial rollout reads and writes the current package format. Existing
Forced tags remain conservative complete replacements, including historical
content whose original intent cannot be reconstructed. No automatic bulk asset
rewrite, enum reinterpretation, or version bump is part of this plan.

Keep the strict restoration implementation available for genuinely unproven
serializer paths, not as an undocumented permissive bypass. If observation-based
restoration fails to prove validity, fall back before publication when the
original route remains applicable; otherwise reject with its typed cause. Do not
retry a partially published mutation. Test-only differential mode must not add
duplicate production work.

Any future support for persistent element operations, richer provenance,
subobject shadowing, or changed inheritance needs a separate semantic/format
decision with versioned readers, migration fixtures, save/resave rules, and
rollback policy. Update this plan before expanding scope. After final profiling,
bulk metadata reuse or worker-side pure parsing may become separate plans;
live-object creation, mutation, and PostLoad threading require their own safety
contracts.

## UE Design References

These primary sources guide responsibility and naming, not binary compatibility
or a promise that Durin implements every UE feature. The public documentation
routes may track different UE versions; Stage 0 pins the actual target. Treat
overridable serialization APIs as evolving/experimental where documented.

- [FPropertyTag](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPropertyTag): tagged field metadata and override operation.
- [EOverriddenPropertyOperation](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/EOverriddenPropertyOperation): operation vocabulary; availability is not Durin support.
- [FOverriddenPropertySet](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FOverriddenPropertySet): per-object state and traversal-aware restoration.
- [FOverridableManager query](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FOverridableManager/GetOverriddenPropertyOperation): manager and visitor-path query boundary.
- [FArchiveSerializedPropertyChain](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FArchiveSerializedPropertyChain) and [GetSerializedPropertyChain](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FArchive/GetSerializedPropertyChain): transient current traversal.
- [FPropertyVisitorPath](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FPropertyVisitorPath): transient property visitation paths.
- [UStruct](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UStruct): schema ownership reference; its unversioned schema cache is not justification for bypassing tagged editor compatibility.
- [FLinkerLoad::Preload](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FLinkerLoad/Preload): object serialization responsibility within loading.

Owning Durin contracts to update after implementation:
[serialization](../Runtime/ObjectSystem/Serialization.md),
[property reflection](../Runtime/ObjectSystem/PropertyReflection.md),
[package persistence](../Runtime/ObjectSystem/PackagePersistence.md), and
[asset data lifecycle](../Runtime/Assets/Packages/AssetDataLifecycle.md).
