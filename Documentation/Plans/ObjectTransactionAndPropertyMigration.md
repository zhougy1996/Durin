# Object Transaction And Property Migration Plan

Summary: Add executable root-free property object records and migrate reflected property history onto the editor transactor without advancing the global cutover.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

P0 and P1 are complete. `DTransBuffer` owns bounded GC-visible data history and
can validate focused records, but it does not yet apply before/after values.
`FPropertyEditSession` and `FPropertyTransaction` still store rooted legacy
`FPropertyValueSnapshot` values and commit the executable transaction directly
to `FTransactionManager`.

P2 is complete. The legacy manager remains the application-facing ordered
history until P4, with a data-free legacy bridge preserving ordering and
package revisions while authoritative property records, application, and
collector edges live in `DTransBuffer`.

## Goal

Deliver executable, retention-neutral reflected-property records that:

- preserve the validated detached mutation and notification pipeline for
  before/after application;
- retain targets and hard values only through `DTransBuffer` collector traversal;
- let an active edit keep its before value reachable without snapshot roots;
- preserve current interactive, cancel, container-path, normalization, package
  revision, and application Undo/Redo behavior; and
- coexist with legacy command transactions through an ID-only bridge until P4.

## Scope

This plan changes CoreDObject snapshot payload utilities, DurinEd transactor and
property editing code, property-view host wiring, and focused editor tests. It
may add expected-ID transition and record-update/removal operations required by
the P2 bridge.

## Non-Goals

- Do not migrate non-property `ITransaction` consumers; P3 owns custom changes
  and command migration.
- Do not route application commands, Activity History, or workspace adapters
  directly to `DTransBuffer`; P4 owns the global cutover.
- Do not add object creation/deletion records or resurrection.
- Do not add general asynchronous transaction execution. A history restore
  whose validation requests deferral fails synchronously; P3 owns async custom
  completion and module leases.
- Do not remove `FTransactionManager`, its package revisions, or its command
  event surface in P2.

## Selected Decisions

### Root-Free Active And Committed Values

- Property editing uses `FPropertyValueSnapshotPayload` for original, current,
  map-key, and transaction before/after data. Payload equality restores detached
  values for semantic comparison and falls back to encoded identities on decode
  failure.
- `FPropertyValueSnapshot` remains a source-compatible legacy adapter for
  callers outside the migrated path, but neither `FPropertyEditSession` nor
  committed property history owns it.
- Beginning a transacted edit inserts one provisional before=after object record
  into the pending `FTransaction`. That record makes the original hard values
  collector-visible while the live object changes. Each successful Apply
  replaces the provisional record's after payload.
- The non-reflected session may keep its target rooted while it owns borrowed
  live container addresses. The transaction system adds no target or value
  roots; removing the session root is a separate ownership redesign.

### Stable Property Object Records

- A property object record owns an exact target reference, stable top-level
  member locator, member-to-leaf path descriptors, change kind, logical
  identity, and retention-neutral before/after payloads.
- Records reconstruct a transient `FPropertyEditTarget` only after resolving
  the exact target and verifying the snapshot member and stored path metadata.
  They never retain a live value-container or leaf address.
- Undo applies records in reverse order; Redo applies them forward. All records
  validate before the first write. A failed apply rolls back already-applied
  records before returning failure and does not move the history cursor.
- Application uses the same detached draft, `PreEditChangeProperty`, extension,
  live write, recapture, `PostEditChangeProperty`, path, kind, and origin
  pipeline as interactive edits. Transaction restore cannot accept deferred
  validation in P2.

### P2 Legacy Ordering Bridge

- `FPropertyTransaction` becomes a snapshot-free bridge containing only the
  expected transactor transaction ID, exact transactor identity, description,
  and affected package pointer.
- Bridge Undo/Redo calls expected-ID transitions on `DTransBuffer`. A mismatch
  fails without moving either history.
- Destroying a bridge removes its matching property entry from the transactor.
  Legacy redo replacement, eviction, package forgetting, Clear, and shutdown
  therefore release the authoritative record and collector edges too.
- `FTransactionManager` continues to own application ordering, package
  before/after revisions, saved checkpoints, dirty synchronization, and public
  events. The P2 buffer event queue remains internal.
- Property-view hosts supply both `DTransactor` and `FTransactionManager` until
  P4. A session without a transactor edits and notifies normally but creates no
  property history; it never falls back to rooted committed snapshots.

## Implementation Stages

### Stage 0: Freeze Payload And Object-Record Contracts

- [x] Add semantic equality and explicit legacy-payload access without adding
  retention to `FPropertyValueSnapshotPayload`.
- [x] Define stable property path and before/after object-record data without
  live container addresses or executable module callbacks.
- [x] Extend the transaction envelope, validation, collector traversal, and
  overflow-safe accounting for property records.
- [x] Add expected-ID transitions plus pending record update and retained entry
  removal contracts required by scoped sessions and the legacy bridge.
- [x] Cover payload equality, class identity, move-only data, accounting, and
  wrong-ID invariants.

Stage 0 is complete when property records are root-free data, their collector
and accounting sources are singular, and the transactor can address one pending
or retained record without weakening P1 barriers.

### Stage 1: Apply Property Records Through The Validated Pipeline

- [x] Reconstruct a transient target from exact object identity, member locator,
  and owned member-to-leaf path data.
- [x] Apply before/after payloads through detached validation, normalization,
  atomic live write, recapture, hooks, and synchronous notification.
- [x] Implement reverse Undo, forward Redo, prevalidation, partial-failure
  rollback, and cursor preservation.
- [x] Preserve value, struct, fixed/dynamic array, map key/value, structural
  kind, logical identity, and hard/weak/soft reference behavior.
- [x] Cover incompatible paths, stale/garbage targets, rejected hooks,
  normalized values, notification phase/origin, and deferred-restore rejection.

Stage 1 is complete when structural transitions have become executable object
transitions without bypassing any current property mutation invariant.

### Stage 2: Migrate Sessions And Preserve Legacy Ordering

- [x] Move session original/current/map-key storage to retention-neutral payloads
  and open/update/cancel one scoped transactor record per logical edit.
- [x] Convert `FPropertyTransaction` to an ID-only expected-head bridge and make
  bridge destruction remove its authoritative transactor entry.
- [x] Supply transactor plus legacy manager through `FPropertyViewContext` and
  every production Details/material/texture property-view host.
- [x] Preserve continuous preview coalescing, no-op, cancel, destructor safety,
  deferred interactive validation, exact paths, container edits, custom rows,
  and multi-object call sites.
- [x] Preserve legacy package revision, saved-state, dirty-state, mounted-content,
  and event behavior through the bridge.

Stage 2 is complete when production property edits are stored and applied only
by the new buffer while the existing application Undo/Redo and save surfaces
remain behaviorally unchanged.

### Stage 3: GC, Coexistence, Documentation, And Qualification

- [x] Prove pending and committed before/after hard references survive through
  collector traversal while weak/soft values do not gain retention.
- [x] Prove cancel, bridge redo replacement, legacy eviction/forget/Clear,
  transactor eviction/Reset, and shutdown release property edges.
- [x] Prove property history owns no `AddToRoot`, `FScopedObjectRoot`, rooted
  snapshot, callback, or module-owned deleter.
- [x] Update the reflected-property and transactor architecture contracts and
  mark P2 complete in the roadmap with exact coexistence boundaries.
- [x] Run `EditorPropertyTests`, `EditorOperationTests`, `EditorShellTests`, and
  `CoreObjectTests`, followed by the complete registered-profile build.
- [x] Run changed-document, all-document, all-plan, and all-roadmap validation
  and record exact evidence in this plan.

Stage 3 is complete when existing property behavior passes through root-free
records, the legacy bridge preserves user-visible ordering and revisions, and
all removal and shutdown paths release collector edges.

## Acceptance Gates

- [x] Active and committed property before/after values use retention-neutral
  payloads; transaction-owned manual roots are absent.
- [x] Undo/Redo applies exact targets and paths through validation, hooks,
  notifications, and atomic rollback without moving the cursor on failure.
- [x] Interactive preview, normalization, no-op, cancel, continuous coalescing,
  struct/array/map edits, logical identities, and custom property rows remain
  covered.
- [x] Target and hard values survive only through pending/retained transactor
  collector traversal; weak/soft values and every removal path behave correctly.
- [x] The legacy bridge contains no property value data and preserves package
  revisions, save checkpoints, dirty state, application event ordering, and
  expected-head failure behavior.
- [x] Legacy command consumers remain on `FTransactionManager`; no P3 command
  migration or P4 global cutover occurs in this plan.

## Validation

Use the repository agent build and native-test workflows. The required P2 lane
is `EditorPropertyTests`, `EditorOperationTests`, `EditorShellTests`, and
`CoreObjectTests`, followed by the complete registered-profile build and all
applicable documentation validators.

Completion evidence on `macos-xcode-arm64` / `MacOS-arm64-Debug-DurinEditor`:

- `./DevTool test EditorPropertyTests --agent`: 32/32 passed.
- `./DevTool test EditorOperationTests --agent`: 36/36 passed.
- `./DevTool test EditorShellTests --agent`: 47/47 passed.
- `./DevTool test CoreObjectTests --agent`: 85/85 passed.
- `./DevTool test MaterialTests --agent`: 108/108 passed.
- `./DevTool test SplineTests --agent`: 40/40 passed.
- `./DevTool test WorldTests --agent`: 106/106 passed.
- `./DevTool test StaticMeshTests --agent --timeout 600`: 75/75 passed.
- `./DevTool test TextureTests --agent --timeout 600`: 78/78 passed.
- `./DevTool build`: complete registered-profile build passed.
- `./DevTool doc validate --scope changed`: 4 documents validated.
- `./DevTool doc validate --scope all`: 141 documents validated.
- `./DevTool doc plan validate --scope all`: 5 active, 3 completed, and 295
  archived plans validated.
- `./DevTool doc roadmap validate --scope all`: 3 active and 23 archived
  roadmaps validated.

## Related Code

- [`Archive.h`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h)
- [`PropertyEditing.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/PropertyEditing.h)
- [`PropertyEditing.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/PropertyEditing.cpp)
- [`PropertyView.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/PropertyView.h)
- [`Transactor.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/Transactor.h)
- [`Transactor.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp)
- [`ReflectedPropertyTransactionTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Editor/ReflectedPropertyTransactionTests.cpp)
- [`ReflectedPropertyContainerTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Editor/ReflectedPropertyContainerTests.cpp)
