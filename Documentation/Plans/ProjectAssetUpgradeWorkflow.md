# Project Asset Upgrade Workflow Plan

Summary: Add a project-wide asset upgrade audit, consolidated editor upgrade center, and safe batch execution without making startup eagerly retain every asset.

Last reviewed: 2026-07-30

Status: Active
Completed:

## Current Status

Stage 0 completed on 2026-07-28 against baseline `03acb67f`. The repository
inventory found two authored upgrade domains: AssetCore field-structure
compatibility, including the registered `DStaticMeshComponent` material-field
upgrader, and `DStaticMesh::MaterialSlotsVersion`. Package-envelope rewriting
becomes a third domain only if AssetCore intentionally supports an older DAST
wire format. The Tagged Asset Field Upgrades plan keeps the current DAST format
at v2 and establishes tagged field identity plus retained legacy payloads as
the authored-structure compatibility contract; this workflow consumes those
audit and execution results without duplicating package or migration policy.

Texture2D source identity reconciliation and StaticMesh source-hash refresh can
also mark packages Dirty during `PostLoad()`, but they represent external source
freshness rather than an authored-structure upgrade. They are now explicitly
classified as non-upgrade load mutations that must block an upgrade batch from
silently saving unrelated state. Material, MaterialInstance, TextureCube,
SplineComponent, and Level `PostLoad()` paths do not currently perform authored
structure migrations. DDC, shader, cooked-payload, importer, decoder, builder, and
projection versions govern rebuildable or runtime data and remain outside the
authored-package upgrade inventory.

Measurement invalidated the original full-load startup design. Isolated cold
loads of the seven current Engine/Sandbox authored packages ranged from 2.30ms
to 2002.50ms; the Level root transaction triggered a StaticMesh DDC rebuild and
the Material load took 252.78ms. Warm isolated loads ranged from 2.15ms to
9.97ms. By contrast, twenty complete package inspections per asset produced a
worst individual observation of 0.464ms. The selected startup design is
therefore a two-tier workflow: a game-thread, object-free inspection pass with
a 2ms cumulative and four-package per-frame cap, followed by fresh full loading
only when the user explicitly executes or reviews an upgrade.

The Stage 0 measurement used a temporary focused EngineTests harness, removed
after recording results. It registered the repository Engine and Sandbox
Content roots, called `InspectAssetPackage` twenty times per package, then
isolated each `LoadAsset` with `ShutdownAssetManager()`. The same seven-package
corpus and procedure are the required baseline for Stage 5 performance
comparison.

Stage 1 is in progress. AssetCore now exposes content-hashed all-object
inspection, deterministic package/session reports, object-free structure and
owner-specific semantic contributors, a separate load-mutation ledger, and a safe
fresh-load execution primitive. Execution rejects stale or already loaded
targets, blocks non-upgrade `PostLoad()` mutations, verifies that materialized
migrations reproduce the audit, publishes through the ordinary atomic save
path with the expected fingerprint, and releases only packages introduced by
the transaction. Risky packages require explicit package-scoped data-loss
consent, while read-only upgrade candidates are blocked during audit.

Engine contributors now cover legacy `DStaticMeshComponent` material fields
and `DStaticMesh::MaterialSlotsVersion`. The latter also fixes a DDC-hit path
that previously skipped the slot-identity migration. Texture2D source identity
and StaticMesh source-hash reconciliation are recorded as non-upgrade
mutations. `DClass::IsChildOf` is exported because AssetCore uses the public
reflection relationship query across the CoreDObject DLL boundary.

Stage 1 checkpoint validation passed all 54 AssetCoreTests plus focused Engine
coverage for legacy component fields and semantic StaticMesh slot-version
audit/execution. The full 367-test EngineTests run remains limited by
`FTexture2DTests.ReflectedBuildSettingsRebuildTransactionallyAndSupportUndoRedo`:
the test completes its edits, unload, and deletion, then exits with a breakpoint
exception while its transaction manager is destroyed after the edited Texture
object. Temporary diagnostics were removed; resolving that independent
transaction/object lifetime defect is outside this plan's working set.

Stage 2 is in progress. DurinEd now owns a process-wide
`FAssetUpgradeAuditService` with an atomically published immutable snapshot.
MainFrame starts it only after the current project's workspaces have registered,
and `DEditorEngine` advances it on the game thread outside PIE and shuts it down
before editor teardown. Each slice completes at most four packages and stops at
the 2ms cumulative budget; an individual over-budget package completes
atomically and records its duration. The service supports pause, resume, cancel,
explicit re-audit, deterministic queue reconstruction when the registry revision
changes, and preserves completed results plus explicit `NotAudited` entries.

The first Stage 2 checkpoint passed five focused coordinator tests covering
deterministic incremental publication, count and time budgets, pause/resume,
cancellation, registry-revision invalidation, and terminal shutdown. The
MainFrame module also builds successfully. Per instruction, unrelated failures
from the currently unstable parallel EngineTests suite are not part of this
checkpoint and the full suite was not run.

The second Stage 2 checkpoint adds one notification path per audit generation,
including progress, cancellation, completion classification counts, and an
`Open Asset Upgrade Center` request that MainFrame retains for the Stage 3 host
window. Level, Material, and Texture workspaces merge load-discovered reports
into the immutable snapshot and immediately mark successful saves or moves
stale; the existing registry revision boundary rebuilds the deterministic queue
after saves, moves, deletes, imports, or registry refreshes. Reports discovered
while startup is still idle are retained and reconciled when MainFrame starts
the audit. Three additional focused tests cover workspace report merging,
individual invalidation, and the single actionable notification lifecycle.

### Stage 0 Handoff

- Baseline: `03acb67f`.
- Working set: this plan; `AssetSystem.h/.cpp`; `StaticMesh.cpp`;
  `Texture2D.cpp`; `TextureCube.cpp`; temporary EngineTests measurement harness
  removed before handoff.
- Key symbols: `FAssetLoadReport`, `FAssetPackageInspection`,
  `InspectAssetPackage`, `RegisterAssetStructureUpgrader`,
  `FAssetManager::LoadPackageInternal`, `DStaticMesh::PostLoad`,
  `DTexture2D::PostLoad`.
- Decisions: object-free startup inspection, explicit upgrade contributors,
  separate non-upgrade load-mutation ledger, content-hashed stale protection,
  no automatic full loads, and `AssetCore -> Engine contributors -> DurinEd
  coordinator -> MainFrame/LevelEditor presentation` dependency direction.
- Open questions: none for Stage 0. Exact public type names may change during
  Stage 1 without changing the frozen ownership and behavior.
- Validation: all seven representative packages inspected and loaded
  successfully; focused seven-test Level upgrade model suite also passed.

## Goal

Give users one accurate project-wide inventory of assets that require or can
benefit from an upgrade, explain the reason and risk for each package, safely
batch-upgrade eligible packages, and expose the same audit to automation without
blocking editor startup or silently discarding incompatible data.

## Scope

- All packages discovered through the active asset registry across project,
  engine, and plugin mounts.
- A unified report for tagged-field compatibility, registered owner-specific
  migrations, supported older package-envelope rewrites, and packages that
  cannot be upgraded by the current process.
- A non-blocking post-start audit coordinated from the editor host.
- A project-wide Asset Upgrade Center with filtering, package and object
  details, progress, stale-result handling, and explicit actions.
- Content Browser upgrade status and navigation backed by the same audit
  snapshot.
- Safe batch save, individually confirmed risky cleanup, retry, cancellation,
  failure isolation, and atomic publication.
- Integration with the existing blocking Level-open workflow.
- A command-line audit mode suitable for continuous integration.
- Native tests, checked-in compatibility fixtures, and end-to-end editor
  validation.

## Non-Goals

- Loading and retaining every project asset before the editor becomes usable.
- Automatically rewriting assets merely because the engine version changed.
- Treating rebuildable DDC, shader caches, cooked containers, or source-library
  files as authored asset packages.
- Inventing migration rules for unknown incompatible fields or newer packages.
- Silently saving risky packages, suppressing future warnings permanently, or
  allowing one global confirmation to authorize unrelated data loss.
- Changing the DAST wire format, reflection identity, dependency semantics, or
  atomic file-publication contract.
- Replacing the blocking decision required before an incompatible Level becomes
  the active world.
- Adding asynchronous object construction or running asset loading on a worker
  thread.

## Design Decisions and Invariants

### Ownership

- AssetCore owns package audit requests, immutable unified reports, package
  fingerprints, stale-result detection, upgrade execution options, and the
  distinction between writable, read-only, unsupported, safe, and risky
  outcomes.
- Modules that own reflected asset types register exact legacy-field or
  owner-local semantic migration contributors with stable handler IDs,
  human-readable reasons, and explicit risk classifications. AssetCore does not
  depend on Engine asset classes.
- DurinEd owns the process-wide audit session and queryable snapshot. MainFrame
  starts and hosts the global workflow after project mounts, registry
  reconciliation, and workspace registration succeed.
- LevelEditor consumes the shared snapshot for Content Browser decoration and
  retains its specialized Level activation boundary. It does not own the
  project-wide audit service.
- The command-line entrypoint uses the same AssetCore audit model and
  classification policy as the editor; it does not parse logs or editor text.

### Unified upgrade report

- One package report records virtual path, main asset class, mount owner and
  writability, registry revision, physical-file fingerprint, package format,
  audit state, compatibility issues, migration contributions, proposed action,
  risk, and diagnostics.
- Structure-compatibility issues remain object-level and retain their original
  payload and handler information. Package reports reference those issues
  without weakening the existing payload-lifetime contract.
- Asset-specific migrations must report why the represented state changed.
  A package becoming Dirty during audit is not by itself sufficient evidence of
  an upgrade because it may have been modified before the audit.
- The physical-file fingerprint contains byte size, stable last-write ticks,
  and a content hash computed from the inspected bytes. Registry revision
  invalidates queue topology; the content hash is authoritative for deciding
  whether an individual package still matches its audit.
- Package audit states are `NotAudited`, `UpToDate`, `SafeUpgrade`,
  `RiskyUpgrade`, `RewriteAvailable`, `BlockedUnsupported`,
  `BlockedLoadMutation`, `AuditFailed`, and `Stale`. Machine-readable schema
  version 1 serializes these stable names rather than ordinal values.
- Each upgrade contribution records a stable handler ID, classification, risk,
  summary, affected object/field identities, and whether execution requires
  object materialization. Non-upgrade load mutations use a separate ledger with
  their own handler and reason; they never make a package batch-safe.
- Supported older package formats may be reported as `RewriteAvailable` even
  when loading them does not mark the package Dirty. Unsupported future formats
  are `BlockedUnsupported`, never actionable upgrades.
- A package is batch-safe only when every contribution is completely recognized,
  has no compatibility risk, is writable, and can be reproduced from the
  audited file. Mixed packages inherit their highest risk.

### Audit lifecycle and ordering

- Startup becomes interactive before a full audit completes. The editor
  publishes a persistent progress notification and an action that opens the
  Asset Upgrade Center.
- The registry snapshot supplies the deterministic package queue. Startup
  auditing uses `InspectAssetPackage`-style byte snapshots expanded to all
  package objects, not only the main asset. It constructs no objects, resolves
  no runtime resources, invokes no `PostLoad()`, and writes nothing.
- Inspection occurs incrementally on the object/game thread because reflection
  and contributor registries are process-global and not currently published as
  thread-safe immutable state. Each frame stops after four packages or 2ms of
  cumulative inspection work, whichever comes first. A single over-budget
  package completes atomically, records its duration, and ends that frame's
  audit slice.
- Inspection contributors classify serialized fields and version carriers
  without mutating represented state. A contribution that cannot prove its risk
  or result from package bytes remains visible but requires package-scoped load
  review and cannot enter the safe batch.
- Dependency packages receive their own queue entries and reports. Inspection
  may read dependency metadata required to explain a root contribution, but a
  root report does not absorb dependency actions or authorize dependency saves.
- Audit ordering is stable by mount priority and virtual path. Cancellation
  preserves completed immutable results and leaves unvisited packages
  explicitly `NotAudited`.
- File I/O, corrupt-package, missing-dependency, and load failures are isolated
  per package. They remain visible in the final summary and do not abort the
  remaining audit.

### Audit and execution are separate phases

- Audit never loads package objects or writes authored packages. Review or
  execution may materialize one root transaction only after an explicit user
  action.
- Selecting an upgrade starts a fresh load, reproduces the proposed migration,
  compares the current registry revision and content-hashed physical-file
  fingerprint with the audit snapshot, and captures every load-time mutation
  before any save.
- A changed package becomes `Stale` and must be re-audited. Execution never
  applies a cached migration result to different bytes.
- A non-upgrade load mutation that was not represented in the inspected upgrade
  contributions changes the package to `BlockedLoadMutation`; the batch does not
  save source reconciliation or another unrelated authored change.
- The expected fingerprint is checked again inside the atomic save boundary so
  an external writer cannot replace the source between the pre-load check and
  publication.
- Saves use the existing atomic publication boundary. One package failing does
  not roll back already published independent packages; the result view records
  every success, failure, skip, and stale item.
- Safe batch execution accepts only batch-safe packages. Each package containing
  `DataLossRisk` or `UnknownIncompatible` requires a separate package-scoped
  confirmation that identifies the exact discarded fields.
- Read-only engine or plugin packages remain visible and explain which owning
  installation must be updated; the editor does not attempt to bypass mount or
  filesystem policy.

### Editor interaction

- The Asset Upgrade Center is a non-modal host window with summary counts,
  audit progress, mount/class/risk filters, a package list, expandable
  object/change details, and actions for re-audit, safe batch upgrade, or
  package-scoped review.
- Startup uses one consolidated notification rather than one modal per package.
  It distinguishes safe upgrades, risky or unknown data, blocked packages, and
  audit failures.
- Content Browser items display status from the latest immutable audit snapshot
  and can open the corresponding Upgrade Center detail. It does not trigger
  hidden loads while drawing.
- Opening a Level with current compatibility issues remains deferred and
  blocking. Its result is published into the shared snapshot, and a successful
  save invalidates or resolves that package entry so the global center does not
  present a duplicate stale action.
- Opening a non-Level asset may publish newly discovered issues to the shared
  snapshot, but the workspace may only offer save or destructive cleanup
  choices through the same central policy.

### Command-line behavior

- The audit command exits successfully only when the selected policy is
  satisfied. Machine-readable output has a versioned schema and stable
  classifications; human-readable output groups packages by severity.
- The initial command is read-only. A future write mode is outside this plan
  unless it can reuse the editor's fingerprint checks, risk policy, and atomic
  execution path without interactive ambiguity.
- CI policy can fail on any required safe rewrite, only risky/blocked assets, or
  audit errors. The selected policy is explicit rather than encoded in warning
  text.

## Current Foundations and Gaps

| Area | Current foundation | Gap |
| --- | --- | --- |
| Registry | Deterministic mounted package discovery, class metadata, dependencies, revision, and cached fingerprints | No audit queue or upgrade status |
| Package inspection | Bounded header and complete field inspection without object construction | Inspection alone cannot execute class-specific object migrations or classify all legacy payloads |
| Structure compatibility | `FAssetLoadReport`, retained legacy payloads, registered upgraders, risk classifications, and save refusal | Reports are requested only by selected callers and exclude dependency loads |
| Asset migrations | Individual assets can migrate versioned state during loading and mark packages Dirty | No shared reason, handler, risk, or audit contribution |
| Package versions | Registry exposes the current package format; reflected field evolution is independent of it | No user-facing rewrite-available classification if an older envelope is supported later |
| Editor notifications | Shared notification service supports progress and actions | No process-wide asset audit producer |
| Level workflow | Deferred activation and explicit safe/risky decisions are implemented and tested | Policy and presentation are Level-specific and do not publish global state |
| Content Browser | Registry-backed snapshot/model split and item presentation are established | No upgrade status, filters, or navigation |
| Automation | DurinDevTool owns repository command routing and machine-readable command patterns | No asset-upgrade audit command |

## Implementation Stages

### Stage 0: Freeze The Audit And Migration Contract

Dependencies: consume the Tagged Asset Field Upgrades contract so exact legacy
field identities, owner-local semantic version carriers, and any future
package-envelope rewrite remain inputs rather than duplicate migration work.

- [x] Inventory every repository-owned path that changes authored package state
  during load, including structure upgraders, owner-local version carriers,
  `PostLoad()` migrations, and supported package-envelope rewrites.
- [x] Classify each path as reportable safe migration, risky migration,
  rewrite-only opportunity, unsupported input, or non-upgrade runtime rebuild.
- [x] Define the immutable package/session report types, stable handler IDs,
  severity ordering, and machine-readable schema.
- [x] Specify the package fingerprint and registry-revision checks used between
  audit and execution.
- [x] Replace the startup load/release algorithm with object-free all-object
  inspection; restrict fresh root-package materialization to explicit review or
  execution and preserve preloaded/active packages.
- [x] Measure representative package load costs and select an explicit
  per-frame audit budget and pause policy for workspace transitions, modal
  decisions, PIE, and shutdown.
- [x] Record the module dependency direction for AssetCore, Engine, DurinEd,
  MainFrame, and LevelEditor before adding editor types.

#### Acceptance Gate

- Every existing authored-state migration has one recorded disposition and no
  Dirty-state heuristic is required.
- Report ownership, payload lifetime, thread affinity, load/unload ordering,
  staleness, and failure behavior contain no unresolved decisions.
- The selected incremental budget keeps the editor responsive on the measured
  representative project and is reproducible in a focused test harness.
- The contract consumes DAST format metadata, tagged legacy-field reports, and
  owner-local semantic migration results without introducing another
  compatibility mechanism.

### Stage 1: Add AssetCore Audit And Execution Primitives

Dependencies: Stage 0.

- [x] Add unified package and session report types, classifications,
  fingerprints, progress counters, and deterministic ordering.
- [x] Expand complete package inspection from main-asset fields to immutable
  per-object snapshots with retained payload context and content fingerprints.
- [x] Add inspection-contributor registration for object-free classification
  and explicit execution contributors for repository-owned migrations from the
  Stage 0 inventory.
- [x] Add a load-mutation ledger and instrument Texture2D source identity,
  StaticMesh source hash, structure upgrades, and owner-local semantic
  migrations so execution cannot infer meaning from Dirty state.
- [ ] Report supported older package formats as rewrite opportunities and
  future unsupported formats as blocked inputs.
- [x] Implement object-free package audit with the selected deterministic queue
  inputs and classification aggregation.
- [x] Implement fresh-load execution with preloaded/active-package preservation,
  stale-result rejection, load-mutation blocking, ordinary safe save, explicit
  package-scoped data-loss consent, expected-fingerprint atomic publication,
  and structured per-package results.
- [ ] Add AssetCore and Engine native tests for safe, risky, unknown,
  rewrite-only, unsupported, stale, read-only, corrupt, missing-dependency,
  circular-dependency, cancellation, and save-failure cases.

#### Acceptance Gate

- Every registry package can produce one terminal audit state without object
  construction or an authored-file write.
- Audit inspection invokes neither dependency loading nor `PostLoad()`.
- Safe execution reproduces the audited migration and publishes atomically;
  changed bytes are rejected as stale before save.
- Risky, unknown, unsupported, and read-only packages cannot enter the safe
  batch path.

### Stage 2: Add The Process-Wide Audit Coordinator

Dependencies: Stage 1.

- [x] Add a DurinEd audit service that snapshots the registry queue, advances
  one game-thread audit transaction within the selected frame budget, and
  atomically publishes immutable progress/results.
- [x] Start the service from MainFrame only after project mounts, registry
  reconciliation, and workspace registration complete.
- [x] Add pause, resume, cancel, re-audit, registry-revision invalidation, and
  orderly shutdown behavior.
- [x] Publish concise progress and completion notifications with an action that
  opens the Asset Upgrade Center.
- [x] Accept reports discovered by normal workspace loads and reconcile saves,
  moves, deletes, imports, and registry refreshes without duplicate or stale
  actions.
- [ ] Add coordinator tests for deterministic queues, frame budgets,
  cancellation, resumption, mutation invalidation, workspace-discovered
  reports, and shutdown during an in-flight audit step.

#### Acceptance Gate

- The editor becomes usable before the project-wide audit completes.
- Auditing never runs asset object construction off the game thread and does
  not advance during a forbidden lifecycle state.
- The published snapshot remains internally consistent while registry and
  workspace events invalidate individual packages.
- Startup produces one consolidated progress/completion path rather than a
  sequence of package modals.

### Stage 3: Build The Asset Upgrade Center And Content Browser Integration

Dependencies: Stage 2 and the completed Content Browser model/presentation split
from the Level Editor Modularization plan.

- [ ] Add the host-level Asset Upgrade Center window with progress, summary
  counts, filters, deterministic package rows, object/change details, and clear
  empty, cancelled, stale, and failure states.
- [ ] Add safe selection and batch execution with live per-package results,
  cancellation between packages, retry, and a final outcome summary.
- [ ] Add a separate package-scoped risky-review flow that displays exact
  incompatible fields and never reuses consent for another package.
- [ ] Add Content Browser status decoration, filtering, tooltip explanations,
  and navigation to the corresponding Upgrade Center detail.
- [ ] Present read-only engine/plugin ownership and blocked-version guidance
  without exposing an invalid save action.
- [ ] Add editor model and interaction tests for filtering, selection,
  batch-safety enforcement, stale transitions, risky confirmation, navigation,
  cancellation, retry, and notification actions.

#### Acceptance Gate

- Users can identify every audited package requiring attention and distinguish
  safe, risky, blocked, read-only, failed, stale, and unaudited states.
- Safe batch upgrade never includes a package requiring data-loss consent.
- Content Browser presentation performs no package loads and reflects the same
  snapshot and classifications as the Upgrade Center.
- One package failure leaves remaining selected packages actionable and all
  outcomes reviewable.

### Stage 4: Integrate Workspace Opens And Remove Level-Only Policy Duplication

Dependencies: Stage 3.

- [ ] Extract the existing Level decision policy from Level-specific
  presentation onto the shared upgrade action model without changing deferred
  activation semantics.
- [ ] Publish Level-open reports to the global audit snapshot and resolve or
  invalidate them after save, open-without-save, cancel, unload, or activation
  failure.
- [ ] Ensure dependency issues discovered while opening a Level receive their
  own package entries and never inherit consent intended for the Level package.
- [ ] Route compatibility reports discovered by Material, Texture, StaticMesh,
  and future asset workspaces into the shared snapshot.
- [ ] Preserve the blocking modal only for the asset whose activation or save is
  currently requested; unrelated packages remain non-modal.
- [ ] Extend workspace integration tests across safe save, risky refusal,
  open-without-save, cancellation, stale global results, and duplicate-report
  reconciliation.

#### Acceptance Gate

- Opening an affected Level still cannot replace the active world before its
  explicit decision.
- The Level modal and Upgrade Center apply identical risk and save policy while
  retaining their distinct activation behavior.
- A successful workspace save removes the obsolete global action, and
  open-without-save remains visible as unresolved after a later unload/re-audit.
- Dependency and non-Level asset issues are visible as independent package
  entries.

### Stage 5: Add Read-Only Automation And Complete Validation

Dependencies: Stages 1 through 4.

- [ ] Add a DurinDevTool asset-upgrade audit command using the shared runtime
  report model, with human-readable and versioned machine-readable output.
- [ ] Add explicit CI policies for any actionable rewrite, risky/blocked
  assets, and audit failures.
- [ ] Validate the command against the checked-in old-structure,
  unknown-newer-field, older-package-format, stale, corrupt, and
  missing-dependency fixtures.
- [ ] Run focused AssetCore, Engine, DurinEd, MainFrame, LevelEditor, and
  tooling tests through the repository entrypoints.
- [ ] Run a complete `all` build and a real editor startup smoke test because
  the workflow is user-visible and begins during startup.
- [ ] Measure startup-to-interactive time, audit frame cost, peak retained
  package count, complete audit duration, and safe batch results on a
  representative project.
- [ ] Move lasting audit, migration, editor ownership, and automation contracts
  into their owning documentation, validate all plans, and mark this plan
  complete.

#### Acceptance Gate

- Editor and command-line audits produce equivalent paths, classifications,
  risks, and diagnostics for the same package corpus.
- CI can detect required upgrades without modifying authored files.
- Startup remains within the Stage 0 responsiveness budget, object-free
  inspection remains bounded, and all selected safe packages reload without
  upgrade issues after batch publication.
- Focused suites, the full editor build, startup smoke test, and all-plan
  validation pass.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Inventory | Every repository-owned load-time authored-state mutation has a registered report or explicit non-upgrade disposition |
| Discovery | Project, engine, and plugin registry packages appear once in deterministic order |
| Structure compatibility | Safe cleanup, migrated fields, data-loss risk, unknown newer schema, grouped objects, and retained payloads |
| Owner-specific migration | Exact legacy identity or local version, handler, reason, represented-state change, and clean reload |
| Package format | Supported rewrite opportunity, current format, and unsupported future format |
| Lifecycle | Object-free audit, preloaded execution target, dependency, circular, active-workspace, cancelled, and shutdown cases |
| Staleness | Registry revision, changed fingerprint, moved, deleted, imported, and externally edited package |
| Persistence | Atomic safe save, explicit risky save, read-only refusal, partial batch failure, retry, and clean reload |
| Editor | Startup notification, Upgrade Center states/actions, Content Browser status/navigation, and responsive frame budget |
| Workspace integration | Deferred Level activation, non-Level discovery, dependency separation, no-save, cancellation, and duplicate reconciliation |
| Automation | Human and machine output parity plus each CI failure policy |
| Regression | Focused native/tooling suites, full `all` build, editor startup smoke test, and plan validation |

Build, test, and runtime execution follow
[Build And Run](../Development/Build/BuildAndRun.md) and
[Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- After startup, one non-blocking workflow inventories every registry-discovered
  package and clearly identifies safe upgrades, risky or unknown data,
  rewrite-only opportunities, blocked inputs, read-only ownership, failures,
  stale results, and unaudited packages.
- Users can batch-publish all eligible safe upgrades without loading and
  retaining the entire project or authorizing data loss.
- Every risky save requires exact, package-scoped review and explicit consent.
- Audit and execution are separated by reproducible migration and package
  fingerprint checks.
- Level activation retains its blocking safety contract while all asset types
  share one report, policy, global status, and navigation model.
- Content Browser and the Asset Upgrade Center consume immutable audit state and
  never load assets as a side effect of drawing.
- CI can audit the same corpus and policy without modifying content or parsing
  logs.
- Required validation passes and lasting behavior is documented outside this
  plan.

## Deferred Follow-ups

- Incremental worker-thread parsing of package bytes if future object-free audit
  metadata can preserve complete migration classification.
- A non-interactive command-line write mode after an explicit repository content
  migration policy is designed.
- Downgrade tooling.
- Remote or source-control-aware checkout before upgrading read-only authored
  packages.
- Persistent audit caches beyond registry fingerprints if measured projects
  justify the invalidation complexity.
- Automatic upgrade on import or source reimport.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Tagged Asset Field Upgrades Plan](Archive/2026-07/TaggedAssetFieldUpgrades.md)
- [Level Editor Modularization Plan](Archive/2026-08/LevelEditorModularization.md)
- [Asset Structure Upgrade Plan](Archive/2026-07/AssetStructureUpgrade.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorNotification.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/AssetStructureUpgradeModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/AssetStructureUpgradeModelTests.cpp`
