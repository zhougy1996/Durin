# UE 5.8 Transaction Architecture Alignment Plan

Summary: Align Durin's object transactions, annotations, change events, reinstancing, and editor consumers with a pinned UE 5.8 baseline.

Last reviewed: 2026-10-21

Status: Active
Completed:

## Current Status

Stage 7 is accepted against UE 5.8.0 CL 53629095 on `a2237ed10` plus this patch. Source
comparison covers EditorTransaction.cpp's SaveArray/SerializeContents and
TransArray.h's mutation adapters. Typed range records replace the unsupported
SaveArray stub; native spline point edits no longer require an unconditional
whole-owner Modify. Ordered inverses and delayed flips preserve checked mixed
record compensation. Element hard edges share the existing persistent-reference
categories, including recreated default graphs; weak edges retain exact semantics.
Implemented contracts live in [array operation records](../Editor/Architecture/Transactions/TransactionRecords.md#array-operation-records).

Consumer scope was narrowed to eligible direct array edits after auditing Engine,
Sandbox and RoadWeaver. Property/RoadNet proposals may normalize or rebuild the
wider owner, and material graph commands own semantic rollback, so these paths
retain explicit full-object/domain fallbacks. Maps retain their existing policy.
Point reordering retains its affected interval; full-curve moves, bulk edits,
annotations, vector relocation and native rebuilds can still cost O(N).

Windows/MSVC Debug CPU coverage comprises 1327 distinct isolated editor cases
across 21 targets and 228 shared-process EditorOperationTests cases. The broad
1326-case baseline is bound to `stage7-broad-inputs.json`; the final five-file
reference-table/default-graph delta passed 506 isolated cases across
EditorOperationTests, CorePropertyValueSnapshotTests, CoreObjectReplacementTests,
CoreTransactionContractTests and WorldTests. Other business paths, fixtures and
existing snapshot wire/restore code are unchanged, so their baseline receipts
remain applicable. Editor `all`, Sandbox Game and RoadWeaver Game builds passed
on the final native inputs, together with 19 isolated CoreTransactionContractTests
cases in Win64-Debug-DurinGame. Changed-document and all-plan validation passed.

The fixed one-element int32 capture probe measured identical retained bytes and
capture allocation counts at N=10,000 and N=100,000: Insert 312 bytes / 12 debug
CRT allocations; Remove and Replace 460 bytes / 49 allocations. Whole-object
captures owned 62,577 and 450,543 bytes respectively. Final shared-process samples
measured range capture at 15.2-37.8 microseconds and an Undo/Redo pair at
115.0-171.6 microseconds, versus 77.1-903.7 milliseconds for whole capture.
These Debug observations include instrumentation, use a warmed metadata path and
exclude initial vector mutation/reallocation from capture timing; they are not
a timing SLA. Range replay allocations also stay independent of N (83 for
Insert/Remove pairs and 156 for Replace). Owner annotations were disabled for
this storage probe; their independent costs remain explicit.

Reports, source/UE fingerprints, consumer decisions and all six raw cost samples
use the `stage7-` prefix under ignored `Build/UE58TransactionArchitecture/`.
`stage7-final-inputs.json` covers all 17 native inputs; full cost samples are in
`stage7-cost-samples.json` and the final shared-process XML. Stage 8 transactor
management and Stage 9 GPU/application gates remain open; this plan stays Active.

Stage 6 is accepted against UE 5.8.0 CL 53629095 on `408e150a0` plus this patch.
Source comparison covers TransactionCommon.cpp's root/subobject categories,
Obj.cpp's Modify path and EditorTransaction.cpp's OnObjectsReinstanced. Ordinary
objects keep exact generation identity; published CDO/default-subobject references
use class-checked Outer/name chains. Transactional default-graph nodes use ordinary
native Modify/serialization. Script/reflected mutation and template construction
protections remain intact; arbitrary archetypes and instance propagation remain
unsupported. Opaque commands retain exact participants and require checked
replacement or expire before replay. The explicit native hook reuses the existing
history participant, whole-entry retirement and package checkpoint policy.
Implemented rules live in [Runtime Transactions](../Runtime/ObjectSystem/Transactions.md)
and [Transaction Records](../Editor/Architecture/Transactions/TransactionRecords.md).

Windows/MSVC Debug acceptance passed: 1312 isolated editor cases across 21 targets,
213 shared-process EditorOperationTests cases, and 19 isolated runtime-only
CoreTransactionContractTests cases on Win64-Debug-DurinGame. Editor `all` and
bounded Sandbox/RoadWeaver Game builds passed. Coverage includes default-graph
recreation, GC, native-command expiry, same-package reinstancing, rejected owner
and partial maps, destination slot reuse, failed preparation, annotations and
cross-package checkpoint preservation/retirement. Obsolete exact-only serialized
reference storage and its assertions were replaced; relevant failure and lifecycle
tests remain. Existing global default-graph, package and script protections passed.

Reports, source comparisons, build logs and consumer audits use the `stage6-`
prefix under ignored `Build/UE58TransactionArchitecture/`. The final receipt
`stage6-final-inputs.json` fingerprints all 17 changed native inputs and three UE
sources. Broad coverage is recorded against `stage6-broad-inputs.json`; the final
delta freezes custom participants to their existing exact category, adds its
expiry regression, and corrects a Class header comment. EditorOperationTests,
EditorPropertyTests, LevelMutationTests and WorldTests were rerun on that final
delta; the other consumer implementations/fixtures and generic reference code are
unchanged, and ordinary custom participants already had exact semantics. Final
workspace builds and Game contracts cover the final input fingerprints. Changed
documents and all plans validate. GPU/application acceptance remains in later stages.

Stage 5 is accepted against UE 5.8.0 CL 53629095 on `dccd1303abc9` plus this
Stage 5 patch. Source comparison covers EditorTransaction.cpp, ActorEditor.cpp,
Actor.cpp, Level.cpp, ActorComponent.cpp and the Actor/Level declarations.
Actor Label, Actor/Component Name and existing-skybox texture edits now use
generic object records. Property sessions and transactional CallInEditor share
business admission. Level records own serialized membership/camera fields;
native presence and attachment adapters complete ownership and registration.
The consumer audit covers Engine, Sandbox and RoadWeaver and records each
remaining custom command's domain rationale in
[Transactors](../Editor/Architecture/Transactions/Transactors.md). Implemented replay rules
live in [Transaction Records](../Editor/Architecture/Transactions/TransactionRecords.md) and
[Level System](../Runtime/World/LevelSystem.md).

UE's Save-all-before-Load order and whole-transaction no-op decision are retained.
Durin's checked-failure extension publishes inverse history only after successful
native completion, allowing clean compensation and retry without changing
historical dependencies. UE-style completion is adapted to retained absent
objects, including preservation of live Actors added outside the recording.
Obsolete generated-component pointer-reuse assertions were replaced by source-key
resolution and restored-output checks; play/shutdown guards exercise structural
component edits instead of the removed bespoke rename history. Mixed-edit,
failed-replay, cancellation and lifecycle regressions remain covered.

Final Windows/MSVC Debug validation passed on the 24 native input fingerprints in
`Build/UE58TransactionArchitecture/stage5-final-inputs.json`: 1261 isolated
editor-profile cases across 24 targets, 460 shared-process cases across
EditorOperationTests/WorldTests/LevelMutationTests, and 19 isolated
CoreTransactionContractTests cases on Win64-Debug-DurinGame. The editor `all`
build and bounded Sandbox/RoadWeaver Game builds passed. The editor build covers
shared Actor/Level/Scene and transaction headers; the additional Game builds and
consumer tests cover the other projects declared by Durin.dworkspace. Reports,
build logs, seven UE source fingerprints and the symbol audit use the `stage5-`
prefix under that ignored receipt directory. Final report checks found no failed,
errored or skipped cases; native fingerprints still match. Subsequent edits are
documentation only. Changed-document and all-plan validation passed. GPU and
application gates remain for later stages.

Stage 4 is accepted against UE 5.8.0 CL 53629095 ActorEditor.cpp and
TransactionCommon.cpp on `e2ae7f2c1` plus this Stage 4 patch. Actor
annotations observe generated components, match current instances with shared
component source information, and use CoreDObject property-only diffs for events.
Native and instance components keep their own records. Durin has no child-Actor
or archetype transaction admission yet: newly generated live components report
presence and captured reflected fields, without inventing an archetype baseline.
Removed components have no live notification target. Observation data is ephemeral
as in UE, separate from serialized reconstruction data, and is checked during
history replacement and counted in retained annotation storage. Reference slots
use UE-style path FNames, including translation through a mapped ancestor when
the captured generated reference has already been collected. Shared serializer
options distinguish property observation from ordinary virtual Serialize/replay;
construction-owned references remain excluded from ordinary undo fields.

Final Windows/MSVC Debug validation passed on the eight native input fingerprints
in `Build/UE58TransactionArchitecture/stage4-final-inputs.json`: 618 isolated
editor-profile cases across EditorOperationTests (205), WorldTests (189),
LevelMutationTests (55), CoreTransactionContractTests (19),
CoreObjectReplacementTests (40), AssetPackageReloadTests (15), SplineTests (69),
SandboxGameplayTests (13), RoadSceneIntegrationTests (5) and
RoadEditorIntegrationTests (8); 19 more CoreTransactionContractTests cases on
Win64-Debug-DurinGame; and 394 shared-process EditorOperationTests/WorldTests
cases. The editor all build and bounded Sandbox/RoadWeaver Game builds passed.
Reports, source/UE fingerprints and build logs use the `stage4-` prefix under
that ignored receipt directory. Final changes after these runs update documents
only; changed-document and all-plan validation passed. The source audit covers
component source matching, property-only/native serialization separation,
construction filtering with the root exception, snapshot closure, live replay
targets, reference-path equality, absent/collected generation replacement,
immutable source snapshots, GC and bounded history storage. Existing generic
event tests cover nested/context ordering, selected snapshots, custom records,
no-op/cancel, exceptions and compensation. GPU/application behavior is unchanged
and those later plan gates are not claimed here.

Transactor management and final application gates remain open; this parent plan
stays Active.

Foundation cleanup Stages 8-11 are accepted on final native inputs: 1125
isolated CPU cases, 426 shared-process cases, rendered Discard Vulkan
qualification, the editor all build and bounded Game closures passed. The
foundation plan is Completed again. Reconstruction caches contain instance data
only; annotations own lifecycle history, component ownership is derived from
the object graph and instance membership, and typed factories reuse FDStructOps.
The subsequent Stage 4 event acceptance is recorded above. Business,
template/CDO and other remaining feature/application gates stay open.

The foundation's final consumer gate found that copied components lacked ownership
when Spline PostLoad requested Actor reconstruction. The shared UE-style
`PostInitProperties` allocation hook now establishes native component ownership
before graph callbacks. Editor instance-presence commands explicitly prepare
absence before their recorded Apply. This is an initialization foundation fix,
not template transaction admission or a new duplication-specific context.
Foundation Stage 7 is accepted: 1292 isolated CPU cases, 386 shared-process
regressions, two Vulkan correctness gates, the editor all build and bounded
Game project closures passed on final native inputs. That receipt covers the original foundation inputs; the subsequent cleanup
acceptance is recorded above.
All remaining gates here stay open until their own features and evidence are
complete.

Foundation Stage 6 removes duplicated record/snapshot storage and groups the
regressions by owner. Its cleanup does not implement this plan's remaining
business-consumer or template features. At that acceptance Actor Label was still
NonTransactional and used a domain command; Stage 5 now replaces that history
representation. Actor rename still checks level membership and reserved names
for new edits. Actor/component presence,
attachment and Material graph commands retain structural/domain work that a
serialized field load alone does not perform. These are not temporary adapters
for the removed mixed-state API. UE Actor annotation diffable-component additional
change reporting remains a Stage 4 event feature; generic annotation additional
changes already have snapshot/finalization closure coverage. The Stage 4
acceptance above now includes Actor diffable-component reporting.

The object-state foundation's Stage 5 now supplies checked ordinary-object
reinstancing through the existing reload participant: transfer/observation data,
Engine instance caches, annotations, affected native commands, primary objects,
history position and package checkpoints prepare and publish together. Unaffected
commands retain their unique ownership; unsupported affected state retires the
whole entry. Document Discard consumes this same integration. At that foundation
acceptance, this plan's Stage 6 default-graph admission and category tests remained
open; their subsequent acceptance is recorded above. Later gates remain below.

The object-state foundation's Stage 1 migration supersedes the historical focused
capture/promotion interfaces described in earlier receipts below. Current generic
history uses whole-object capture exclusively; remaining acceptance must use the
new runtime identity and shared transfer APIs. Earlier focused-test names are
historical evidence, not supported production routes.

Execution order update: the user authorizes substantial foundational refactoring.
The [object state foundation plan](UE58ObjectStateFoundation.md) now owns the
replacement of mixed transaction state, component recovery and replacement
integration. Complete its Stages 0-2 before expanding/closing this Stage 4, its
Stages 3-4 before broad Stage 5 migration, and its Stage 5 before Stage 6
replacement parity. This plan retains its functional and final acceptance gates;
completed historical stages are evidence, not exemption from validating changed
foundations. Do not develop parallel implementations under the two plans.

Stage 0 is complete against the installed UE 5.8.0 CL 53629095 source snapshot
identified below. Stages 1 through 4 are complete. Core owns custom
changes, context and operation identity; CoreDObject owns exact references,
serialized states and annotation contracts. Modified-participant recording and editor consumer
migration are implemented and validated as recorded below.
The starting production revision is
`3e9c43a5b`, which separates Actor Name and Label. Its 516 passing naming and
consumer tests are useful regression evidence, not transaction parity evidence.

Durin has one editor-owned history, `GUndo`, nested scoped recording, `Modify()`,
whole-object virtual serialization, custom commands, collector-owned references,
saved package checkpoints and recovery-aware replay. Object-level event dispatch
and active snapshots were introduced at `f6bc02f00` and migrated to the shared
foundation; Stage 4's full acceptance is recorded above. Stages 6 and 7 implement
template-aware logical references and explicit array operation records. Custom records freeze explicit
modified participants separately from GC dependencies; single-object declarations
bind their target and composites supply summaries, hooks and affected packages.

The existing [transactor contract](../Editor/Architecture/Transactions/Transactors.md) remains
a description of the current implementation during migration. Its scope and
custom-record rules have now been checked against the pinned UE 5.8 source;
unimplemented features in this plan are not current production guarantees.

Stage 0 validation: base revision `44f92feae109239eb6c0cad24a439a6d66e22f7f`
plus this stage's three characterization cases; registry selection
`EditorOperationTests`, profile `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor`, MSVC 14.44.35207 on Windows. All 137 cases passed;
the three new cases also passed individually through serial CTest isolation.
Reports: `Build/TransactionUE58/stage0-editor-operation.xml` and
`Build/TransactionUE58/stage0-isolated.xml`. The initial sandbox build failed
with C1902; the same target built and ran successfully outside the sandbox.
Changed-document validation, all-plan validation and diff whitespace checks
passed. These receipts cover the unchanged production implementation and this
stage's test inputs only; they do not satisfy later architecture acceptance.

Stage 1 validation: base `a6816f517` plus this stage's implementation and fixtures,
profile `windows-msvc-x64`, MSVC 14.44.35207 on Windows, Debug presets with Tracy
enabled. The default `Win64-Debug-DurinEditor` **all** build passed after the
DObject ABI and Engine/Sandbox/RoadWeaver consumer migration. Registry selection
`@domain=reflection+editor-operation+property-editor+viewport+material-editing+world+sky-box+editor-shell+spline,kind=contract+feature`
passed all 26 targets with four CTest slots. This includes all 140
`EditorOperationTests` cases, runtime contracts and property, structural, material,
viewport, package and lifecycle consumers. Reports:
`Build/TransactionUE58/stage1-consumers.xml` and
`Build/TransactionUE58/stage1-editor-all.log`.

Additional registered targets `ContentBrowserWorkflowTests`,
`PythonToolIntegrationTests`, `PythonReflectionIntegrationTests` and
`PythonScriptPanelTests` passed (135/137 ContentBrowser cases, two skipped for
unavailable directory/file symlink privileges; all 48, 45 and 4 Python cases
passed). Their reports are `Build/TransactionUE58/stage1-<Target>.xml`.
The consumer build exposed an existing pointer call to reference-taking property
session Begin; that fixture now uses the declared reference API.

The three new editor cases passed individually with serial CTest isolation:
`Build/TransactionUE58/stage1-editor-isolated.xml`. Registered
`CoreTransactionContractTests`, linked only to Core/CoreDObject, built and passed
all three cases in isolation under `Win64-Debug-DurinGame`:
`Build/TransactionUE58/stage1-runtime-game.xml`. This closes Stage 1's runtime
boundary; the wider Engine/project DurinGame foundation and final gates remain
in Stages 3/9. Builds ran outside the sandbox due the established C1902 issue.
Documentation-only status edits preserve these source/environment receipts;
annotation/event dispatch, serialization and later parity gates remain open.

Stage 2 validation: base `6a1259062` plus this stage's implementation and fixtures,
same Windows/MSVC profile and Debug presets as Stage 1. The 26-target CPU consumer
selection above passed with four CTest slots (`Build/TransactionUE58/stage2-consumers.xml`),
including 148 EditorOperationTests cases and property, structural, package,
material, Spline, SkyBox and World consumers. The editor **all** build passed
(`Build/TransactionUE58/stage2-editor-all.log`) for the Core Archive ABI change
and all Engine/Sandbox/RoadWeaver consumers. The nine runtime-only cases passed
individually in `Win64-Debug-DurinGame`
(`Build/TransactionUE58/stage2-runtime-game.xml`). The eight new editor cases and
two structural regression cases passed individually with serial isolation
(`stage2-editor-isolated.xml` and `stage2-structural-isolated.xml` in the same
evidence directory).

Validation found and fixed two integration defects: container baseline queries
consumed package wire markers, and lazily declared intrinsic classes lacked an
exact registered identity. The final code uses non-consuming
PreservesContainerParentValues and completes class registration before capture.
Coverage includes native-only/no-op state, first-before/final/flip state, focused
coalescing and promotion, metadata, hard/weak/soft references, excluded nested
fields, schema/expiry rejection, load compensation and callback exception
recovery. Changed-document/all-plan validation and diff checks passed. No GPU or
application-hosted execution is claimed by these receipts; later explicit gates
remain in Stages 3 and 9.

Stage 3 validation: the runtime annotation foundation landed in `77f8a7207` on
base `16b241ea0`; final Engine integration and fixtures were validated on
`77f8a7207` plus this stage's changes, under the same Windows/MSVC/Debug profile.
The 26-target CPU consumer selection above passed, including all 163
EditorOperationTests (`Build/TransactionUE58/stage3-engine-consumers.xml` and
`stage3-engine-editor-operation.xml`). All twelve new Engine/focused/compensation
cases passed independently with serial isolation (`stage3-engine-isolated.xml`).
The twelve Core/CoreDObject runtime contracts passed in serial isolation under
Win64-Debug-DurinGame (`stage3-foundation-runtime-game.xml`). All named reports
are in `Build/TransactionUE58/`.

The foundation editor **all** build passed
(`Build/TransactionUE58/stage3-foundation-editor-all.log`). Production targets
Engine, Sandbox and RoadWeaver built under Win64-Debug-DurinGame, including their
Core/CoreDObject dependencies (`stage3-foundation-game.log` in the same directory).
This closes Stage 3's workspace ABI and runtime layering milestone. Changed-doc,
all-plan and diff validation passed; later changes require applicable coverage
again. These receipts do not replace Stage 9's final acceptance matrix.

Annotations now capture immutable first/final/inverse snapshots before Serialize,
retain their hard dependencies and use checked owned-byte accounting. Native
Actor/Component state and generated-root recovery use Engine lifecycle adapters;
authored fields remain in the canonical serializer. Components complete before
owners; generated roots are hydrated before creation callbacks. Tests cover GC
requests, expiry before field mutation, failed reconstruction, retired callback
participants, root kind switches and repeated restoration without duplicate
lifecycle callbacks. In-process annotation serialization is explicit and separate
from object bytes or history persistence. Active observation snapshots remain in
Stage 4; general controlled reinstancing remains in Stage 6.

Validation also fixed a standalone-light no-op regression through implicit default
component annotations, preserved original annotations across compensation retries,
and retained focused first-before state during promotion. A mixed serialized-child
and generated-root test exposed a Redo pose error (13 instead of 8). Snapshot
refresh now preserves the original recovery participant scope and captures current
nullable parents/poses even after root removal; the mixed case passes in whole
target and isolated runs.

Stage 4 work in progress on `32888aa64`: runtime object-state diffs retain original
identity strings and distinguish reflected field changes from native byte changes,
including unnamed native payloads. Detached focused promotion now preserves
reflected field classification. All sixteen runtime contracts passed in serial
isolation in both editor and Win64-Debug-DurinGame presets:
`Build/TransactionUE58/stage4-runtime-diffs-editor.xml` and
`stage4-runtime-diffs-game.xml`. The editor all build and all 163 transaction
regressions passed (`stage4-runtime-diff-editor-all.log` and
`stage4-runtime-diff-editor-operation.xml` in the same directory). These receipts
cover `32888aa64` plus this runtime-diff implementation and fixtures, with the
same Windows/MSVC/Debug configuration as Stage 3. Event dispatch, active snapshots
and observer reentrancy work remained open at that checkpoint.

Stage 4 continues on `24eb83016` with synchronous Finalized/UndoRedo dispatch,
active snapshots and capture/observer reentrancy guards. Record events and
annotation events preserve UE's separate `CreatedBy` sources; there is no
cross-object event coalescer. Snapshots own independent observation baselines,
accumulate selected members and release observation storage after finalization.
Focused observation does not serialize an entire object or clone its undo record.
The two budget fixtures now measure an entry rather than assuming a fixed C++
record layout. All 173 transaction regressions and ten serially isolated event
cases passed (`Build/TransactionUE58/stage4-object-events.xml` and
`stage4-object-events-isolated.xml`) on the editor Debug preset with Windows/MSVC,
covering `24eb83016` plus this event implementation and fixtures. The final editor
all build passed (`Build/.agent-state/logs/20261020-151320-236102-19576-cmake.log`),
as did all sixteen non-editor runtime contracts in serial isolation
(`stage4-event-runtime-game.xml`). The 26-target CPU consumer set passed
(`stage4-event-consumers.xml`); it covers the same public event layout and dispatch
implementation before the final snapshot no-op and last-annotation comparison
fixes. The final transaction regressions and isolated traces cover those two
behavioral adjustments, which do not change another consumer's inputs. Native annotation
summaries and the complete Stage 4 source/behavior audit remain open.
Stage 4 is not complete. The source audit additionally aligned UE's no-op handling
of uncaptured/custom/finalized snapshot requests and the final annotation comparison
against both initial and last snapshot state. The obsolete unsupported-snapshot
assertion was removed; functional snapshot coverage owns that behavior.

### Stage 1 temporary adapters

These are declarations/adapters around the one recorder, not alternate histories.
Implemented module rules live in [runtime transaction contracts](../Runtime/ObjectSystem/Transactions.md).

| Adapter | Owner | Required removal/replacement |
| --- | --- | --- |
| Editor namespace aliases for Core custom/context types and CoreDObject exact references | DurinEd | Stage 8 removes compatibility aliases after consumer migration; no duplicate definitions remain. |
| ContainsPieObjects explicitly returns an unsupported error rather than false | DurinEd | Stage 8 supplies PIE package identification and inspection policy. |

`RuntimeBoundaryPreservesOperationContextAndRejectsInvalidRequests` checks invalid
array descriptors and the remaining explicit PIE error. Array behavior is covered
by functional `FTransactionArrayTests`; Stage 8 must replace the PIE expectation
when that implementation lands.

Context primary-object pointers are borrowed values at the Core API boundary;
the editor retains and resolves the CoreDObject exact identity. Newly created
objects in structural commands are declared after initial execution, when
StoreUndo freezes their identities; this does not imply a pre-execution rollback
service. Stage 5 retains the required native lifecycle preparation and adapters.

## Goal

Make ordinary object and property edits use a shared transaction mechanism, with
custom commands reserved for operations that require executable domain behavior.
Support UE 5.8-shaped object serialization, annotations, per-object changes,
snapshots, object replacement, array operations, and history controls. Preserve
correct GC, package dirty state, native lifecycle handling, and cross-editor order.

Alignment means verified observable semantics and appropriate module boundaries.
It does not require UE container layouts, raw array addresses, identical class
names, a Blueprint implementation, or persistent undo across application restarts.
UE 5.8 is the selected version, not a moving latest-version target. A later UE
upgrade requires a new recorded comparison and an explicit plan amendment.

## Selected Architecture And Migration Rules

- Put runtime-neutral transaction interfaces, context, and custom-change contracts
  in Core; put object identity, serialization, annotation, and object-event support
  in CoreDObject. Engine owns Actor/Component reconciliation. DurinEd owns editor
  history and recording policy. Runtime code must not depend on DurinEd to define
  a transaction-capable object or a custom change.
- Use object-owned records for general serialization, object metadata, annotations,
  and diff/snapshot state. The foundation plan replaces focused history with
  whole-object SaveObject; selected SnapshotObject remains observation only.
- Single-object custom records bind their target. Composite operations explicitly
  distinguish modified participants from GC dependencies. A collector reference
  alone never implies that an object was edited or should receive completion hooks.
- Keep strong-reference collection, weak/soft-reference semantics, checked native
  byte accounting, module retirement safety, package checkpoints, and explicit
  recovery handling. Treat these as documented Durin extensions where the verified
  UE contract differs; do not silently weaken them to match an API signature.
- Keep exact identities for ordinary objects. Add controlled logical references
  and explicit replacement mapping for supported reinstancing cases. Never resolve
  an arbitrary stale object by finding a different object with the same Name.
- Treat reversible editor absence separately from irreversible garbage/destruction.
  Restoring serialized fields alone must not bypass component registration, Tick,
  attachment, renderer, or physics lifecycle boundaries.
- Preserve one ordered application history across documents, tools, property UI,
  and Python. Ordinary scoped cancellation is not an implicit rollback service;
  settle its precise UE 5.8 semantics in Stage 0.
- Temporary adapters may keep stages buildable, but each needs an owner and removal
  stage. The completed system must have one canonical capture/replay path rather
  than permanent parallel legacy and UE-style implementations.
- Search shared symbols across all source/test roots in `Durin.dworkspace`, including
  Engine, Sandbox, and RoadWeaver. Migrate every affected consumer and fixture.
  Commit stage status and exact Plan/Stage provenance with implementation changes.

## Reference And Source Ownership

Use these UE 5.8 official contracts as the public reference. Their presence proves
an interface exists, not an undocumented ordering, lifetime, or failure semantic.
Stage 0 must pin the implementation evidence needed for those details.

| Concern | UE 5.8 reference |
| --- | --- |
| Runtime capture, commands, snapshots, and arrays | [ITransaction](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/ITransaction) |
| Object serialization and diffable record state | [FObjectRecord](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FTransaction/FObjectRecord) |
| Extra restoration data | [ITransactionObjectAnnotation](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/ITransactionObjectAnnotation?lang=en-US) |
| Actor/Component reconstruction data | [FActorTransactionAnnotation](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FActorTransactionAnnotation) |
| Per-object completion and changes | [PostTransacted](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObject/PostTransacted?lang=en-US), [FTransactionObjectEvent](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FTransactionObjectEvent?lang=en-US) |
| Reinstancing-aware identity | [FPersistentObjectRef](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPersistentObjectRef) |
| Buffer policy, barriers, and replacement hooks | [UTransBuffer](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UTransBuffer?lang=en-US) |

Primary implementation sites are
[Core ITransaction](../../Engine/Source/Runtime/Core/Public/Misc/ITransaction.h),
[DObject](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h),
[object archives](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h),
[editor transaction records](../../Engine/Source/Editor/DurinEd/Public/Transactions/TransactionObjectRecord.h),
[transactor implementation](../../Engine/Source/Editor/DurinEd/Private/Transactions/Transactor.cpp),
[command admission](../../Engine/Source/Editor/DurinEd/Private/Editor/EditorCommand.cpp),
and [Actor mutations](../../Engine/Source/Editor/DurinEd/Private/Actors/ActorMutations.cpp).

Follow the existing [record foundation](../Editor/Architecture/Transactions/TransactionRecords.md),
[property editing](../Editor/Architecture/PropertyEditing/ReflectedPropertyEditing.md),
[serialization](../Runtime/ObjectSystem/Serialization.md),
[GC](../Runtime/ObjectSystem/GarbageCollection.md), and
[reflection/defaults](../Runtime/ObjectSystem/ReflectionSystem.md) contracts when changing
their boundaries. Record intentional changes before depending on them.

## Implementation Stages

### Reference identity and reproduction

The user supplied the local Epic installation at
`E:/Programs/Epic Games/UE_5.8`. Its `Engine/Build/Build.version` identifies
**5.8.0, Changelist 53629095, branch ++UE5+Release-5.8**, promoted build,
non-licensee build, CompatibleChangelist 0. This is an installed source snapshot,
not a claimed Git commit. The SHA-256 fingerprints below pin the actual evidence
bytes independently of the mutable installation directory or public API pages.
No UE source is copied into this repository. Reproduce the review with the same
installation build and `Get-FileHash -Algorithm SHA256` for each relative path;
changed fingerprints require a fresh comparison before reusing this evidence.
The machine-readable inventory is `Build/TransactionUE58/ue580-source-identity.json`.

All paths in this table are relative to the UE installation, not Durin paths.

| UE evidence file | SHA-256 |
| --- | --- |
| `Engine/Build/Build.version` | `a0980725880e3db8d99a82e25474e4a407c1fffdde716e7af6bb684a7cf73a32` |
| `Engine/Source/Runtime/Core/Public/Misc/ITransaction.h` | `74a6aedc8d6d2b183c2c04886d9e79c5762269f60a8ee698f258c7bebabfe24d` |
| `Engine/Source/Editor/UnrealEd/Classes/Editor/Transactor.h` | `dd96718f964be87f66226e4a3434ad0b18fb87678b28d390952ed09545e5bcd7` |
| `Engine/Source/Editor/UnrealEd/Classes/Editor/TransBuffer.h` | `fb092124c2f2ec7b4efdfdbcb6d43cbb8bdb6b7756d38119380a35b20e8ea023` |
| `Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp` | `56f5975aed86e54af420bb5916f6de6ada9f054a1fcceb9d621d4a8ad06e2827` |
| `Engine/Source/Editor/UnrealEd/Private/ScopedTransaction.cpp` | `a78f33731fd830c17c57413b5c0e94cb289c85f22e01bb9e12499b8e20471b40` |
| `Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp` | `98312cd54fe9b1a533e8daf2e905729543586c97c2adf2cff4534175f1b4b09a` |
| `Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp` | `e9837961c288f6632cd1e005e5c5d803980955055609aa46b73e39eae429d3d5` |
| `Engine/Source/Runtime/CoreUObject/Private/UObject/Property.cpp` | `034566684438e4ac74bb61183fd8f95f3df70b7759d5de7672a0ae1f25a1961f` |
| `Engine/Source/Runtime/CoreUObject/Public/Misc/TransactionCommon.h` | `2981a5c8d9f340ba3edfd941f259c3242285838fbaf1c7ead04f612d7b9ab146` |
| `Engine/Source/Runtime/CoreUObject/Private/Misc/TransactionCommon.cpp` | `0dd170aed490f90c4386ef7f9c5b581ac88041b0204646e632308851e57da940` |
| `Engine/Source/Runtime/CoreUObject/Public/Misc/ITransactionObjectAnnotation.h` | `538bb006fb6115bc29b0a525df81f699283069afe44e9dc1bc5168b249ee64c0` |
| `Engine/Source/Runtime/CoreUObject/Public/Misc/TransactionObjectEvent.h` | `77f91015c3582da8064f297d71bd7c97adcee3e37a6b6a120ac89ce97065c8ca` |
| `Engine/Source/Runtime/Engine/Private/Actor.cpp` | `fe4a77dfea664de75354ccd07445f8690eb17a242a5b6eafe281fa5496ac2270` |
| `Engine/Source/Runtime/Engine/Private/ActorEditor.cpp` | `39ab9f01c7ae16f146b4e8de7d8638254ed22bc8b48ed7a24e5adb9cf5978554` |
| `Engine/Source/Runtime/Engine/Public/ActorTransactionAnnotation.h` | `971ab3c8ff1e420077feeae65855d5aba599fd4cf71f32895f21834c73d3a4cd` |

### Stage 0 behavior matrix and selected contracts

Evidence kinds: **A** = official public API, **S** = the pinned source above,
**D** = an intentional Durin extension. No executable UE probe has been run.
Source locations below abbreviate those exact files; symbol names, rather than
line offsets alone, define reproduction. A source-verified outcome is a target
contract, not a claim that Durin already implements it. Stage 9 must attach
implementation and test evidence to every row.

| Behavior | Verified UE outcome and source location | Selected Durin outcome / implementation gate |
| --- | --- | --- |
| Nested Begin/End | **S** `TransBuffer.h::BeginInternal`, `EditorTransaction.cpp::UTransBuffer::End`: Begin returns prior depth; End returns prior depth and only depth 1 finalizes. | Preserve one outer recording, context and final capture; existing `NestedModifyPreservesOutermostFinalState` and `NativeBeginEndAndInnerCancelUseUEActiveCounts` characterize it. |
| Cancel | **S** `UTransBuffer::Cancel`: any nonzero index is normalized to zero; drops the entire active recording, restores displaced history, does not call Apply. | Cancel is discard without rollback, including inner scopes; property gesture rollback is a separate domain action. Stage 8 preserves this distinction. |
| Disabled scope | **S** `ScopedTransaction.cpp::Construct`: disabled helpers never Begin; they do not suspend GUndo. | An outer recording still captures Modify inside a disabled helper. `DisabledScopeLeavesOuterCaptureAndLifetimeIntact` covers depth and cleanup. Exact stale-helper protection remains **D**. |
| First/final capture and replay | **S** `SaveObject`, record constructor, `Finalize`, `Save`, `Load`: first capture calls virtual Serialize; final state is serialized; replay captures current state before loading and swapping the two sides. | The foundation's canonical record uses whole-object states with live inverse capture. Before/final/inverse/snapshot storage shares one representation with no duplicate capture metadata. Prepare every inverse before mutation; compensation/recovery is **D**. No parallel focused-property replay remains. |
| Custom before Modify | **A/S** `ITransaction::StoreUndo`, `SaveObject`: custom adds an object record; later SaveObject sees an existing record and adds no serialized snapshot. | Preserve this exclusive object-owner rule. Commands needing generic capture for unrelated authored fields must use serialized participants or distinct declared participants rather than claiming the whole object. Stage 1 migrates callers. |
| Modify before custom | **S** `SaveObject`, `StoreUndo`, `Apply`: the earlier serialized record remains; Undo reverses record order, Redo follows record order. | Retain order, with no implicit deletion of the earlier snapshot. `ModifyBeforeCustomPreservesOrderedReplay` characterizes it. Domain callers must avoid duplicate ownership of a field; ordinary edits migrate to generic records. |
| Focused capture | **S**, UE SaveObject has no focused-history overload; SnapshotObject is observation, not focused undo storage. | Foundation Stage 1 removes focused history, promotion and member locators. Property editing captures a whole object before mutation; domain commands retain only their own required property payloads. |
| Array operations | **A/S** `SaveArray`, `SerializeContents`: owner-bound range records; operation +1 removes on load, -1 inserts on load, 0 restores values; replay reverses the operation sign. Array Finalize skips diffing; a bare array record without annotation does not itself set HasChanges. Raw array addresses are UE-specific. | Stage 7 uses reflected owner/member identity and typed adapters, retains no live vector address, bounds insert/remove payload by changed elements, and falls back explicitly for unsupported containers. Reliable standalone array change detection is **D**; test it without requiring a duplicate whole-container snapshot. |
| No-op and transient entries | **S** `FObjectRecord::HasChanges`, `FTransaction::IsTransient`, `UTransBuffer::End`: serialized byte/name/reference changes, custom records or annotations retain history; no-op/PIE entries restore displaced history after finalization. | Stage 2 compares serialized state, not only property diffs. Annotation-only changes retain history. `NoOpAndCanceledRecordingPreserveRedoBranch` characterizes branch preservation; annotation and PIE cases arrive in Stages 3/8. |
| Property filtering | **S** `Property.cpp::ShouldSerializeValue`, `Transactor.h::FReader/FWriter`: transaction archives are nonpersistent; Transient alone is included, NonTransactional and ordinary Deprecated fields excluded. | Preserve current transaction-purpose filtering, including nested structs. Stage 2 must not accidentally reuse persistence filtering. Existing filtering tests remain valid. |
| IDs and operation lifetime | **A/S** `ITransaction.h::FTransactionContext`, `BeginOperation`, `EndOperation`, buffer Begin/End/Undo/Redo: stable transaction ID, fresh nonzero operation ID per recording or replay, cleared after completion/cancellation. | Stage 1 places context in Core; Stage 4 supplies operation IDs to synchronous object hooks. Monotonic session IDs instead of GUID layout remain **D**. |
| Snapshot events | **S** record `Snapshot`, transaction `SnapshotObject`: only existing noncustom object records; selected property sets accumulate, an unfiltered request stays unfiltered; compares last snapshot (first uses initial state); does not replace undo before-state. | Stage 4 emits synchronous Snapshot events with the live operation ID; no new history entry. Custom/array/finalized/uncaptured records do not acquire synthetic snapshots. |
| Finalization events | **S** transaction `Finalize`, buffer `End`: component completion precedes Actor completion; changed serialized state/diff or annotation triggers PostTransacted; object Finalized occurs before operation closes and GUndo clears. | Stage 4 keeps object hooks synchronous in live context; queued idle history listeners remain separate **D**. No-op without annotation emits no object change. Cancel emits no Finalized object event. |
| Undo/Redo event order | **S** `Apply`, buffer `Undo/Redo`: distinct targets get PreEditUndo, all records save/load, component PostEditUndo before Actor, then each object's PostTransacted; operation remains live. Custom-only records get completion hooks but no synthetic serialized delta event. | Stages 3/4 preserve ordering and declare modified participants explicitly. Recoverable failures and compensation emit no misleading successful completion; balanced cleanup/recovery dispositions are **D** and need trace tests. |
| Annotation capture/lifetime | **A/S** `FSerializedObject::SetObject`, record `Snapshot/Finalize/Save/Load/AddReferencedObjects`: annotation captured before serialization, at finalization/snapshot and inverse capture; both serialized sides enumerate references; replay uses restored side's annotation. | Stage 3 puts annotation ownership/hooks in CoreDObject, additional object changes in event contracts; checked owned-byte accounting is **D**. Default object annotation is absent. |
| Actor reconstruction | **S** `Actor.cpp::Modify`, `ActorEditor.cpp::FactoryTransactionAnnotation/CreateIfRequired/PreEditUndo/PostEditUndo`: native root capture, conditional instance/root/attachment data; lifecycle work accompanies state restore. | Foundation Stages 3-5 use Engine-owned destruction/recreation, instance caches, root/attachment data and shared lifecycle/Tick owners. No Blueprint subsystem is required. Actor/Component annotations adapt these owners; they do not schedule a second lifecycle implementation. Full editor consumer acceptance remains Stage 5 below. |
| Package state | **S** `SavePackage`, `Apply`, dirty-fence counter, `Obj.cpp::Modify`, `UObjectGlobals.cpp::SaveToTransactionBuffer`: package state is independent of object fields; replay updates dirty state before loading; saves fence historical clean state. Modify can dirty without retaining history. | Keep Durin saved-revision/checkpoint authority **D**, including external writes, retirement and branch changes; do not replay dirty as a serialized member. Existing checkpoint/uncaptured dirty tests remain valid. |
| Undo barriers | **A/S** `SetUndoBarrier/RemoveUndoBarrier/ClearUndoBarriers/GetCurrentUndoBarrier/CanUndo`: stack stores queue cursor; only top barrier limits further Undo; empty query is invalid index; Reset clears stack. | Stage 8 implements cursor barriers with tests for push/pop/clear, Redo, reset and trimmed history; retirement must preserve a valid bound. |
| PIE inspection | **A/S** `ContainsPieObject/ContainsPieObjects/IsTransient`, `SaveArray`: checks target and serialized references on both sides; any PIE makes entry transient; array capture rejects PIE owner. Custom/annotation traversal is not a blanket PIE scan in this implementation. | Stage 8 makes PIE support explicit at package boundary; stronger declared-participant/dependency inspection is **D**. No implicit equivalence between runtime world and PIE package. |
| History budget/branching | **S** `BeginInternal`, `End`, `Cancel`, `DataSize`: Begin temporarily removes Redo or oldest prefix at size >= budget; cancel/no-op restores it; successful entry can exceed budget; DataSize counts serialized side plus custom bytes. | Preserve final history outcomes; commit-time trimming is **D** (pending entry is not exposed as retained history). Checked native allocations, both owned sides and overflow rejection remain **D**; transitive GC graph size is not claimed. Stage 8 documents observable event differences. |
| Exact/logical identity | **S** `TransactionCommon.cpp::FPersistentObjectRef` and `OnObjectsReinstanced`: ordinary root pointer, CDO/default-subobject owner/name chain, explicit replacement collector. | Foundation Stage 5 implements exact ordinary handles and explicit checked maps for payloads, caches, annotations and retained history. Generated component matching belongs to Engine instance-source info. Stage 6 adds checked CDO/default-subobject chains, native default-graph admission and an explicit reinstancing hook using the same history participant. Class/owner/property compatibility and atomic publication are stricter **D**; same-name objects never revive exact references. |
| Expiry and destruction | **S** `HasExpired`, buffer `Undo/Redo`: expired custom transactions may be skipped. UE replay also changes garbage state for recorded pending-kill transitions. | Preserve irreversible garbage exclusion **D**; reversible editor absence uses lifecycle records/annotations. Unsupported replacement/module unload retires whole entries atomically. Stage 6/8 must not silently skip partially incompatible entries. |

The inspected source resolves the required baseline questions without a UE
executable probe. Product tests for snapshots, annotations, arrays, replacement,
PIE and barriers remain implementation work, not unresolved reference evidence.
Durin differences above are selected deliberately; the final claim is the
documented alignment boundary, not byte-for-byte UE behavior.

Characterization is grouped into record, history, object-event, annotation and
reinstancing sources under `Engine/Tests/Native/EngineTests/Private/Editor`,
registered in `EditorOperationTests`. Ordinary construction/lifecycle regressions
are in [ComponentInstanceLifecycleTests.cpp](../../Engine/Tests/Native/EngineTests/Private/World/ComponentInstanceLifecycleTests.cpp)
under `WorldTests`; transfer/diff and exact-reference contracts belong to
`CoreTransactionContractTests`. Existing GC, compensation, overflow,
package, filtering, scope and ownership cases are retained; the three named new
cases close disabled-scope, reverse custom ownership and Redo-preservation gaps.
The exact-identity test remains valid for ordinary objects; it must be extended
with explicit logical-reference cases rather than weakened globally in Stage 6.
Generated-reference exclusions are construction guarantees, not a ban on Engine
annotations. Current tests cover virtual Serialize, metadata, annotations and
events; older focused-capture receipts remain historical evidence only.

The Stage 1 non-editor contract-test target must link Core and CoreDObject only.
The Stage 9 runtime gate selects the registered `Win64-Debug-DurinGame` preset
and bounded production targets `Core`, `CoreDObject`, `Engine`, `Sandbox`, and
`RoadWeaver`, plus that contract-test target. These cover both workspace project
consumers and their transitive runtime dependencies without editor linkage.

### Stage 0: Establish the UE 5.8 reference and behavior matrix

Dependency: none. Outcome: a reproducible reference and selected semantics for all
implementation-sensitive decisions; no inherited UE 5.3.2 assumptions.

- [x] Record the starting revision, current architecture gaps, source ownership,
  selected direction, and public UE 5.8 references in this plan.
- [x] Pin an exact UE 5.8 release/build and lawfully accessible source revision or
  executable probe environment. Record file/symbol locations and evidence identity;
  do not vendor UE source or private credentials into this repository.
- [x] Build a behavior matrix covering nested Begin/End/Cancel, disabled scopes,
  first capture/final capture, custom-record versus Modify ownership, array records,
  no-op detection, transient/NonTransactional fields, operation IDs, event ordering,
  annotation timing, package state, barriers, PIE references, and history trimming.
- [x] Distinguish API-verified facts, source/probe-verified behavior, Durin extensions,
  and unresolved questions. Choose serialized-state/flip ownership, focused-record
  coalescing, bound-command rules, and supported reinstancing categories explicitly.
- [x] Establish native characterization tests for current intentional guarantees.
  Identify tests that encode obsolete restrictions and change their expectations
  only when the replacement contract and reference evidence are recorded.
- [x] Resolve unverified behavior through the pinned source or small UE probes.
  Missing evidence blocks only dependent parity decisions; independent work may
  proceed, but neither an affected gate nor final parity can be marked complete.

Completion: the matrix has concrete expected outcomes and evidence for each
required behavior; the remaining implementation work has no hidden baseline choice.

### Stage 1: Establish runtime-neutral transaction contracts and participants

Dependency: Stage 0 contract decisions. Outcome: runtime modules can describe
transaction state and commands without an editor-module dependency.

- [x] Expand Core's transaction boundary for verified capture, finalization,
  operation scope, context/primary object, snapshots, arrays, PIE inspection, and
  object-bound custom changes; preserve appropriate forward-declaration boundaries.
- [x] Define the CoreDObject annotation, identity, and object-change contracts and
  module placement. Keep editor presentation and history policy out of CoreDObject.
- [x] Give single-object and composite records explicit modified-participant data;
  integrate it with ContainsObject, object summaries, hooks, GC, and package effects.
- [x] Migrate interface implementations, test doubles, and command entry points.
  Settle overlapping custom/serialized ownership according to Stage 0, including
  the existing custom-before-Modify and Modify-before-custom cases.
- [x] Establish a non-editor compile target for the new runtime boundary. Register
  any new contract-test target rather than leaving validation to an ad hoc binary.

Completion: owner/participant coverage includes single and multi-object commands;
runtime header contracts compile without DurinEd; temporary adapters are enumerated.

### Stage 2: Implement general object records and transaction serialization

Dependency: Stage 1. Outcome: Modify captures object transaction state through a
general object record rather than only expanding reflected members.

- [x] Implement a transaction archive that invokes virtual object Serialize and
  handles reflected plus custom serialized data with explicit transaction purpose.
  Preserve Transient/NonTransactional filtering and hard/weak/soft reference rules.
- [x] Record supported object metadata changes, including Name, Outer, and relevant
  object/deletion flags, with validated restoration and no irreversible destruction
  resurrection. Package dirty state has one authority, not competing field restores.
- [x] Implement first-before capture, verified final/flip state, no-op comparison,
  reference enumeration, type compatibility, expiry, and overflow-safe accounting.
- [x] Merge focused member and whole-object recording so interleaved nested edits
  retain first-before/latest-after without duplicate application. Do not retain
  live container addresses as a shortcut to serialized state.
- [x] Preserve proposal-validation bypass on historical replay and recovery-aware
  failure propagation. Add tests for custom Serialize data and ordinary object
  identity changes, not only reflected scalar values.

Completion: object-state and focused-state tests cover mixed edits, references,
metadata, failure, and no-op pruning; unsupported states reject before partial restore.

### Stage 3: Add transaction annotations and reconstruction state

Dependency: Stage 2. Outcome: supplemental recovery data belongs to object records
and object hooks rather than a growing set of business-specific snapshot classes.

- [x] Implement annotation capture, final/snapshot state, collector traversal,
  serialization policy, and memory accounting. Pin hook timing to Stage 0 evidence.
- [x] Extend DObject's transaction hooks for annotations while preserving balanced
  completion, compensation, and safe no-annotation defaults.
- [x] Add Engine-owned Actor/Component annotations for required instance state,
  root/attachment reconciliation, and reconstruction. Keep serialized authored state,
  supplemental instance data, and rebuildable derived state distinct.
- [x] Exercise callback-triggered GC, missing participants, generated components,
  failed reconstruction, and multiple restores in one operation without leaks or
  duplicate lifecycle callbacks.
- [x] Close the foundation integration milestone with an editor `all` build and a
  non-editor boundary build. This covers shared Core/CoreDObject/Engine ABI and
  reflection changes across all workspace projects, beyond bounded unit coverage.

Completion: annotations restore required native state; foundational consumers build;
no runtime-to-editor dependency or unowned supplemental reference remains.

### Stage 4: Add object diffs, transaction events, and active snapshots

Dependency: Stages 2-3. Outcome: observers can distinguish actual object changes
during recording, finalization, and Undo/Redo.

- [x] Implement transaction and per-operation identities, object identity metadata,
  changed-property/non-property summaries, and PostTransacted-style notification.
- [x] Add active SnapshotObject behavior with selected-property support. A process
  snapshot must not create another undo entry or replace the original before state.
- [x] Dispatch verified Snapshot, Finalized, and UndoRedo events in a documented
  order alongside property notifications and PreEditUndo/PostEditUndo. Define
  no-op, canceled, custom-command, failed, and compensated-operation behavior.
- [x] Prevent observer reentrancy from invalidating in-flight records. Do not blindly
  apply the current Idle-only listener queue to object hooks that require a live
  transaction context; separate the two delivery contracts where necessary.
- [x] Add event-trace tests for nested edits, repeated snapshots, multiple targets,
  custom participants, annotations, and exceptions.
- [x] Add UE Actor annotation diffable-component additional-change reporting using
  shared instance-source matching, including generated component recreation and
  last-snapshot closure. Do not treat supplemental cache restoration alone as
  evidence of component change events.

Completion: traces match the selected UE 5.8 contracts or explicit Durin extensions;
observers receive accurate modified objects and changed fields exactly as specified.

### Stage 5: Migrate editor edits and structural lifecycle consumers

Dependency: Stage 4. Outcome: ordinary properties use generic transactions, and
structural commands retain only domain behavior that generic records cannot supply.

- [x] Migrate Actor Label to generic object/property recording; remove its dedicated
  history class and special NonTransactional exclusion. Preserve admission, no-op
  behavior, hierarchy revision refresh, saved labels, and Name/Label separation.
- [x] Audit Actor/Component rename, transform, visibility, primary-camera selection,
  attachment, creation, deletion, duplication, and membership. Replace redundant
  snapshots with object records/annotations and retain explicit lifecycle adapters
  where required by verified semantics and native registration contracts.
- [x] Verify whole-object Modify, property UI edits, and structural commands compose
  without suppressing unrelated fields or restoring the same participant twice.
  Retained absent objects keep valid identities and Name reservations, not Labels.
- [x] Migrate MaterialEditor graph operations, property sessions, CallInEditor,
  LevelEditor panels, tools, Python scopes, and exposed script adapters. Give each
  remaining custom command a concrete domain rationale and declared participants.
- [x] Preserve Tick, attachment, render/physics registration, selection validity,
  saved checkpoints, and the example's persistent Tags across repeated Undo/Redo.
  Object field restoration alone is not structural lifecycle completion.
- [x] Update stubs, examples, tests, and owning contracts with each migrated API.

Completion: ordinary Label and property edits have no bespoke history representation;
structural and material workflows share correct recording and completion contracts.

### Stage 6: Support controlled reinstancing and template transactions

Dependency: Stages 3-5. Outcome: supported object replacement can preserve applicable
history without resolving unrelated stale objects by Name.

- [x] Add exact versus logical/subobject transaction reference categories and
  explicit old-to-new replacement integration. Validate class, owner, hierarchy,
  and property compatibility before switching a retained participant graph.
- [x] Remap targets, serialized hard references, annotations, custom-command
  participants, primary objects, and package tracking as one checked operation.
  Define expiry/retirement for incompatible or unmapped custom records.
- [x] Add admitted CDO/default-subobject transactions where needed to match the
  pinned UE categories. Coordinate with default-graph and archetype rules; do not
  remove global template protections or silently expose CDO mutation to scripts.
- [x] Preserve current atomic reload retirement for unsupported changes. Define
  when verified replacement preserves history and when whole-entry retirement is
  required, including cross-package entries and invalidated save checkpoints.
- [x] Test CDO/subobject recreation, compatible and incompatible replacement,
  same-name impostors, slot reuse, partial maps, failed preparation, and GC before
  and after commit. No half-remapped transaction may become executable.

Completion: supported reinstancing preserves correct history; unsupported cases
retire or expire deterministically without stale references, data loss, or false clean state.

### Stage 7: Add array operation records and bounded capture costs

Dependency: Stages 2, 4, and 6. Outcome: small supported array operations need not
serialize a complete large container for every history entry.

- [x] Implement typed array range/operation records through the Core capture
  boundary, adapting to Durin reflection and std::vector ownership. Handle insert,
  remove, value replacement, nested object references, and allocation relocation.
- [x] Define interaction with whole-object snapshots and multiple array operations
  on one owner. Keep an explicit safe full-snapshot fallback for unsupported cases;
  do not imply that ordinary std::vector mutations intercept themselves.
- [x] Migrate eligible direct spline/road array edits through explicit mutation
  adapters. Audit property-container and graph paths; preserve whole-object
  proposal/domain fallbacks where callbacks or semantic rollback affect siblings.
  Evaluate maps separately; do not infer map semantics from UE SaveArray.
- [x] Compare owned bytes, allocations, and capture/replay cost for fixed small
  edits in 10,000- and 100,000-element arrays. For supported insert/remove records,
  payload growth must track changed elements plus bounded metadata, not full N.
- [x] Test repeated edits, resizing, nested scopes, cancellation, object lifetime,
  mixed record kinds, no-ops, overflow, and bounded history eviction.

Completion: supported edits have measured bounded storage and correct replay;
fallback behavior and remaining full-container costs are documented honestly.

### Stage 8: Align transactor management and retire obsolete policy

Dependency: Stages 4, 6, and 7. Outcome: buffer controls and recording semantics
follow the verified UE 5.8 baseline with named, tested Durin extensions.

- [ ] Complete verified Begin/End/Cancel, scope lifetime, primary-object/context,
  operation scope, no-op/transient entry, and custom StoreUndo semantics. Resolve
  the current implicit standalone StoreUndo extension explicitly.
- [ ] Implement Undo Barrier stack behavior and PIE-reference inspection/policy.
  Map each control to a real editor caller or a native contract test.
- [ ] Align buffer budget configuration, eviction timing, branch replacement,
  reset reasons, and observable buffer events. Preserve checked accounting and
  distinguish native record bytes from the transitive retained object graph.
- [ ] Reconcile package dirty/checkpoint handling, external writes, module unload,
  reload retirement, exceptions, and RecoveryRequired with the new object records.
  Cancel behavior and replay failure compensation remain separate concepts.
- [ ] Remove superseded adapters and old UE 5.3.2 normative assumptions from active
  contracts. Keep historical archive documents as history, not current authority.

Completion: buffer semantics have reference-backed tests; retained deviations have
an explicit reason, owner, and observable behavior rather than an accidental fallback.

### Stage 9: Complete workspace acceptance and publish the new contracts

Dependency: Stages 0-8. Outcome: the final implementation, consumers, evidence, and
documentation agree on the pinned UE 5.8 alignment boundary.

- [ ] Close every row of the Stage 0 matrix with implementation/test evidence or an
  explicitly selected Durin extension. Required unresolved rows prevent completion.
- [ ] Run final default-editor `all` integration coverage for the final ABI, reflected
  metadata, and consumers across Engine, Sandbox, and RoadWeaver. Reuse valid
  receipts only when the relevant inputs and environment satisfy the test workflow.
- [ ] Validate the final runtime boundary in the registered Debug DurinGame preset:
  a bounded Core/CoreDObject/Engine/project consumer build proves that moving
  transaction contracts did not introduce an editor dependency. Stage 0 selects
  the exact target closure. No extra Release/Shipping or graphics preset is implied.
- [ ] Run the required native acceptance matrix below, with registry-based selection
  and isolated runs for state-sensitive new cases. Resolve failures and account for
  any uncovered consumer before claiming parity.
- [ ] Complete an editor smoke covering Label rename, transform drag and cancel,
  multi-object edit, structural undo, save/Undo/Redo dirty state, and the Python
  example's create/Undo/rerun/duplicate rejection. Application-hosted automation,
  when used for this gate, follows its documented host admission workflow.
- [ ] Update the owning Core/Runtime/Editor contracts, script guides, API examples,
  and test registry. The plan must not remain the only source of implemented rules.
- [ ] Record final evidence applicability, remove temporary adapters, validate docs
  and plan lifecycle, then mark the plan Completed only after every required gate.

Completion: a clean final handoff has no hidden legacy capture path, unresolved
required parity claim, untracked migration consumer, or unsupported validation claim.

## Validation And Evidence

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection/result reuse](../Agents/Testing.md). Do not copy or bypass the
host DevTool launcher. Re-query the registry at execution time; current target
names below identify existing coverage, not an immutable test inventory.

| Acceptance area | Required coverage and current anchors |
| --- | --- |
| Recording, custom ownership, GC, recovery, checkpoints | EditorOperationTests, especially TransactionRecordTests; new object/archive/annotation contract cases |
| Whole-object UI edits, containers, callbacks, gestures | EditorPropertyTests, CorePropertyChangeTests, CorePropertyValueSnapshotTests |
| Actor/Component lifecycle and metadata | LevelMutationTests, WorldTests, SkyBoxTests, SplineTests, ViewportTests |
| Material and graph consumers | MaterialGraphEditingTests, MaterialEditorInteractionTests, MaterialEditingPersistenceTests; extend selection for changed material/asset consumers |
| Object replacement and saved graph integrity | AssetPackageReloadTests, relevant CoreDObject default/serialization/GC suites, new reinstancing cases |
| Script and editor adapters | PythonReflectionIntegrationTests, PythonToolIntegrationTests, PythonScriptPanelTests, affected CallInEditor/tool and shell cases |
| Runtime layering and workspace ABI | Foundation integration gate in Stage 3; final editor all build and bounded Debug DurinGame consumer gate in Stage 9 |
| UE 5.8 semantic compatibility | Pinned reference matrix, source/probe evidence, event traces, and assertions for each migrated contract |

The native matrix must include cancellation without assumed rollback; initial
mutation versus history-storage failure; custom-before/after-Modify ownership;
no-op operations; Transient and NonTransactional fields; interleaved object,
property, and array changes; weak/soft/hard references; replay compensation;
snapshot identity and event ordering; reconstruction; and saved-state branch changes.

Record concise acceptance summaries in Current Status with revision, selection,
profile, environment, and applicability to final inputs. Store raw reports, probes,
traces, inventories, and benchmarks under ignored `Build/TransactionUE58/` or CI
artifacts, with enough information to reproduce them. Keep required contract
decisions and result summaries in versioned documentation rather than only logs.

No GPU qualification is required merely because transactions reference renderable
objects. If implementation changes an actual graphics/resource lifecycle contract,
select the affected integration gate explicitly and follow its host/resource rules.
Validate changed docs and the all-plan lifecycle as required by the documentation
workflow before each plan handoff.
