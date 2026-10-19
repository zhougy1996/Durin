# Single Editor Engine Plan

Summary: Adopt one active editor engine, UE-shaped transaction entry points, and explicit window/World ownership while removing concurrent-editor arbitration.

Last reviewed: 2026-10-19

Status: Archived
Completed: 2026-10-19

## Current Status

All five stages are complete, including final integration qualification.
Production Launch activates one editor engine; MainFrame owns one shell.
Inactive construction cannot redirect the active service, and sequential
replacement follows completed retirement. Windows, documents, client sessions,
PIE and previews share this editor while retaining their own lifetimes.

Validation uses Win64-Debug-DurinEditor outside the sandbox. Tested source is
Stage 3 commit `1b9dc855f` plus the Stage 4 StoreUndo guard and native fixture
migrations committed with this completion record. The final production `all`
build passed; its preserved receipt is
`Build/SingleEditorEngineValidation/20261019-113005-647822-41512-cmake.log`.
EditorOperationTests passed 132 cases including competing Begin/StoreUndo
rejection, current-recording lifetime and editor replacement. Receipt:
`Build/SingleEditorEngineValidation/20261019-113016-466654-37804-EditorOperationTests.log`.
WorldTests passed 174 cases and both Python suites passed 47 and 12 cases against
the final source. All final acceptance logs are preserved outside the rolling
log directory under `Build/SingleEditorEngineValidation/`; the receipt manifest
`receipts.md` records exact files and counts: 1,139 passing tests across 21
targets covering Core object/property capture, editor operations and UI,
World/subsystems, Python/MCP, material editing, resource reload, RoadWeaver and
Sandbox. Runtime Core, Sandbox, viewport and header
receipts remain applicable because the last change affects only rejected
standalone editor custom recording. Consumers of custom records were rechecked.
Changed-document validation and all-plan validation passed, as did diff checking
and removed-symbol searches across all three workspace projects. Application
and GPU qualification are outside the selected acceptance scope.

The Core ITransaction subset implements SaveObject/ContainsObject; UE SaveArray,
annotations and full binary capture remain outside this ownership migration.
The pinned source URL could not be fetched during this run, so implementation
claims remain limited to the repository's existing audited baseline and tests.
The retired-editor Python case expects ReferenceError because native objects
are purged on retirement rather than remaining foreign live hosts.
Stage 4 review found that standalone StoreUndo assumed Begin always succeeds.
It now returns the admission failure when another buffer owns GUndo; the
single-recording test covers this path and verifies no history is added.
Final integration also migrates the remaining console dual-editor fixture to
inactive construction and sequential replacement, and explicitly activates the
editable-document fixture before invoking global editor commands.

## Goal

Use one active editor engine per process. Windows, documents, PIE and preview
Worlds share editor services and history. Keep headless native verification,
independent low-level transaction buffers, multiple clients and script jobs.
Replace concurrent-editor selection with ownership and scoped execution rules.

## Selected Decisions

- UE 5.8 public Engine, EditorEngine, Transactor, ScopedTransaction and WorldContext
  APIs are the interface reference. Repository-pinned UE 5.3.2 source is the
  available implementation reference for transaction nesting and cancellation.
  Record any unsupported UE feature or unavailable source comparison explicitly;
  do not claim complete UE 5.8 source equivalence.
- Construction does not publish GEditor. Initialization activates exactly one
  editor; inactive objects cannot replace it. Headless tests use the same
  activation boundary and retire one editor before activating another.
- Launch owns production activation/retirement. Subsystem callbacks may request
  shutdown; resource destruction still waits for callback, World and render
  boundaries. Keep those lifetime constraints, not multi-editor recorder stacks.
- Restore UE-shaped convenience scopes and queue/context queries as canonical
  interfaces, migrate consumers, and keep the corrected undone-count meaning.
  Retain Durin's typed failure reporting and property payload implementation;
  full UE binary serialization, annotations and collaboration are separate work.
- Runtime-neutral transaction capture remains below Engine/Editor dependencies.
  Remove candidate arbitration and host recorder rebinding. Current recording
  has one owner and is unpublished before owner storage is released.
- Editor provider refresh addresses one active collection. World collections
  remain independent. Existing editor, PIE and preview World ownership is kept;
  add only context APIs that serve those concrete lifetimes.
- Keep client/session/job cancellation and detached endpoints. Remove tests of
  concurrent full editor engines, replacing them with single-editor identity,
  multi-session, multiple World and shutdown/reentrancy coverage.

## Implementation Stages

### Stage 0: Pin reference contracts and acceptance

- [x] Record reference APIs, implementation limits and supported behavior.
- [x] Validate and commit this plan before implementation.

Completion: the selected scope and validation gates are reviewable in Git.

### Stage 1: Establish one active editor lifecycle

Depends on Stage 0.

- [x] Move active-editor publication out of construction; enforce one active
  editor at initialization and release it after shutdown.
- [x] Give headless fixtures an explicit production activation path.
- [x] Remove concurrent-editor provider refresh and update affected fixtures.

Completion: inactive objects do not redirect global UI/service access; concurrent
activation is rejected and sequential editor lifetimes remain supported.

### Stage 2: Align engine and transaction entry points

Depends on Stage 1.

- [x] Add runtime Engine transaction virtual entry points with editor overrides.
- [x] Restore implicit editor transaction scopes and canonical UE queue/context
  queries; migrate all Engine, Sandbox and RoadWeaver consumers.
- [x] Replace recorder candidates and host rebinding with one active recording
  boundary, retaining capture failures, GC visibility and stale-scope safety.
- [x] Cover nested recording, cancellation, Undo/Redo, branch replacement,
  inactive scopes and owner retirement.

Completion: editor actions, tools and Python use the same editor history without
concurrent-host recorder arbitration or exported thread-local data.

### Stage 3: Consolidate services and World ownership

Depends on Stages 1 and 2.

- [x] Route production UI/tools through the active editor and remove redundant
  per-callback editor selection while retaining callback lifetime guards.
- [x] Expose explicit editor/PIE World contexts without creating engines for
  windows or previews; preserve preview ownership and render fences.
- [x] Migrate cross-editor Python/tool fixtures to same-editor sessions, jobs,
  World lifetimes and sequential editor retirement.
- [x] Update editor, tool, Python, transaction and World contract documentation.

Completion: context routing distinguishes windows/Worlds/jobs, not concurrent
editor engines; teardown and external endpoint safety remain tested.

### Stage 4: Qualify the final integration

Depends on all implementation stages.

- [x] Inspect affected-test selection and run relevant transaction, editor,
  subsystem/World, tool, Python/MCP, material and RoadWeaver coverage.
- [x] Build all default-profile production targets: shared Engine/CoreDObject
  headers, virtual entry points and editor consumers cross module boundaries
  broadly enough that a final integration build is required.
- [x] Validate changed documents and all plans; review removed-symbol searches
  and final diff; record exact receipts and any remaining UE differences.
- [x] Commit each validated stage with Plan and Stage trailers, then mark the
  plan completed only after every required gate passes.

Validation follows [build](../../../Agents/BuildAndRun.md) and
[test](../../../Agents/Testing.md) selection and result-reuse rules. Use the existing
Win64-Debug-DurinEditor profile; application-hosted and GPU qualification are
not acceptance gates for this ownership refactor.

## Reference Contracts

- [UE Engine transaction entry points](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UEngine)
- [UE scoped transactions](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FScopedTransaction)
- [UE transaction buffer](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UTransBuffer)
- [UE World context](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FWorldContext)
- [Current transaction contract](../../../Editor/Architecture/Transactors.md)
- [Editor subsystem ownership](../../../Editor/Architecture/EditorSubsystems.md)
