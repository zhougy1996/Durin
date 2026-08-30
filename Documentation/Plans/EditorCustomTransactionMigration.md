# Editor Custom Transaction Migration Plan

Summary: Add executable custom changes to the editor transactor and migrate every non-property command transaction without creating a second history.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

P3 is complete. `DTransBuffer` now owns every executable property and custom
change payload. `FTransactionManager` retains only data-free expected-ID
bridges plus its temporary package revision and event state; P4 can remove that
facade without redesigning any domain transaction.

## Goal

Provide one move-only custom-change record inside `FTransaction`, then move all
asset, Content Browser, material graph, Level, placement, attachment, gizmo,
and deferred command payloads into `DTransBuffer`. Preserve the established
failure, compensation, recovery-required, package revision, mounted-content
revision, event, and module-retirement contracts.

## Scope

This plan changes the shared DurinEd transaction contract and every current
non-property `ITransaction` implementation and submission seam in AssetTools,
ContentBrowser, MaterialEditor, LevelEditor, and MainFrame. The legacy manager
may remain only as a data-free application ordering/revision bridge until P4.

## Non-Goals

- Do not cut application Undo/Redo, save checkpoints, or Activity History over
  to `DTransBuffer`; P4 owns that global cutover.
- Do not remove the legacy manager or its public compatibility surface in P3.
- Do not persist editor-session history or add multi-user exchange.
- Do not change the Engine asset mutation journal or weaken
  `RecoveryRequired` handling.

## Selected Decisions

- `FTransactionCustomChange` owns one `ITransactionCustomChange` payload and is
  a peer of object records in `FTransaction`.
- Custom changes expose description/details, affected packages, mounted-content
  mutation, synchronous/deferred Undo/Redo, collector traversal, and bounded
  native allocation accounting through one shared contract.
- `DTransBuffer` owns every migrated executable payload. Any P3 legacy bridge
  contains only the expected transaction ID plus revision/event metadata.
- A failed transition keeps the history cursor fixed. A deferred transition
  keeps the transactor non-reentrant until completion; success alone advances
  history and publishes success state.
- Domain implementations remain data-driven where practical. Module-owned
  implementations register retirement drains so executable code cannot survive
  its module lease.

## Implementation Stages

### Stage 0: Shared Custom-Change Contract

- [x] Define the move-only custom-change record, validation/application,
  collector traversal, and overflow-safe size accounting.
- [x] Extend recording and execution for already-applied and not-yet-applied
  changes without introducing another history stack.
- [x] Implement synchronous failure rollback and deferred completion barriers,
  including destruction-safe callback disconnection.
- [x] Preserve package, mounted-content, event, and expected-ID metadata needed
  by the temporary P3 bridge.
- [x] Add focused transactor tests for sync, deferred, failure, compensation,
  retention, eviction, reset, and shutdown behavior.

### Stage 1: Asset And Content Browser Migration

- [x] Migrate asset relocation and mutation adapters into custom changes.
- [x] Migrate Content Browser deletion, staging, and deferred filesystem
  recovery behavior without weakening `RecoveryRequired`.
- [x] Preserve exactly one mounted-content revision per successful transition.
- [x] Cover compensation failure, recovery-required, stale plans, branch
  replacement, and callback shutdown.

### Stage 2: Material And Level Migration

- [x] Migrate material graph commands and continuous graph edit commits.
- [x] Migrate spline, level mutation, skybox, terrain, primary-camera,
  visibility, attachment, and transform-gizmo transactions.
- [x] Preserve object participant reachability explicitly through custom-change
  collector traversal rather than transaction roots.
- [x] Preserve dirty-state and package-revision behavior for all commands.

### Stage 3: Coexistence, Retirement, And Qualification

- [x] Prove no migrated payload remains in legacy history and all P0 inventory
  entries are accounted for.
- [x] Drain module-owned custom records before module retirement and verify no
  deferred callback can enter retired code.
- [x] Update lasting transaction documentation and mark P3 complete in the
  roadmap.
- [x] Run focused editor, asset, material, Level, and transactor tests followed
  by the complete registered-profile build.
- [x] Run changed/all document, all-plan, and all-roadmap validation and record
  exact evidence here.

## Acceptance Gates

- [x] `FTransaction` contains object records and custom changes in deterministic
  forward/ reverse order.
- [x] All existing non-property `ITransaction` consumers store executable data
  only in `DTransBuffer`.
- [x] Failed or deferred transitions preserve cursor, package revision,
  mounted-content revision, and event correctness.
- [x] Asset recovery-required and compensation behavior is unchanged.
- [x] Module retirement and shutdown release callbacks, executable payloads,
  and collector edges safely.
- [x] P4 can remove the legacy manager without redesigning any command payload.

## Validation

Use the repository agent build and native-test workflows. Required lanes are
`EditorOperationTests`, `EditorShellTests`, `MaterialTests`, `SplineTests`,
`WorldTests`, `StaticMeshTests`, and `EditorPropertyTests`, followed by the
complete registered-profile build and all documentation validators.

Completion evidence on `macos-xcode-arm64` /
`MacOS-arm64-Debug-DurinEditor`:

- `./DevTool test EditorOperationTests --agent`: 39/39 passed, including custom
  sync failure, deferred barriers/completion, collector retention, and module
  drain coverage.
- `./DevTool test EditorShellTests --agent`: 47/47 passed.
- `./DevTool test MaterialTests --agent --timeout 600`: 108/108 passed.
- `./DevTool test SplineTests --agent --timeout 600`: 40/40 passed.
- `./DevTool test WorldTests --agent --timeout 600`: 106/106 passed.
- `./DevTool test StaticMeshTests --agent --timeout 600`: 75/75 passed.
- `./DevTool test EditorPropertyTests --agent --timeout 600`: 32/32 passed.
- `./DevTool build`: complete registered-profile build passed.
- `./DevTool doc validate --scope changed`: 4 documents validated.
- `./DevTool doc validate --scope all`: 142 documents validated.
- `./DevTool doc plan validate --scope all`: 5 active, 4 completed, and 295
  archived plans validated.
- `./DevTool doc roadmap validate --scope all`: 3 active and 23 archived
  roadmaps validated.

## Related Code

- [`Transaction.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/Transaction.h)
- [`Transactor.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/Transactor.h)
- [`Transactor.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp)
- [`AssetOperations.cpp`](../../Engine/Source/Editor/AssetTools/Private/AssetTools/AssetOperations.cpp)
- [`ContentBrowserOperations.h`](../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserOperations.h)
- [`MaterialGraphOperations.cpp`](../../Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphOperations.cpp)
- [`TransformGizmo.cpp`](../../Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.cpp)
