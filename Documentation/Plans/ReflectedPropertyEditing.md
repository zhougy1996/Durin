# Reflected Property Editing Simplification Plan

Last reviewed: 2026-07-23

## Current Status

The shared reflected-property view, edit sessions, snapshots, notifications,
transactions, bindings, and semantic mutation adapters are implemented. The
remaining UI migration and validation work is now secondary to simplifying the
mutation model itself.

Phases 0 through 4 are complete: adapter semantics are classified, proposal
storage is owned by an internal property draft, active sessions/transactions
re-resolve ephemeral leaf storage, and generic mutations now pass through
detached object validation/normalization with rollback protection. All built-in
semantic adapters have been migrated to object hooks, leaving no proven policy
exception. Sessions now use one atomic mutation executor, failed restoration
remains retryable, every interaction emits one terminal event, and transactions
resolve any registered exceptional policy when Undo/Redo executes. Phase 5 is
next but has not started.

The remaining simplification work is concentrated at the host and public API
boundaries. Host transitions still need one enforced cancellation policy, and
the now-unused built-in adapter surface remains available until final cleanup.
Those boundaries are narrowed before more property-specific behavior is added.

The implemented architecture remains documented in
[Reflected Property Editing](../Architecture/ReflectedPropertyEditing.md) until
each stage below lands. Earlier view API exploration remains in
[Reflected Property View Evolution](../Reference/ReflectedPropertyViewEvolution.md).

## Goal

Make the normal property-editing path understandable as one flow:

```text
widget -> detached draft -> edit session -> object mutation hooks -> transaction
```

Most reflected properties must use the same generic, atomic apply path. The
object validates or normalizes a detached candidate before storage changes and
reacts to the applied value afterward. A property mutation policy remains only
for mutations that genuinely cannot be expressed by generic reflected storage
plus object hooks.

## Scope

- Hide scratch allocation, construction, and cleanup behind an internal
  detached property-draft abstraction.
- Make edit targets and committed transactions retain only stable object,
  member, path, operation, and snapshot identity.
- Add a pre-apply object hook for validation and normalization of a detached
  proposal, and retain `PostEditChangeProperty()` as the unified reaction hook.
- Replace the adapter-on-every-edit model with a generic mutation path and a
  narrowly scoped `IPropertyMutationPolicy` escape hatch.
- Migrate existing transform, camera, spline, static-mesh, material-slot, and
  material-parent semantics to the simplest valid mechanism.
- Define atomic failure, cancellation, host-transition, Undo, and Redo behavior.
- Remove bypasses, legacy target fallback, and registry precedence rules that
  are no longer required after migration.
- Close automated and editor-smoke validation gaps for the simplified flow.

## Non-Goals

- Observing arbitrary runtime C++ assignments outside the editor pipeline.
- Moving transaction history or editor UI dependencies into `DObject`.
- Replacing domain-specific widgets, object customizations, or stable container
  bindings with a single generic presentation.
- Adding generated property callback metadata before the runtime contract is
  stable and the remaining policy use cases are measured.
- Restoring package dirty state when Undo reaches the last saved revision.
- Making all setters editor-only or requiring gameplay code to use reflection.

## Design Decisions and Invariants

### Draft, not UI context

The current scratch value is a detached, complete candidate for the stable
snapshot root. It exists so a leaf widget can edit nested or container data
without touching live object storage. It is not widget state and is not an
additional public layer.

The implementation will call this internal concept a property draft. The view
may create and edit a draft, but allocation, reflection construction,
leaf-resolution, snapshot capture, and destruction belong to one RAII type.
Public proposal callbacks receive only the resolved draft leaf for the duration
of the callback.

### Stable targets only

An edit target is stable identity, not cached storage. It retains the rooted
object, object-owned member, owned member-to-leaf path, mutation kind, and any
stable key/index data required by that path. The current leaf container and
other addresses derived from array or map storage are resolved immediately
before capture or apply and are never retained by an active transaction.

Missing snapshot-root or container data is an invalid target. `Begin()` must
not silently synthesize these values from a leaf as a compatibility fallback.

### One default mutation pipeline

The normal apply sequence is:

```text
resolve stable target
capture current value
validate and normalize detached draft through PreEditChangeProperty
write reflected storage once
recapture the actual value
notify PostEditChangeProperty with the actual applied path/value context
```

The pre-apply hook operates on detached data and may reject or normalize it
without mutating the live object. The post-apply hook cannot reject the edit or
change reflected canonical storage; it updates caches, render state,
dependencies, or other derived runtime state. Every live value change uses this
ordering. Terminal Commit, Cancel, Undo, and Redo notifications retain explicit
phase and origin without reapplying an already committed value.

If validation, target resolution, policy application, or snapshot capture
fails, the operation restores the captured value before returning failure and
does not notify, dirty the package, or enter transaction history. Restoration
failure is surfaced as a hard editor error and the session remains recoverable;
destruction must not silently discard an applied preview whose cancel failed.

### Mutation policy is an escape hatch

`IPropertyMutationPolicy` replaces the semantic role of
`IReflectedPropertyMutationAdapter`, but is not selected for every edit. It is
allowed only when a mutation cannot be made atomic through detached validation,
one reflected write, and post-apply reconstruction. Examples may include an
external subsystem that owns the canonical value or a setter whose atomic
effects cannot be decomposed into validation and reaction hooks.

Policies are stateless, process-lifetime services. Registration uses validated
class/member identity and deterministic most-derived selection; reverse
registration order must not define behavior. Transactions retain stable target
identity and snapshots, not raw leaf addresses or an arbitrary adapter pointer.

### One semantic owner per rule

- Candidate rejection and normalization belong in the pre-apply object hook.
- Reflected storage assignment belongs in the generic mutation implementation.
- Cache, scene, render, and dependency refresh belong in the post-apply hook.
- Presentation, ranges, and interaction state belong in the view/customization.
- Undo/Redo ordering and dirty-state policy belong in the transaction layer.
- A mutation policy may own a rule only when the generic split is impossible,
  and the reason must be recorded beside its registration.

### Explicit interaction termination

Every host transition follows one documented policy and reports failure:

| Transition | Active edit result |
| --- | --- |
| Widget confirms or loses edit focus normally | Commit |
| User presses Escape | Cancel |
| Inspected object or document is replaced | Cancel |
| View becomes read-only or PIE starts | Cancel |
| Workspace or panel closes with an active preview | Cancel |
| Editor shutdown with an unfinished preview | Cancel |

An edit that returns to its original value still receives the terminal event
selected by the user action: normal completion is `Committed`, while Escape or
a cancelling host transition is `Cancelled`. Every begun interaction therefore
has exactly one terminal outcome even when it creates no history entry.

## Current Foundations and Gaps

### Foundations to preserve

- Focused property snapshots retain referenced objects.
- Member-to-leaf paths survive container resize and map rehash.
- One continuous interaction creates at most one transaction.
- Stable bindings re-resolve logical map values for every operation.
- `FPropertyChangedEvent` already distinguishes phase, kind, origin, member,
  leaf, and path.
- `FEditorTransactionManager` remains host-owned and shared across editor views.

### Gaps to remove

- Proposal decoding currently applies a candidate temporarily to live storage
  and then restores it before calling some setters.
- Targets mix stable snapshot identity with ephemeral leaf/container addresses.
- `Begin()` contains compatibility fallback for incomplete targets.
- Every edit pays for an adapter abstraction even when generic reflection is
  sufficient.
- The material-slot adapter combines semantic setter calls with a direct array
  rewrite to restore shape.
- Adapter failure does not state or enforce a fully atomic contract.
- Session destruction ignores cancellation failure before resetting state.
- `bUseTransaction == false` bypasses sessions, semantic mutation,
  notification, cancellation, and dirty-state behavior as one bundle.
- Host transition behavior and no-op terminal notification are inconsistent.
- Adapter lookup outcome depends on reverse registration order.

## Implementation Stages

### Stage 0: Classify Existing Mutation Semantics

- [x] Record, for every registered adapter, its rejection/normalization rules,
  live storage write, derived-state reaction, and Undo/Redo requirements.
- [x] Classify each rule as pre-apply validation, generic storage write,
  post-apply reaction, view presentation, or a justified policy exception.
- [x] Prove whether any current property requires `IPropertyMutationPolicy`.
  Do not preserve an adapter merely because a setter exists today.
- [x] Add assertions or focused tests that reproduce partial-apply rejection,
  cancellation failure, edit-away-and-back, and container invalidation risks
  before changing the contracts.

#### Classification Result

| Current registration | Validation/normalization | Reflected write | Derived-state reaction | Selected destination |
| --- | --- | --- | --- | --- |
| `DSceneComponent.RelativeTransform` | Normalize quaternion | Assign complete transform | Recompute component-to-world | Pre/Post hooks |
| `DCameraComponent.ProjectionSettings` | Clamp FOV, clip planes, and aspect ratio as one candidate | Assign complete settings | None beyond canonical state | Pre hook + generic write |
| `DSplineComponent.SplineCurve` | Clamp reparameterization steps | Assign complete curve | Rebuild spline cache | Pre/Post hooks |
| `DStaticMeshComponent.StaticMesh` | Enforce object type through reflected metadata | Assign object reference | Mark render state dirty | Generic write + Post hook |
| `DStaticMeshComponent.Material` | Enforce material type | Keep legacy slot-zero mirror coherent | Bind/unbind material and mark render state dirty | Pre/Post hooks |
| `DStaticMeshComponent.Materials` | Enforce every element type | Assign exact array shape and slot-zero mirror | Bind/unbind changed materials and mark render state dirty | Pre/Post hooks |
| `DMaterialInstance.Parent` | Enforce type and reject parent cycles | Assign parent reference | Maintain dependency links and invalidate render data | Pre/Post hooks |

No current built-in adapter requires a retained mutation-policy exception. All
canonical values are reflected storage owned by the edited object, and their
setter behavior can be split into detached validation/normalization plus
post-write reaction. `IPropertyMutationPolicy` remains an uninstantiated escape
hatch for a future externally owned canonical value.

#### Acceptance Gate

- Every current adapter has one selected destination and every retained policy
  has a written reason that generic apply plus object hooks cannot satisfy.
- The failure cases being simplified are covered by tests that fail against the
  unsafe behavior or explicitly capture the current behavior to be changed.

### Stage 1: Introduce Stable Targets and Internal Drafts

- [x] Add one RAII property-draft implementation that owns reflected temporary
  storage and exposes scoped leaf resolution.
- [x] Move scratch construction and proposal-to-snapshot capture out of
  `FReflectedPropertyView` recursion helpers into that implementation.
- [x] Remove persisted leaf-container addresses from transaction state and
  re-resolve the target path for every capture, apply, Cancel, Undo, and Redo.
- [x] Make target factories construct complete valid targets and delete the
  `Begin()` snapshot/container fallback.
- [x] Keep bindings as stable logical target factories; do not expose draft or
  path internals to Material Editor or Details customizations.

#### Acceptance Gate

- No proposal construction writes live object storage.
- Resizing or rehashing a container between frames cannot invalidate an active
  session or committed transaction.
- Invalid or incomplete targets fail at construction/begin with a reported
  error and no live mutation.

### Stage 2: Establish the Generic Object Mutation Hooks

- [x] Define the detached pre-apply proposal contract, including mutable
  normalization, rejection error reporting, property path, mutation kind,
  phase, and origin.
- [x] Implement the atomic generic apply sequence and its rollback guard.
- [x] Route `PostEditChangeProperty()` through the same path for Interactive,
  Commit, Cancel, Undo, and Redo without allowing post-notification rejection.
- [x] Ensure the snapshot recorded after apply reflects normalization and any
  deliberate cross-field changes, not merely the widget's raw proposal.
- [x] Specify and test reentrancy: object hooks may update derived state but may
  not start a nested edit of the same target.

#### Implementation Result

`FPropertyEditProposal` exposes borrowed detached root and resolved leaf
storage together with path, kind, phase, and origin. Map removal and key rename
are the explicit structural exception: when the old leaf no longer exists in
the candidate, the leaf container is null while the complete mutable draft root
remains available.

The generic mutation path captures live state, restores the candidate only into
a draft, invokes `PreEditChangeProperty()`, captures the normalized draft,
writes reflected storage once, and recaptures the actual value. A failed write
or recapture attempts rollback before returning without post notification,
dirtying, or history. Apply, Cancel, Undo, and Redo use this path; Commit emits
the terminal post event without replaying the already-applied value. A
thread-local target guard rejects same-target nested edits from object hooks.

#### Acceptance Gate

- A generic scalar, nested struct field, array element, and map value all use
  the same atomic hook pipeline.
- Rejection and normalization happen before live mutation; failed apply leaves
  value, notifications, dirty state, and history unchanged.
- Cancel and Undo/Redo produce the same derived object state as a fresh apply.

### Stage 3: Migrate Semantic Adapters

- [x] Move `RelativeTransform` validation and scene/render reactions into the
  object hook split while preserving `SetRelativeTransform()` behavior.
- [x] Move camera projection cross-field normalization into detached pre-apply
  handling and confirm there is no separate projection cache to refresh.
- [x] Move spline validation and curve-cache rebuilds into the hook split.
- [x] Move static-mesh assignment and material-slot resource reactions into
  post-apply handling without setter-plus-direct-storage dual writes.
- [x] Move material-parent cycle rejection into pre-apply handling and material
  invalidation into post-apply handling.
- [x] Replace any proven exceptions with narrowly registered
  `IPropertyMutationPolicy` implementations and delete migrated adapters.
- [x] Make policy resolution independent of registration order and add
  base/derived-class selection coverage.

#### Acceptance Gate

- Each migrated property preserves setter rejection, clamping, cache rebuild,
  scene/render refresh, Cancel, and Undo/Redo behavior.
- The generic path is used unless a Stage 0 policy exception was proven.
- No implementation decodes a proposal by temporarily writing live storage.

#### Implementation Result

The seven former built-in registrations now use the generic atomic path.
Transform, camera, spline, material-array, and material-parent pre hooks read
and normalize or reject detached draft roots directly. Post hooks rebuild
transform/spline derived state and reconcile static-mesh material bindings,
slot-zero compatibility storage, render state, and material parent dependency
links for Interactive, Cancel, Undo, and Redo.

No current property required an `IPropertyMutationPolicy`. The legacy adapter
registry remains only as a temporary exceptional/compatibility API pending the
Stage 6 surface cleanup; it has no built-in registrations. Its lookup now walks
most-derived to base class, with coverage proving that reverse registration
order does not change selection.

### Stage 4: Simplify Session and Transaction Semantics

- [x] Make session Apply, Commit, and Cancel delegate to one atomic mutation
  operation rather than duplicating adapter, snapshot, and notification rules.
- [x] Define a recoverable session state for failed Apply or Cancel; do not
  clear the target or original snapshot until live restoration succeeds.
- [x] Replace `bUseTransaction` with explicit APIs for editable transactional
  values versus genuinely read-only/display-only values. No writable path may
  bypass validation and notification as a side effect of disabling history.
- [x] Store stable target identity and before/after snapshots in transactions;
  resolve generic mutation or the registered policy at execution time.
- [x] Emit one terminal event for every begun interaction, including an edit
  that ends at its original value, while keeping no-op history empty.

#### Implementation Result

Apply, Commit, Cancel, Undo, and Redo now share one mutation executor that
resolves stable targets, applies generic or exceptional semantics, captures the
actual result, rolls exceptional partial failure back, and owns post-event
ordering. A failed Apply or Cancel updates the session only from observable live
state and keeps the stable target and original snapshot until restoration can
be retried. Destruction reports a fatal editor error if its final safety cancel
cannot restore the preview.

The earlier `bUseTransaction` bypass is no longer present. Writable submissions
always enter the session, validation, notification, and cancellation pipeline;
`bReadOnly` is the explicit display-only mode, while the host transaction
manager controls history ownership without disabling mutation semantics.

Transactions retain only stable target identity and snapshots. Exceptional
adapters must be registered by class/member identity before a transactional
edit can commit, and Undo/Redo resolves that registration at execution time.
Commit and Cancel deliver a terminal event even when the final snapshot equals
the original, without dirtying the package or adding no-op history.

#### Acceptance Gate

- One interaction has zero or more Interactive events and exactly one
  Committed or Cancelled terminal event.
- A failed cancel remains visible/retryable and cannot be silently abandoned by
  the session destructor.
- No writable reflected-property UI bypasses object hooks, error reporting, or
  cancellation merely to avoid recording history.

### Stage 5: Make Host Lifecycle Policy Explicit

- [ ] Apply the termination table to Details selection changes, Material Editor
  document changes, workspace activation/closure, PIE read-only transitions,
  panel destruction, and editor shutdown.
- [ ] Propagate commit/cancel errors through the host error callback and prevent
  a document or target transition when live preview restoration failed.
- [ ] Remove host-specific fallback behavior that resets a view without a
  deliberate terminal action.

#### Acceptance Gate

- Automated host-level tests cover object/document replacement and read-only
  transitions during an interaction.
- Manual smoke checks confirm the same behavior for workspace close, PIE, and
  editor shutdown, with no preview mutation left uncommitted or unrestored.

### Stage 6: Consolidate API, Tests, and Documentation

- [ ] Remove obsolete adapter types, registry APIs, scratch helpers, raw target
  fields, fallback branches, and comments describing the old model.
- [ ] Keep generated property metadata deferred unless policy registration is
  still repetitive after migration; if evaluated, resolve validated member
  identity at generation time without per-edit string lookup.
- [ ] Update the architecture document only after each new contract is
  implemented and validated.
- [ ] Complete the validation matrix and run an editor smoke test through the
  repository build/run workflow.

#### Acceptance Gate

- Public headers expose only concepts required by hosts or customizations.
- Architecture documentation describes the implemented flow and contains no
  adapter-on-every-edit or persistent raw-leaf assumptions.
- Full build, native tests, and hidden-window editor smoke validation pass.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Draft isolation | Nested, array, and map proposals do not mutate live storage before Apply |
| Generic values | Scalar, enum, string, object, math struct, and nested fixed-array edits |
| Validation | Rejection and normalization before mutation, including parent-cycle errors |
| Derived state | Transform, camera, spline, static-mesh, material-slot, and material invalidation |
| Interaction | Continuous preview, confirm, Escape, and edit-away-and-back terminal event |
| Failure atomicity | Apply failure, rollback failure reporting, Cancel retry, and destructor safety |
| Containers | Resize/rehash between frames, structural Undo/Redo, map-key collision |
| Policy lookup | Generic default, justified exception, base/derived selection, registration order independence |
| Host lifecycle | Selection, document, workspace, read-only/PIE, shutdown |
| History/dirty | No history or dirtying for rejection/cancel; one entry per committed interaction |
| Editor smoke | Details edit/save/Undo/Redo, Material Editor switch, PIE, close, shutdown |

## Definition of Done

- A normal reflected property edit can be explained and traced as draft,
  generic atomic mutation, object hooks, and transaction without an adapter.
- Scratch storage is entirely internal to the draft implementation and no live
  object storage is used to decode a proposal.
- Transactions and sessions retain no ephemeral container or leaf addresses.
- Existing semantic properties preserve validation and runtime side effects,
  with mutation policies limited to documented, tested exceptions.
- Apply and Cancel failure cannot leave an untracked preview or silently reset
  session state.
- All host transitions have a deliberate commit/cancel outcome.
- The validation matrix, full build, native tests, and hidden-window editor
  smoke test pass.
- Long-lived implemented rules have moved into Architecture documentation and
  this plan's status is marked complete.

## Deferred Follow-ups

- Generated property-specific validation/reaction metadata, after policy use is
  measured on the simplified implementation.
- Saved-revision-aware package dirty-state restoration.
- Multi-object editing and copy/paste of property drafts.
- Cross-object atomic transactions.

## Related Documentation

- [Reflected Property Editing](../Architecture/ReflectedPropertyEditing.md)
- [Reflected Property View Evolution](../Reference/ReflectedPropertyViewEvolution.md)
- [Native Tests](../Setup/NativeTests.md)
- [Build and Run](../Setup/BuildAndRun.md)

## Related Code

```text
Engine/Source/Runtime/CoreDObject/Public/DObject/PropertyChange.h
Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h
Engine/Source/Editor/DurinEd/Public/Editor/ReflectedPropertyEditing.h
Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyEditing.cpp
Engine/Source/Editor/DurinEd/Public/Editor/ReflectedPropertyView.h
Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyView.cpp
Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h
Engine/Source/Programs/Tests/EngineTests/Private/ReflectedPropertyEditingTests.cpp
```
