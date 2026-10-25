# UE 5.8 Object State Foundation Refactoring Plan

Summary: Replace transaction-specific object-state and component-recovery paths with UE-aligned serialization, diffing, instance-cache, lifecycle, and replacement foundations.

Last reviewed: 2026-10-21

Status: Completed
Completed: 2026-10-21

## Current Status

Post-acceptance cleanup Stages 8-11 are complete on final native inputs based
on `e76f5a18a`. The unused cache ApplyToComponent entry point and duplicate
matching loop are deleted. Lifecycle history belongs to transaction annotations;
the reconstruction cache has no lifecycle capture mode or restore branch, and
lifecycle source identity no longer carries another Owner reference. Independently
recorded component history still uses its component annotation; both annotation
scopes delegate transitions to the component's existing lifecycle implementation.

AuthoredComponents and its public query/mutation/rebuild paths are removed.
Following UE ResetOwnedComponents, live ownership is derived from native object
ownership and the persistent InstanceComponents subset. A real two-instance
save/load regression reproduced graph-order versus serialized membership-order
mismatch; reconstruction now restores relative instance order from that sole
persistent list while retaining native/generated slots. Native and instance
persistence, duplication, absence, GC and structural history remain covered.

The built-in type-kind switch and per-derived replacement clones are replaced by
native type descriptors. Existing FDStructOps own construction, copying and
destruction inside coallocated typed storage. The small name registry resolves
native cache factories; it supplies no competing property reflection, field
serialization or struct operation table. Derived archive/copy independence,
unknown/unregistered type rejection and derived destruction are covered. The
lasting contracts are in [native construction](../Runtime/World/NativeConstruction.md).

Final Win64 Debug DurinEditor acceptance passed 1125 isolated cases in 20 targets:
EditorOperation (200), World (189), LevelMutation (55), CoreTransactionContract
(18), CoreObjectReplacement (40), AssetPackageReload (15), AssetCook (37),
EditorProperty (70), PhysicsScene (62), RendererSceneContract (63), Spline (69),
PythonReflectionIntegration (45), PythonToolIntegration (48), PythonScriptPanel
(4), MaterialGraphEditing (105), MaterialEditorInteraction (66),
MaterialEditingPersistence (13), SandboxGameplay (13), RoadSceneIntegration (5)
and RoadEditorIntegration (8). The first six targets used serial isolation;
remaining targets used four slots. EditorOperation (200), World (189) and
AssetCook (37) also passed shared-process execution. An isolated Cook case needed
the existing test-system initialization before AssetManager startup; its original
behavior assertions remain intact. Failed reproduction reports are diagnostic,
not accepted evidence. The complete matrix was refreshed after the final order
repair and FDStructOps refinement; no earlier ABI report substitutes for it.

The editor all build and bounded Debug DurinGame Sandbox/RoadWeaver closures
passed with MSVC 14.44.35207. Import inspection of Core, CoreDObject, Engine,
Sandbox and RoadWeaver runtime DLLs found no editor dependency. Rendered package
Discard passed its Vulkan qualification with Khronos validation enabled on NVIDIA
GeForce GTX 1060 6GB, API 1.4.312, driver `0x91908000`. Unchanged low-level GPU
resource replacement is compiled by all; it does not need another qualification
for this cleanup. No graphics performance or application-smoke claim is made.

Final source/report hashes, settings, UE provenance, FDStructOps dependency,
runtime imports, diagnostics and retained build/backend logs are recorded in
ignored `Build/UE58ObjectStateFoundation/cleanup-final-inputs.json` and
`cleanup-final-acceptance-transcript.txt`. Changed-document, all-plan lifecycle
and diff validation pass. Documentation-only completion edits preserve the
validated native inputs. This foundation plan is Completed; the parent transaction
plan remains Active and resumes Stage 4 Actor annotation component events, with
business migration, templates/CDO, arrays, barrier/PIE/budget and application-smoke
gates still open.

### Original Stage 7 acceptance

Stage 7 is complete on final native inputs based on `7c23031e1`. The original Stages 0-7 are accepted. The parent [transaction architecture
plan](UE58TransactionArchitecture.md) remains Active and resumes its Stage 4
Actor annotation component-event work; its business migration, template/CDO,
array scaling, barrier/PIE/budget and application-smoke gates remain open.

Final consumer validation found that Spline PostLoad could request reconstruction
before copied components had Actor ownership. Following UE 5.8.0 CL 53629095's
`UActorComponent::PostInitProperties` and `FObjectInitializer::PostConstructInit`,
DObject now dispatches a shared post-construction hook after native construction
and identity/Outer registration, before load/duplication graph callbacks. The
component override establishes native ownership; editor instance-presence
commands explicitly prepare absence before Apply. Hook failure retires the fully
constructed registered graph for normal virtual GC destruction. Registration,
initialization and Play remain separate lifecycle transitions. The lasting rules
are documented in [object initialization](../Runtime/ObjectSystem/GarbageCollection.md#object-initialization)
and [native construction](../Runtime/World/NativeConstruction.md).

The new virtual interface invalidated the initial Stage 6 ABI evidence. Fresh
Win64 Debug DurinEditor acceptance passed 1292 isolated cases across 22 targets:
CoreObject (176), CoreTransactionContract (18), CoreObjectReplacement (40),
CorePropertyChange (2), CorePropertyValueSnapshot (29), EditorOperation (198),
World (188), AssetPackageReload (15), EditorProperty (70), PhysicsScene (62),
RendererSceneContract (63), Spline (69), PythonReflectionIntegration (45),
PythonToolIntegration (48), PythonScriptPanel (4), MaterialGraphEditing (105),
MaterialEditorInteraction (66), MaterialEditingPersistence (13), SandboxGameplay
(13), RoadSceneIntegration (5), RoadEditorIntegration (8) and LevelMutation (55).
CoreTransactionContract, EditorOperation, World, CoreObjectReplacement,
AssetPackageReload and LevelMutation used serial isolation; other targets used
four slots. EditorOperation (198) and World (188) also passed shared-process
execution. All source/test roots declared in `Durin.dworkspace` were audited.

The editor `all` build passed after the ABI change. Bounded Win64 Debug DurinGame
Sandbox and RoadWeaver builds passed, covering Core/CoreDObject/Engine; import
inspection of all five runtime DLLs found no editor dependency. The toolchain
was MSVC 14.44.35207. Actual Vulkan resource replacement and rendered package
Discard each passed on NVIDIA GeForce GTX 1060 6GB, API 1.4.312, driver
`0x91908000`, with Khronos validation enabled. The latter ran its registered
qualification mode. These are correctness gates, not graphics performance claims,
and do not close the parent's application-smoke gate.

Bounded allocation checks passed with 32 observations of a 1 MiB text field:
the 1048616-byte serialized payload and transfer context share their allocations
with the observation, old storage expires after replacement, and retained
observation accounting stays at 1573980 bytes. A 1024-element array with 32
snapshots retains one pending record: ordinary/final history is 18032 bytes,
snapshot peak is 26233 bytes, eight Undo/Redo cycles do not grow retention, and
Reset releases history. Full array-range scaling stays in transaction Stage 7.
The interrupted oversized diagnostic sample is recorded as discarded, not passed.

Final native hashes, exact report hashes/counts, memory properties, selection,
UE source provenance, runtime imports and build logs are retained in ignored
`Build/UE58ObjectStateFoundation/stage7-final-inputs.json` and
`stage7-runtime-imports.json`. Initial failing reports are retained as diagnostics;
final reports supersede them. Documentation-only acceptance edits do not change
the tested native inputs. Changed-document and all-plan lifecycle validation
pass. Obsolete mixed-state, focused-record, construction-context and transaction
endpoint/Tick APIs have no remaining source/test consumers.

### Earlier checkpoints

The following receipts describe their respective stages, including gates that
were still open at each historical checkpoint; Stage 7 above is final acceptance.

Stage 6 is complete on final native inputs based on `98f892ba3`, including Stage 5's `05ffb77b9` and subsequent
workspace changes. The deletion-map audit confirms that the
mixed state coordinator, focused records, construction target-set mechanism and
transaction-only endpoint/Tick wrappers have already been removed. The remaining
cleanup unifies the canonical record's duplicated capture/observation metadata
and snapshot storage, removes its redundant snapshot-presence flag and routes
default component lifecycle restoration directly through Engine's shared state.
Tests are grouped by record, event, lifecycle and replacement responsibility;
overlapping exact-reference and repeated-capture cases will be merged without
removing their behavioral assertions. The annotation snapshot-scope hook stays:
it is the deliberate Stage 0 capture-scope policy, not a temporary serializer.
Production code is reduced by 46 lines; three redundant cases are merged and
seven ordinary construction/lifecycle cases now belong to `WorldTests`. The
remaining editor cases preserve their behavior under shared fixture types rather
than duplicating reflected test classes across translation units.

Validation passed 792 isolated cases in Win64 Debug DurinEditor: CoreTransactionContract
(17), EditorOperation (197), World (188), CoreObjectReplacement (40), AssetPackageReload
(15), Spline (69), PhysicsScene (62), RendererSceneContract (63), EditorProperty (70),
PythonReflectionIntegration (45), SandboxGameplay (13), RoadSceneIntegration (5)
and RoadEditorIntegration (8). Replacement/package cases used serial isolation;
the other targets used four slots. EditorOperation (197) and World (188) also
passed in shared-process mode after regrouping. The editor `all` and Win64 Debug
DurinGame `Engine` builds passed, as did changed-document validation. The affected
selection expands to unrelated GPU/application targets; named CPU owners and
cross-project consumers cover this cleanup. Final native input hashes, report
hashes, UE 5.8.0 CL 53629095 source provenance, test moves/merges and build logs are
retained in ignored `Build/UE58ObjectStateFoundation/stage6-final-inputs.json` and
`stage6-test-ownership.json`. Final declaration-header cleanup was followed by
both consuming test targets and the editor `all` build; other relevant inputs
were unchanged. Stage 7 and the parent transaction plan's remaining business,
component-event, template and final acceptance gates stay open.

Stage 5 is complete on the final native inputs based on `de97cd787`. The integration uses UE's
`FindAndReplaceInstances` responsibility for typed component instance data and
reuses the same checked graph map for annotation and history preparation.
Durin retains its pre-publication failure boundary: typed native snapshots are
copied before replacement; collector traversal remains observational. Supported
records, primary objects, package transitions and tracked package state publish
together. Custom annotations/commands must return independent replacement data;
unsupported state retires its entire transaction. No historical load or
construction callback is permitted during preparation.
Replacement relevance must include the transfer's recorded weak-reference slots,
not only collector-owned hard edges. Within an affected transaction, unchanged
native commands move with their original ownership at publication; they do not
require a replacement hook. Only command state whose dependencies actually change
requires independent preparation. This preserves UE's unchanged-record behavior
without sharing mutable command ownership or admitting partial publication.

Final-input validation passed 795 isolated CPU cases across 13 targets in
Win64 Debug DurinEditor: EditorOperation (206), CoreObjectReplacement (40),
AssetPackageReload (15), CoreTransactionContract (18), World (181), Spline (69),
PhysicsScene (62), RendererSceneContract (63), EditorProperty (70),
PythonReflectionIntegration (45), SandboxGameplay (13), RoadSceneIntegration (5)
and RoadEditorIntegration (8). CoreObjectReplacement and AssetPackageReload used
serial isolation; the remaining targets used four slots. The editor `all` build
and Win64 Debug DurinGame `Engine` build passed. The affected-selection expansion
includes unrelated GPU coverage; these named CPU owners cover the changed
runtime/editor APIs and all three workspace projects. Native input hashes,
reports, selection rationale and build logs are recorded in ignored
`Build/UE58ObjectStateFoundation/stage5-history-final-inputs.json`; documentation
acceptance edits leave those native inputs unchanged. Stages 6-7 remain open,
including final workspace/backend gates. The transaction plan's template/CDO
admission and full reinstancing acceptance remain open.

Superseded paths are deleted in the stage that migrates their consumers. This
integration replaces blanket history retirement and removes the post-reload
checkpoint reset that erased retained history. Stage 6 audits residual adapters
and overlapping tests; it is not permission to retain replaced implementations.

Historical transfer checkpoint `de97cd787`, based on `b9277c43b`, added transfer-owned
reference locations and detached preparation of serialized captures/diffable
observations through the existing FObjectReplacementMap. Preparation must not
load historical fields into a live object or run reconstruction callbacks.
The runtime participant validates and swaps those prepared values under the
existing graph publication protocol, including complete native-owner coverage.
Exact hard expiry remains an error; weak expiry cannot resolve by name. Null
removals are admitted only for weak references. Schema-incompatible capture
targets reject preparation. Historical identity text/keys remain provenance while
live target and payload dependencies explicitly rebind. At that checkpoint,
whole-entry reload retirement remained pending record/cache integration.

Checkpoint validation: 426 isolated cases passed in Win64 Debug DurinEditor:
CoreObjectReplacement 39 (serial isolation), CoreTransactionContract 18,
EditorOperation 188 and World 181 (four slots). All 10 targets selected by
`test affected --explain` also passed in ordinary target execution. This covers
paired publication/allocation sharing, persistence retry, abort, changed inputs,
native-schema rejection, complete native-owner coverage, removed hard/weak
references, expired weak identity, invalid reference layouts, ordinary retained
captures and old-graph GC. Input hashes and reports are retained under ignored
`Build/UE58ObjectStateFoundation/stage5-transfer-inputs.json`. Instance-cache,
annotation, command and retained-history integration were still pending then.
The Win64 Debug DurinGame Engine build passed, including CoreDObject; no editor
dependency was introduced. Final-input workspace and backend gates remain in Stage 7.


Stage 4 is complete on `efadc671f`. Ordinary registration, World scene
replacement, component membership changes and historical recovery now use the
component/Tick lifecycle owners. `UnregisterComponent` follows UE's boundary:
it removes Tick/resource registration while retaining creation, initialization
and Play. EndPlay and uninitialization remain explicit teardown transitions.
The cache no longer executes its own component transition sequence.

`FComponentReregisterContext` and `ReregisterComponent` restore registration
without duplicating creation, initialization or BeginPlay. They retain timers
and explicit Tick configuration edits, respect retirement, and report callback
failures without implicitly retrying partially executed callbacks. World scene
replacement uses these scopes and resumes deferred construction only after its
operation gate settles. Tick capture/validate/restore/archive transfer belongs to
FTickFunction; the transaction Tick wrapper is deleted.

`FScopedSuspendRerunConstructionScripts` replaces both Actor transaction
replay/completion booleans and is also used by ordinary scene replacement.
Its per-Actor/runtime scope is an explicit adaptation of UE's global editor
helper: each historical Actor must consume its own captured inputs at completion.
Exception unwinding discards pending reruns, and incompatible annotations or inner
callback failures release the scope. Scene completion now repairs authored
attachment reverse links and transforms before registration callbacks. The old
order was demonstrated failing by a dedicated regression before the fix.

The native checkpoint structs are Durin data, not claimed UE types. Their fields
exist because these settings/flags are not reflected in this engine. They do not
add another property replay payload, lifecycle command interpreter or scheduler.
The responsibility audit is:

| State/effect | Authority and recovery |
| --- | --- |
| Authored fields and membership | Existing object serialization and Actor ownership APIs; generated ownership remains transient. |
| Creation, registration, initialization and Play | Component-owned capture/validation/application and existing callbacks; unregistered initialized/playing states remain representable. |
| Tick configuration and admission | Tick-owned state transfer, used by ordinary reregistration and historical recovery. |
| Authored attachment links/transforms | Scene completion before component registration; inner completion still precedes Actor reconciliation. |
| Generated roots and external attachment recovery | Shared Actor reconstruction/cache path from the prior construction revision. |
| Render proxies, physics bodies and Tick queues | Recomputed through resource/registration owners; no resource handles are copied from history. |

Final Stage 4 receipts are in
`Build/UE58ObjectStateFoundation/stage4-final-inputs.json`: 22 source/build input
hashes, ten report hashes and exact build logs. Windows MSVC Debug isolated runs
with four CPU slots passed WorldTests (181), EditorOperationTests (188), SplineTests
(69), PhysicsSceneTests (62), RendererSceneContractTests (63), SandboxGameplayTests
(13), RoadSceneIntegrationTests (5), RoadEditorIntegrationTests (8),
PythonReflectionIntegrationTests (45), and EditorPropertyTests (70): 704 cases.
Editor `all` and Game `Engine` builds passed. The broad build covers public header,
Actor layout, Scene virtual dispatch and source-discovery changes across all
workspace projects. Named CPU contracts cover the changed lifecycle/resource
paths without expanding `affected` into unrelated GPU/application tests.
No GPU qualification or application-hosted execution is claimed.

`stage4-ue-lifecycle-source.json` pins the four installed UE 5.8 lifecycle source
files inspected for this stage. `stage4-final-consumers.txt` records workspace
consumers; `stage4-deletion-audit.txt` records zero old Tick-wrapper/Actor-flag
references across all source/test roots. Native inputs remain unchanged after
validation; later edits are documentation only. The enduring contract is in
[Native Actor Construction](../Runtime/World/NativeConstruction.md).
Stages 5-7 remain open; this is not final foundation acceptance.

The completed construction revision on `4389b4220` supersedes Stage 0's keyed desired-set
commit decision and Stage 3's candidate hydration order. Stage 4 has removed `FActorConstructionContext` entirely. Actor owns `RerunConstructionScripts`,
`DestroyConstructedComponents`, `ExecuteConstruction` and `OnConstruction`:
capture and validate shared data, destroy the previous generated components,
create and publish new components directly, apply instance data after construction,
and restore root/attachment relationships. Construction failure does not restore
the previous generated generation. Callers must resolve new component identities.
`AddComponentByClass` and `FinishAddComponent` provide immediate ownership and
ordinary creation/registration APIs, including deferred finish for configuration.
Durin retains stable generated keys solely for instance matching because it has
no Blueprint/SCS component templates. The default AddComponentByClass call uses
exact class and per-class creation order; domain producers may supply stable keys. Native `OnConstruction` is the supported
construction hook; the post-user-construction cache phase follows that hook.
No Blueprint interpreter or atomic target-set publication is introduced.
The Python consumer audit also exposed unconditional Actor annotations keeping
rejected edits in history. UE's ActorEditor.cpp CreateIfRequired/HasInstanceData
pattern is adopted: omit empty/default supplemental state and restore default
Tick state through the existing Tick API when the historical annotation is empty.
Tests retain the rejection/no-history assertions and cover a default-to-custom
Tick Undo/Redo transition. The Tick state wrapper itself remains Stage 4 work.
Engine, Sandbox and RoadWeaver consumers are migrated. Identity-reuse and
atomicity tests now assert destruction/recreation, callback-visible ownership,
stable source matching and failure cleanup. BeginPlay requests wait for settled
Actor lifecycle state before teardown. Retired components release their names;
Rename now respects the existing package-dirty suppression scope.

Final construction-revision evidence is recorded in
`Build/UE58ObjectStateFoundation/construction-revision-inputs.json`: 31 source/build
input hashes, 13 report hashes, and exact build logs. Windows MSVC Debug isolated
runs with four CPU slots passed WorldTests (178), EditorOperationTests (180),
SplineTests (69), RoadSceneIntegrationTests (5), PhysicsSceneTests (62),
RendererSceneContractTests (63), SandboxGameplayTests (13), RoadEditorIntegrationTests
(8), PythonReflectionIntegrationTests (45), CoreObjectTests (174),
CoreTransactionContractTests (18), and EditorPropertyTests (70): 885 Editor cases.
The Game CoreTransactionContractTests add 18 passing cases, for 903 total.
Editor `all` and Game `Engine` builds passed. The broad build covers Actor and
SceneComponent ABI/header changes and every workspace project; named CPU tests
avoid the unrelated GPU/application selections in `affected --explain`.
No GPU qualification or application-hosted execution is claimed.

Two existing CoreObjectTests needed explicit object-system/GameThread initialization
to run alone; their assertions remain unchanged. Source/build inputs were frozen
before final validation; the added fixture initialization was validated by its own
complete isolated target. Subsequent edits are documentation only. Consumer and
zero-obsolete-API audits are in `construction-final-consumers.txt` and
`construction-deletion-audit.txt`. The enduring contract is documented in
[Native Actor Construction](../Runtime/World/NativeConstruction.md).

That earlier receipt closed only the construction revision task. The lifecycle
receipt above now closes Stage 4; Stages 5-7 acceptance remains open.


Stage 0 is complete against `d078bbdf09eed3fe37f53a8dbc6c29f38db97d24` and the
installed UE 5.8.0 CL 53629095 source. Stages 1-4 are complete; Stages 5-7 remain
open. The construction revision baseline is `4389b4220`. The user authorizes substantial internal refactoring and
prefers UE-aligned underlying capabilities over additional transaction-specific
mechanisms. Existing production behavior and useful regression coverage must
survive the migration; internal type names and signatures need not be preserved.

The [transaction alignment plan](UE58TransactionArchitecture.md) has completed
Stages 0-3 and has a validated Stage 4 event/snapshot checkpoint. That evidence
describes the implementation at those revisions, not acceptance of the refactored
foundation. Its remaining functional and final-acceptance gates remain required.
This plan owns foundational replacement; it does not establish another history.

The Stage 0 decisions below are the implementation handoff. The ignored receipts
are `Build/UE58ObjectStateFoundation/ue-source-fingerprints.json` (19 files),
`ue-call-inventory.txt`, and `durin-call-inventory.txt`. They contain source
identities and symbol locations, not copied UE source. No runtime acceptance is
claimed by this source audit.

Historical Stage 3 checkpoint on top of `d3441ade2` (construction mechanics
superseded by the Stage 4 revision above): ordinary and transaction
reconstruction consume the same Engine component-instance cache and shared
root/attachment recovery data. Actor annotations compose this data rather than
own an alternative reconstruction implementation. The old component endpoint and
transaction-only construction annotation pointer are removed.
The minimum data scope is component-owned supplemental overrides (initially
Generated scene relative poses), shared root/attachment recovery, and settled
native lifecycle state. Authored reflected component values remain solely in
object records. Equal class-default scene poses are omitted through the existing
default graph comparison; classes deliberately lacking a CDO retain explicit
native pose data instead of inferring an archetype. Domain cache hooks can extend
that scope without introducing another generic property undo representation.

The annotation refresh decision is refined after consumer inspection: preserve
a thin `CreateSnapshot` adapter that passes the previous shared root capture scope.
The object factory cannot rediscover authored children after their generated
parent is removed; deleting that adapter loses the independently serialized
child's final pose. Scope acquisition, validation and restore now belong to the
shared root data, also consumed by ordinary reconstruction. The adapter owns no
separate reconstruction algorithm and does not require history state on Actor.
The lifecycle transition implementation and Tick wrapper remain Stage 4 work.
The prior checkpoint used Context-local assignment tracking, candidate hydration
and keyed reuse; those details are historical evidence only and are removed by
the current destruction/recreation revision.

Final Stage 3 validation is recorded in
`Build/UE58ObjectStateFoundation/stage3-final-inputs.json`: 16 source/build input
hashes, final report hashes and build logs. Windows MSVC Debug isolated runs with
four CPU scheduling slots passed WorldTests (177), EditorOperationTests (179),
SplineTests (69), PhysicsSceneTests (62), RendererSceneContractTests (63),
SandboxGameplayTests (13), and RoadEditorIntegrationTests (8): 571 cases. The
`Win64-Debug-DurinEditor` `all` build and `Win64-Debug-DurinGame` Engine build
passed. These named selections cover native construction, historical completion,
render/physics effects and consumers in every workspace project. `affected`
expands shared Engine changes into unrelated tests; no GPU qualification or
application-hosted execution is claimed by these CPU contracts.

Regression coverage includes omitted/default poses, same-value native assignment,
constructor defaults and attachment before callbacks, ordinary recreation and
Undo/Redo of generated roots and external children, GC release of old generations,
retired/foreign/same-key different-class candidate rejection, and existing failure
and cancellation behavior. Two existing isolated fixtures needed initialization:
the SplineMesh PIE case starts the asset compiler after object/scheduler setup,
and the planar-mesh bounds case initializes the object system. Assertions and
production thread/service checks remain unchanged. Inline cache/root storage is
counted once in annotation accounting.

The final source/build inputs remain unchanged after validation; subsequent edits
are documentation only. Workspace consumer/deletion audits are in
`stage3-final-consumers.txt` and `stage3-deletion-audit.txt`. Stages 4-7, including
shared lifecycle completion, replacement, retirement of remaining wrappers and
final runtime/allocation acceptance, remain open.

Stage 2 on top of `59fc5efbd` removes the mixed runtime object-state type.
CoreDObject now owns `FDiffableObject`, `FSerializedTaggedData` and shared
`DiffUtil::GetDiffableObject/GenerateObjectDiff`; immutable payload and compatibility
allocations are shared with transfer output from the same Serialize pass. Tags
and comparison belong to observation, while annotations and compensation belong
to the canonical editor record. Its private editor `FSerializedObject` follows
UE's distinct editor/runtime types. Snapshot retains observation and annotation
only, without a replay payload or mutation of the undo baseline.

Finalized events now merge last-snapshot partial field/native closure with the
original full diff, including net-zero edits pruned from history. Selected fields
accumulate and unfiltered observation dominates; unobserved net-zero fields do
not leak into closure. Original identity, live IDs, additional-change provenance,
completion order, cancellation and failure behavior remain covered. Runtime tests
now target serialization and diff primitives; annotation timing, retention and
allocation contracts belong to editor tests. Default/archetype scope is explicit:
transaction observations compare the same live ordinary object; class-default
override comparisons reuse the existing graph-aware default-delta facility in
the Engine instance-preservation work. Arbitrary archetypes and pending-kill
resurrection remain unsupported, as selected in Stage 0.

Stage 2 validation on `59fc5efbd` plus the 12 changed source/build paths is
recorded in `Build/UE58ObjectStateFoundation/stage2-final-inputs.json`, including
final source and report hashes. Windows MSVC Debug serial isolation passed
CoreTransactionContractTests in Editor and Game (18 each), EditorOperationTests
(176), EditorPropertyTests (70), EditorShellTests (61), and MaterialGraphEditingTests
(105): 448 cases. The Editor `all` build passed, covering shared PCH and
Sandbox/RoadWeaver integration. Named tests cover runtime transfer/diff and the
canonical record's property, shell and Material consumers; the renamed native
test source causes `affected --explain` to expand to unrelated Engine tests.
`stage2-deletion-audit.txt` records zero obsolete mixed-state/field API references
across all workspace source, test and build roots. Relevant inputs are unchanged
after validation; only documentation is subsequently edited. Stage 3-7 runtime,
component instance/lifecycle/replacement and final acceptance gates remain open.

Stage 1 migration on top of `dc901c9fb` now uses whole-object capture for all
generic history, including the property editor's pre-edit fallback. Focused
records, member locators, detached promotion overrides, incremental record updates
and their public snapshot helpers are removed. Material graph commands retain
only their domain property payloads and call the existing property capture/restore
operations directly. Runtime capture identity is now `FTransactionObjectId`,
composed by `FSerializedObjectInfo`; delta flags belong to object changes. The
canonical editor record owns failed-load compensation. At that checkpoint the temporary runtime
state coordinator had no restore operation; Stage 2 subsequently removed it. Stage 1 validation is
complete; the earlier checkpoint receipt below is historical.

Final Stage 1 inputs relative to `dc901c9fb` and report hashes are recorded in
`Build/UE58ObjectStateFoundation/stage1-final-inputs.json`. Windows MSVC Debug
serial isolation passed CoreTransactionContractTests in Editor and Game (18 each),
CorePropertyValueSnapshotTests (29), EditorOperationTests (172),
EditorPropertyTests (70), EditorShellTests (61), and MaterialGraphEditingTests
(105). The default `Win64-Debug-DurinEditor` `all` build passed, including Sandbox
and RoadWeaver consumers. Ten property fixtures needed explicit GameThread/DObject
initialization to pass alone; the repaired target then passed all 70 isolated
cases. The shared PCH's obsolete include was removed and covered by the all build.
No property lifecycle or ContainerOps gap required another implementation.

The source/build input manifest is unchanged after these runs; subsequent edits
are documentation only. Changed-document validation and diff checks pass. This
closes Stage 1, not the final foundation acceptance: later API changes invalidate
relevant build evidence, and the remaining Game Engine/Sandbox/RoadWeaver,
render/physics and allocation gates remain in Stage 7. Stage 2 subsequently removed the
mixed state coordinator and moved range bookkeeping and event comparisons into
the diffable observation owner.

Earlier Stage 1 serialization checkpoint on top of `0551b5d68`: extracted
`FSerializedObject`, `FSerializedObjectInfo`, the transfer compatibility context,
and the sole transaction data reader/writer into CoreDObject
`TransactionSerialization.h/.cpp`. The previous state writer/reader are deleted;
the existing coordinator delegates capture/preflight/load to the shared path.
The new payload does not own annotations, diffing or compensation. Transfer uses
the existing object archive, reflected lifecycle/container operations and native
Serialize; canonical name encoding, exact hard ownership, weak/soft behavior and
frozen generated-reference exclusions are preserved. Load defers GC and suppresses
package dirtying; its caller owns partial-write compensation.


Checkpoint validation: Windows MSVC Debug, with serial CTest isolation
(`--isolate --test-jobs 1 --report`), passed `CoreTransactionContractTests` in
both `Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame` (19 cases each).
`EditorOperationTests` passed 95 cases selected by
`FEngineTransactionAnnotationTests.*:FFocusedTransactionObjectSnapshotTests.*:FPersistentObjectRefTests.*:FTransactionObjectEventTests.*:FTransBufferTests.*`.
Final source fingerprints relative to `0551b5d68` are recorded in
`Build/UE58ObjectStateFoundation/stage1-serialization-inputs.json`; reports are
under `Build/NativeTestResults/<Preset>/`. Initial sandbox compilation hit the
documented MSVC C1902; the successful runs used the same toolchain outside the
sandbox. Subsequent changes are documentation only. Changed-document validation
and diff checks pass. These bounded contracts cover the extracted transfer and
its existing editor callers; the final `all` build, remaining Game consumers,
render/physics execution and allocation acceptance remain Stage 7 gates.

Baseline implementation gaps motivating the refactor:

- `FTransactionObjectState` combines serialized payload, identity, field ranges,
  schema/version checks, annotation ownership, diff generation and compensated
  restoration. Later consumers would inherit this mixed boundary.
- Actor/component annotations currently implement substantial native lifecycle,
  attachment and reconstruction recovery themselves. There is no shared
  `FComponentInstanceDataCache` equivalent in the inspected Engine source roots.
- At the baseline, `FActorConstructionContext` staged generated components by
  stable key. The user-selected Stage 4 revision removes that mechanism in favor
  of UE Actor construction entry points. `FObjectGraphReplacement` remains the
  existing checked replacement participant facility for Stage 5.
- Archive, property value lifecycle, reference traversal and `ContainerOps`
  already exist. Foundation work must extend their actual gaps, not wrap them in
  parallel serialization, reflection, container or replacement frameworks.

## Goal

Provide UE-aligned responsibilities for transaction serialization and observation,
component instance preservation, lifecycle restoration and controlled replacement.
Make transaction records thin coordinators over those facilities. Ordinary
reconstruction and editor changes must use the same underlying paths as Undo/Redo.

Success requires both working behavior and removal of superseded production paths.
A new UE-shaped class layered over an unchanged alternative implementation does
not satisfy this plan.

## Reference And Scope

Use the lawful local UE 5.8.0 CL 53629095 installation, branch
`++UE5+Release-5.8`, at `E:\Programs\Epic Games\UE_5.8`.
The [pinned source identity and behavior matrix](UE58TransactionArchitecture.md#reference-identity-and-reproduction)
owns the existing fingerprints. Record additional source fingerprints and bounded
inventories under ignored `Build/UE58ObjectStateFoundation/`; do not vendor UE source.

Stage 0 must inspect these primary source locations and their consumers:

- Core `Misc/ITransaction.h` and `Misc/Change.h`.
- CoreUObject `Misc/TransactionCommon.h/.cpp`,
  `Misc/ITransactionObjectAnnotation.h`, `Misc/TransactionObjectEvent.h`, and
  the persistent object reference implementation used by those files.
- UnrealEd `Classes/Editor/Transactor.h`, `Classes/Editor/TransBuffer.h` and
  `Private/EditorTransaction.cpp`.
- Engine `Public/ComponentInstanceDataCache.h`, its implementation,
  `Public/ActorTransactionAnnotation.h`, `Public/ComponentReregisterContext.h`,
  and Actor/ActorComponent/SceneComponent construction and registration consumers.

Align responsibilities and observable behavior, not Unreal container layout or
unavailable Blueprint subsystems. Do not implement SCS/UCS, a new archetype system,
general-purpose persistence, or a general rollback framework as incidental work.
Where default/archetype behavior is necessary, integrate the existing Durin default
graph and explicitly delimit the supported categories. Unsupported categories
remain named gaps, never silent substitutes.

## Selected Architecture

| Capability | UE reference | Durin target and owner |
| --- | --- | --- |
| Serialized transaction payload | `UE::Transaction::FSerializedObject`, data reader/writer | CoreDObject owns payload and reference/name representation; existing Archive owns transfer semantics. No events, reconstruction or compensation in this data type. |
| Captured identity | `FTransactionObjectId`, `FSerializedObjectInfo` | CoreDObject owns capture-time identity and supported metadata. No copied mutable identity registry. |
| Diffable observation | `FDiffableObject`, tagged serialized data and diff utilities | CoreDObject owns observation data and comparison. Use the same serialization/filtering rules; keep observation baselines separate from undo baselines. |
| Transaction object record | `FTransaction::FObjectRecord` | DurinEd composes payloads, observations and annotations and coordinates recording/replay. No duplicate serializer or component lifecycle implementation. |
| Component instance preservation | `FInstanceCacheDataBase`, `FActorComponentInstanceData`, `FComponentInstanceDataCache` | Engine owns reconstruction-specific instance data and matching, used by ordinary reconstruction and transactions. |
| Actor reconstruction annotation | `FActorTransactionAnnotationData`, `FActorRootComponentReconstructionData`, `FActorTransactionAnnotation` | Engine annotation holds instance-cache/root-reconstruction data and reports additional changes. It delegates restoration to construction/lifecycle facilities. |
| Registration restoration | `FComponentReregisterContext` and component registration APIs | Extend existing Engine registration and construction entry points; add scoped contexts only for a demonstrated shared call pattern. |
| Replacement and references | UE persistent reference/reinstancing behavior | Extend CoreDObject `FObjectGraphReplacement` participants and Engine instance-source matching. Keep exact identities distinct from admitted logical component identities. |
| Typed container access | UE array-record needs; Durin reflected containers | Extend property lifecycle and `ContainerOps` where required. The transaction plan owns actual array history records and their benchmarks. |

Use UE names when the responsibility matches. Do not retain a familiar name over
materially different semantics without documenting the difference. Stage 0 decides
the smallest useful subset of UE helper types; it must not instantiate every UE
type merely for naming parity.

Serialized bytes and diffable snapshots may be distinct representations when
restoration and tagged observation require them. They must share serialization
rules and reference interpretation; they need not duplicate buffers unconditionally.
Keep owned observations only for the lifetime required by snapshots/events.

## Execution And Ownership

1. Complete foundation Stages 0-2 before expanding or closing transaction Stage 4.
2. Complete foundation Stages 3-4 before broad transaction Stage 5 consumer migration.
3. Complete foundation Stage 5 before transaction Stage 6 claims replacement parity.
4. Use foundation Stages 6-7 to remove obsolete paths and validate the foundations.
   Then finish transaction Stages 4-9 against the new implementation, reusing valid
   evidence according to the repository test workflow.

This is a dependency insertion into the existing transaction effort. The original
plan still owns Label/material/script consumer behavior, template transaction
admission, array history, barriers/PIE/budget policy and final workspace acceptance.
Do not create independent checklists that can claim those same gates complete.

Each migration commit must move its owning implementation, affected consumers,
tests and status together. Search all projects in `Durin.dworkspace`: Engine,
Sandbox and RoadWeaver. Temporary adapters require a concrete remaining caller
and removal stage; they cannot become a second supported production route.
Existing in-memory history may be reset at the refactor/session boundary; no
on-disk transaction format or persisted-history migration is required.

## Stage 0 Decisions

### Source findings and supported categories

UE `TransactionCommon.h/.cpp` separates `FSerializedObject` reference/name tables
from `FDiffableObject` tagged data. `EditorTransaction.cpp` captures annotations
before object serialization, retains initial observation separately from undo
bytes, accumulates snapshot selections, and merges last-snapshot closure at
finalization. Its `Transactor.h::FSerializedObject` is an editor record helper,
distinct from the CoreUObject payload with the same unqualified name. Durin will
use the CoreUObject responsibility for its runtime payload; annotation ownership
belongs to the editor record, never that payload.

UE `ActorEditor.cpp` composes the instance cache with conditional root data and
separate component observations. Additional changes match component source info
and compare supported generated components against their archetypes when needed.
`ComponentInstanceDataCache.cpp` consumes a matched entry at most once per apply
pass. `ActorConstruction.cpp` applies the cache after SCS and UCS; ordinary
reconstruction and transaction reconstruction use that same facility.
`ComponentReregisterContext` delegates unregister/register events to the component
and tolerates retirement. These are ownership and sequencing references; Durin
has no SCS/UCS phases to copy.

Initial Durin categories are live ordinary objects, exact authored components,
and generated native components admitted by owner, exact class and existing
`FActorGeneratedComponentKey`. Diffing supports the current class default graph
for generated-instance override comparison. It does not infer arbitrary
archetypes from names. Blueprint/SCS/UCS templates, template transactions,
resurrection of expired exact identities, and cross-class instance matching are
unsupported here. Transaction Stage 6 continues to own template admission.
UE pending-kill resurrection diffs therefore have no silent Durin equivalent:
editor presence changes remain explicit and distinct from object lifetime.

### Capture decision and minimum APIs

Use whole-object `SaveObject` for undo. The production inventory finds focused
record creation in `PropertyEditing.cpp`, principally as a fallback when `Modify`
does not capture; normally it already uses whole capture. There is no measured
caller requirement that justifies another focused undo representation. Remove
focused history and promotion instead of assuming they improve cost. Preserve
property-value drafts, detached mutation values and map-key values in the property
editor, where they validate and publish edits. The Material graph's
`FFocusedTransactionObjectSnapshot` caller must migrate with its owner; its use as
a command payload is not a reason to retain a second generic undo path.

The minimum runtime surface, all below DurinEd, is:

| Owner | Selected surface and contract |
| --- | --- |
| CoreDObject | `FTransactionObjectId` and `FSerializedObjectInfo`: owned capture-time identity and metadata; split identity from delta flags in the existing event representation. |
| CoreDObject | `FSerializedObject`: owned bytes and hard-reference representation only. Names may keep the existing canonical archive encoding; an indexed UE-style name table is not required for format parity. No annotations, callbacks, diffs or compensation. |
| CoreDObject | `FSerializedObjectDataReader/Writer` on `FObjectArchive`: one transaction-purpose field filter, native `Serialize`, reference and version path. Serialization capture/validation/load helpers own schema, versions and frozen exclusion policy once, outside payload and observations. Package archive formats are unchanged. |
| CoreDObject | `FDiffableObject` and shared diff utilities: identity, flags and tagged reflected/native observations; full and selected comparison; no restore operation. Initially reuse the transfer output while capturing, without retaining an unconditional second full buffer. |
| DurinEd | `FTransactionObjectRecord`: before/inverse payload, serialization context, annotations and observations; coordinates validated replay, compensation, notifications and operation state. No property replay loop. |
| Engine | `FActorComponentInstanceSourceInfo`, `FInstanceCacheDataBase`, `FActorComponentInstanceData`, `FComponentInstanceDataCache`; component capture/apply hook, reference enumeration and matching through stable instance-source keys until template/archetype identity exists. No independent component registry. |
| Engine | Use UE cache phase names. Durin has no SCS execution; `PostUserConstructionScript` follows its native `OnConstruction` hook, and `NonConstructionScript` restores settled supplemental state. This native hook placement is an explicit adaptation. |
| Engine | `FActorTransactionAnnotationData` composes cache and conditional `FActorRootComponentReconstructionData`; component/Actor lifecycle APIs complete restoration. Scoped reregistration is added only for an actual shared caller pattern. |

Construction captures shared data before `DestroyConstructedComponents`, then
`ExecuteConstruction` invokes `OnConstruction` with direct component ownership.
`FComponentInstanceDataCache` applies after construction; construction-authored
scene fields take precedence. Root and external attachments are recovered after
component creation. Generic records retain sole authority over authored fields.
The previous target-set commit and unpublished-candidate rollback are removed.
Derived render, physics and Tick resources are updated through component APIs.

### Deviations and deletion map

| Existing implementation | Action, owner and reason |
| --- | --- |
| `FObjectArchive`, reflected traversal, `FReflectedValueStorage`, property payload mutation, `ContainerOps` | Reuse. Detached copy/commit, nested filtering and reference enumeration already exist. Extend only where migrated contracts demonstrate a missing operation; no replacement framework. |
| `FTransactionObjectState::FWriter/FReader` and anonymous transaction archive | Move to the shared runtime serialization path in Stage 1 and delete the old writer/reader. Native schema and frozen generated-reference exclusions are serialization safety policy, retained once in that layer. |
| `FTransactionObjectState` metadata, bytes, hard edges | Split into capture identity, payload and serialization context in Stage 1. Exact generation checks and hard/weak/soft distinctions are required safety policy. |
| `FTransactionObjectState::Restore/Load/Validate` | Move transfer/preflight to serialization helpers and failure compensation to the canonical replay coordinator in Stage 1. Preserve explicit recovery-required results for native exceptions or failed compensation; this is safety policy, not a general rollback subsystem. |
| `FTransactionObjectState::GetChanges`, field ranges and `SnapshotState` | Move to shared diff utilities and `FDiffableObject` in Stage 2; delete the mixed state abstraction. Annotation changes and editor presence are separate from field changes. |
| `FTransactionPropertyRecord`, property path replay, promotion, focused before/after/replay/snapshot storage | Remove in Stage 1 with property-editor and Material command consumers. Retain draft/map-key values only in their property/command owners. Existing tests migrate to behavioral assertions rather than preserving obsolete interfaces. |
| Annotation `Validate` | Retain as a preflight extension: callbacks and stale generation checks can fail in Durin. The record invokes it before mutation. It does not own a duplicate serializer or lifecycle scheduler. |
| Annotation `CreateSnapshot` | Stage 3 moves prior external-attachment scope capture into shared root data. Retain only the thin adapter passing that previous scope to the shared capture: the object hook alone cannot rediscover detached children after generated-parent removal. This is required capture-scope policy, not another reconstruction implementation. |
| `FActorConstructionContext`, `AcquireGeneratedComponent`, `Commit`, candidate rollback | Remove in Stage 4. Actor destruction/recreation and immediate component ownership replace keyed reuse and complete-set publication; stable keys remain only as instance-source identity. |
| `FTransactionComponentEndpoint` | Move exact-vs-generated matching into Engine instance source info in Stage 3. Delete the transaction-specific endpoint and migrate root/attachment/cache consumers; Stage 5 integrates this same source info with replacement. |
| `FTransactionTickState`, component annotation lifecycle booleans/restore sequence | Move necessary native state into shared Engine lifecycle/instance facilities in Stages 3-4; delete the wrappers and manual transaction transition sequence. Tick calls remain in the Tick owner. |
| `PendingTransactionAnnotation`, Actor/component replay flags | Replace with shared construction input/completion state in Stages 3-4. Keep only operation reentrancy guards justified by unsettled callback state; remove transaction-only duplicates. |
| `FObjectGraphReplacement`, its checked map and prepare/validate/commit protocol | Reuse in Stage 5. Prepare detached payload/cache/reference updates; publish only with all participants. No transaction-private map or post-publication fallible remapping. |
| Transactor recording/event/replay guards and GC deferral | Retain necessary safety policy at their current operation owners: synchronous observers must not recursively mutate history, and callbacks may request collection. Do not put guards into payloads or diff values. |

The only planned temporary adapter is the mixed state coordinator between the
Stage 1 payload extraction and Stage 2 observation migration; its concrete callers
are the canonical record and runtime serialization tests. Stage 2 must delete it.
No obsolete production route is admitted after its replacement stage. All shared
API searches and migration validation cover Engine, Sandbox and RoadWeaver from
`Durin.dworkspace`; the ignored raw inventory includes all three projects.

Stage 6 source audit closes the deletion map: the mixed coordinator and focused
record/promotion paths are absent, as are transaction endpoint/Tick wrappers,
construction target-set submission and transaction-only Actor/component flags.
The canonical record's duplicate transfer metadata, separate snapshot wrapper and
presence flag are removed. Archive/property operations, checked graph publication,
annotation preflight/snapshot-scope policy and operation/GC guards remain at the
owners selected above. `stage6-deletion-audit.txt` and `stage6-consumers.txt` under
ignored `Build/UE58ObjectStateFoundation/` record the cross-project searches.
The parent plan still owns business field/admission migration and UE Actor
annotation component event reporting; retained domain commands are not substitutes
for a removed foundation API.

## Implementation Stages

### Stage 0: Pin responsibilities and remove architectural ambiguity

Dependency: none. Outcome: an implementation-ready mapping with explicit deletions.

- [x] Inspect the pinned UE types and call sequences, including archetype/default
  diffing, annotation refresh, instance matching and reconstruction apply phases.
- [x] Inventory existing payloads, serializers, property/container operations,
  annotations, lifecycle entry points and replacement participants. Mark each
  reuse, change, move or delete; keep the raw inventory in ignored Build storage.
- [x] Decide whether focused undo capture is still justified by a measured caller
  need. Default to whole-object SaveObject; selected SnapshotObject remains an
  observation feature. If focused undo remains, it must use the same payload
  primitives and canonical record, not another undo representation or history.
- [x] Classify deviations as necessary runtime adaptation, required safety policy,
  or removable historical behavior. Reassess annotation `Validate/CreateSnapshot`,
  endpoint/tick wrappers, compensation and reentrancy guards by actual owner and
  failure contract; do not preserve or remove them solely for name parity.
- [x] Record the minimum target public APIs, non-editor dependency boundary,
  construction phases and old-to-new deletion map before changing shared headers.

Completion: no unresolved responsibility requires another temporary subsystem;
each retained Durin extension has a concrete reason and one owning layer.

### Stage 1: Establish one serialized payload and archive path

Dependency: Stage 0. Outcome: all transaction payload transfer uses shared primitives.

- [x] Extract UE-aligned serialized object data and identity/info types from the
  existing state implementation. Preserve exact hard-reference ownership, weak
  and soft semantics, native Serialize, versions and field filtering.
- [x] Move reader/writer behavior onto existing object Archive infrastructure.
  Share property traversal and reference interpretation with existing serializers;
  do not change package wire formats or impose transaction capture on package IO.
- [x] Put schema/version compatibility and restore preparation in the existing
  appropriate serialization/replay layer rather than duplicating metadata in
  payload, diff and record types. Keep failure compensation out of data containers.
- [x] Fill only demonstrated property lifecycle/`ContainerOps` gaps needed by the
  migrated serializers, including detached values, nested references and safe
  publication. Preserve excluded nested fields and native field compatibility.
- [x] Migrate canonical object records and retained focused consumers together;
  delete the superseded transaction writer/reader implementation in this stage.

Completion: there is one transaction payload transfer implementation; runtime-only
contracts pass without linking DurinEd and replay retains existing supported data.

### Stage 2: Separate diffable observations and migrate object events

Dependency: Stage 1. Outcome: undo data and observations have distinct, coherent owners.

- [x] Implement `FDiffableObject`-equivalent identity/flags, tagged reflected/native
  data and shared diff utilities using Stage 1 transfer rules. Specify supported
  default/archetype comparison, selection accumulation and unknown/native ranges.
- [x] Move `GetChanges`, field-range bookkeeping and observation state out of the
  mixed transaction state type. Keep actual field changes distinct from metadata,
  opaque annotation changes and editor presence changes.
- [x] Migrate Snapshot/Finalized/UndoRedo event creation and annotation comparisons.
  Preserve live IDs, primary/additional `CreatedBy`, last-snapshot closure and
  component-before-Actor completion according to the pinned source or named policy.
- [x] Remove `FTransactionObjectState` as a competing production abstraction after
  its consumers migrate. Retain a wrapper only if Stage 0 identified an independent
  responsibility; a compatibility name alone is not sufficient.
- [x] Consolidate event/diff regression tests around observable behavior and check
  no-op, cancellation, custom-only records, observer reentry and failure disposition.

Completion: events consume the shared diffable representation, undo baseline
ownership is unchanged by observations, and the old mixed implementation is gone.

### Stage 3: Add shared component instance preservation

Dependency: Stage 2. Outcome: ordinary reconstruction and transaction reconstruction
preserve instance data through one Engine facility.

- [x] Add the minimum UE-aligned instance-data base/component/cache types and
  component capture/apply hooks. Use existing generated keys, owner/class checks
  and authored identity; do not create another generated-component registry.
- [x] Define explicit apply phases around component construction/registration,
  using the phases actually present in Durin. Newly created components must expose
  the correct state to creation and registration callbacks.
- [x] Capture only instance overrides and reconstruction-specific data not already
  restored by generic object serialization. Recompute derived caches; never
  replay the same authored field from both record and instance cache.
- [x] Migrate Actor annotation data to compose the instance cache and conditional
  root/attachment reconstruction data. Remove displaced endpoint/state storage
  and bespoke annotation restore paths as their shared replacements land.
- [x] Prove the same preservation behavior through an ordinary native reconstruction
  and through Undo/Redo, including removed/recreated generated roots and attachments.

Completion: transaction annotations no longer own an alternative instance recovery
implementation, and ordinary reconstruction is a real consumer of the shared cache.

### Stage 4: Consolidate component and Actor lifecycle restoration

Dependency: Stage 3. Outcome: reconstruction and replay share lifecycle completion.

- [x] Replace desired-set construction with Actor-owned destruction/recreation,
  UE-named entry points, immediate component ownership and phased cache recovery;
  remove `FActorConstructionContext` and migrate all workspace consumers.

- [x] Audit existing creation, destruction, membership, attachment, registration,
  initialization, play and Tick transitions and their render/physics side effects.
  Distinguish authored state from native runtime state and derived resources.
- [x] Extend existing register/unregister and construction entry points with the
  required scoped reregistration/recreation behavior. Avoid a generic lifecycle
  command interpreter or a transaction-only transition scheduler.
- [x] Make ordinary property/reconstruction paths and historical replay use those
  entry points. Complete inner components before Actor-level reconciliation.
- [x] Move native restoration out of `FComponentTransactionAnnotation` and
  `FTransactionTickState` where shared lifecycle/instance facilities replace it;
  delete redundant wrappers and transaction-only Actor/component flags.
- [x] Verify callback-visible state, exception/retirement handling, repeated
  registration, deferred GC and render/physics/Tick synchronization. Use CPU
  contracts first and the targeted runtime/render tests required by changed effects.

Completion: lifecycle order has one Engine implementation, and transaction code
does not manually reproduce component registration or initialization sequences.

### Stage 5: Integrate references and instance data with object replacement

Dependency: Stage 4. Outcome: supported replacement updates shared state atomically.

- [x] Extend existing `FObjectGraphReplacement` participant preparation/validation/
  commit with serialized payloads, diffable observations and instance caches.
  Reuse its map and publication protocol rather than introducing a second remapper.
- [x] Unify admitted logical component lookup with construction instance-source matching and shared
  instance matching. Preserve strict exact-reference expiry and reject same-name
  impostors, incompatible types, ambiguous owners and incomplete maps.
- [x] Remap retained record targets, annotations, command participants, primary
  object and package state through the transaction plan's single integration point.
  Unremappable custom state must retire coherently rather than partially switching.
- [x] Cover compatible/incompatible replacement, component recreation, failure
  before publication, GC and ordinary non-transaction replacement consumers.

Completion: no transaction-private replacement map, endpoint resolver or partial
publication path remains. This supplies the foundation for transaction Stage 6;
template admission and its full behavior gates remain owned by that stage.

### Stage 6: Retire alternative paths and reorganize regression ownership

Dependency: Stages 1-5. Outcome: consumers use the new foundations exclusively.

- [x] Migrate all shared-API consumers across Engine, Sandbox and RoadWeaver and
  remove the temporary adapters listed in Stage 0. Keep actual domain commands
  only where generic serialized restoration cannot represent their operation.
- [x] Remove duplicate native state snapshots, specialized serializers, field
  locators and generated-reference wrappers superseded by the shared foundations.
  Audit annotation additional-change reporting and package/GC ownership together.
- [x] Group tests by serialization, diffing, instance preservation, lifecycle,
  replacement and transaction integration. Merge overlapping scenarios and remove
  obsolete interface/layout assertions; keep each important behavioral regression
  covered and retain meaningful failure/recovery and cross-layer tests.
- [x] Update the transaction plan's remaining tasks and UE behavior matrix to the
  final APIs. Publish implemented contracts in their Runtime/Editor owners; keep
  planning and unresolved decisions here.

Completion: the Stage 0 deletion map is closed, each behavior has one production
owner, and tests no longer require an obsolete implementation to remain present.

### Stage 7: Validate the foundation and resume transaction acceptance

Dependency: Stage 6. Outcome: verified foundations are ready for remaining features.

- [x] Run owner contracts and affected consumer targets selected through the
  [test workflow](../Agents/Testing.md), with serial isolation for state-sensitive
  reconstruction, replacement and event cases. Record final relevant inputs.
- [x] Run the default editor `all` build for the shared CoreDObject/Engine ABI and
  foundational-header changes. Validate bounded Debug DurinGame Core/CoreDObject/
  Engine/Sandbox/RoadWeaver consumers to prove no editor dependency was introduced.
- [x] Validate affected registration/render/physics behavior with the actual owning
  test targets and execution prerequisites. Preserve the transaction plan's
  application/GPU acceptance requirements; CPU evidence cannot replace those gates.
- [x] Check observation allocations and retained bytes for ordinary edits and
  repeated snapshots; exclude unconditional duplicate full payload retention and
  unexpected history growth. Full array range scaling remains transaction Stage 7.
- [x] Validate changed documents and plan lifecycle, attach concise receipts and
  record remaining transaction stages. Apply the repository
  [build workflow](../Agents/BuildAndRun.md) and evidence reuse rules throughout.

Completion: foundations have final-input evidence, obsolete implementations are
removed, and the original transaction plan can complete its remaining acceptance
without adding an alternative serialization, reconstruction or replacement path.


### Stage 8: Remove the unused cache application entry point

Dependency: Stage 7. Outcome: one cache-level application path, following UE.

- [x] Delete the unconsumed cache ApplyToComponent API and duplicate matching loop;
  retain the typed instance-data virtual hook and early compatibility validation.

### Stage 9: Separate lifecycle history from reconstruction caches

Dependency: Stage 8. Outcome: the cache contains reconstruction data only.

- [x] Move settled component lifecycle history into Actor transaction annotation
  data; remove the cache capture-mode switch and lifecycle restore phase handling.
- [x] Preserve independently captured component history and sole component-owned
  transition implementation; avoid introducing transaction lookups into caches.

### Stage 10: Derive component ownership from the object graph

Dependency: Stage 9. Outcome: no separately maintained authored aggregate.

- [x] Remove AuthoredComponents and its mutation/rebuild helpers. Reconstruct live
  ownership from native object ownership and the persistent InstanceComponents
  subset, following UE ResetOwnedComponents. Migrate all workspace consumers.
- [x] Preserve native and instance save/load, duplicate, membership order, GC and
  Undo/Redo behavior; generated components remain transient reconstruction output.

### Stage 11: Unify typed instance data storage and accept cleanup

Dependency: Stage 10. Outcome: extensible typed copying and archive construction.

- [x] Replace built-in kind switches and per-derived replacement cloning with
  registered native type descriptors for construction and copying, adapting UE's
  typed struct storage. Reuse existing FDStructOps for storage lifetime and copy;
  the name registry only resolves native cache factories and does not duplicate
  property reflection, field serialization or struct operation tables.
- [x] Validate derived data archive/replacement and fail closed for unsupported or
  mismatched types. Keep independent preparation and checked map publication.
- [x] Run owner contracts and affected workspace consumers using the
  [test workflow](../Agents/Testing.md). The Actor/header ABI change requires the
  editor all build and bounded Game Sandbox/RoadWeaver closures. Exercise actual
  render/physics/reload owners; select GPU correctness only for affected behavior.
- [x] Update lasting contracts, validate changed documents and all-plan lifecycle,
  record final-input receipts and preserve the parent plan's unimplemented gates.
