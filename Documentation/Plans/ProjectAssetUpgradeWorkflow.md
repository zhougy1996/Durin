# Project Asset Upgrade Workflow Plan

Summary: Add a project-wide asset upgrade audit, consolidated editor upgrade center, and safe batch execution without making startup eagerly retain every asset.

Last reviewed: 2026-07-28

Status: Active
Completed:

## Current Status

AssetCore already reports removed, unknown, or type-incompatible serialized
fields when a package is loaded. Registered structure upgraders can classify a
complete conversion as safe or migrated, while risky payloads prevent ordinary
saves unless the caller supplies explicit data-loss consent.

The only user-facing consumer is currently the Level workspace. Opening a Level
passes an `FAssetLoadReport` to `FLevelDocumentController`, defers activation,
and presents one blocking decision for compatibility issues in that Level's
main package. Dependency packages load without contributing their reports, and
assets that are never opened are not examined. Other migration mechanisms,
including asset-specific version changes performed during `PostLoad()` and
supported older package-envelope versions, do not share one discoverable
upgrade-report contract.

The completed Asset Structure Upgrade plan intentionally deferred Content
Browser batch upgrades and command-line auditing. This plan takes ownership of
those follow-ups while preserving the Level-open safety boundary.

## Goal

Give users one accurate project-wide inventory of assets that require or can
benefit from an upgrade, explain the reason and risk for each package, safely
batch-upgrade eligible packages, and expose the same audit to automation without
blocking editor startup or silently discarding incompatible data.

## Scope

- All packages discovered through the active asset registry across project,
  engine, and plugin mounts.
- A unified report for structure compatibility, registered asset-schema
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
- Inventing migration rules for unknown newer schemas.
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
- Modules that own reflected asset types register versioned migration
  contributors with stable handler IDs, human-readable reasons, and explicit
  risk classifications. AssetCore does not depend on Engine asset classes.
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
- The registry snapshot supplies the deterministic package queue. Auditing
  occurs incrementally on the object/game thread with a measured per-frame
  budget; no object or package state crosses to a worker or render thread.
- The audit loads at most one root package transaction at a time, captures its
  report, and releases packages that were introduced solely by that transaction.
  It never unloads a package that was already loaded, is active in a workspace,
  or remains required by another loaded package.
- Dependency packages receive their own queue entries and reports. A root report
  does not silently absorb dependency issues or authorize dependency saves.
- Audit ordering is stable by mount priority and virtual path. Cancellation
  preserves completed immutable results and leaves unvisited packages
  explicitly `NotAudited`.
- File I/O, corrupt-package, missing-dependency, and load failures are isolated
  per package. They remain visible in the final summary and do not abort the
  remaining audit.

### Audit and execution are separate phases

- Audit never writes authored packages. Any in-memory migration performed to
  understand a legacy package is discarded when an audit-only package is
  released.
- Selecting an upgrade starts a fresh load, reproduces the proposed migration,
  and compares the current registry revision and physical-file fingerprint with
  the audit snapshot before any save.
- A changed package becomes `Stale` and must be re-audited. Execution never
  applies a cached migration result to different bytes.
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
| Package versions | Registry exposes the package format and active plans preserve supported legacy reads | No user-facing rewrite-available classification |
| Editor notifications | Shared notification service supports progress and actions | No process-wide asset audit producer |
| Level workflow | Deferred activation and explicit safe/risky decisions are implemented and tested | Policy and presentation are Level-specific and do not publish global state |
| Content Browser | Registry-backed snapshot/model split and item presentation are established | No upgrade status, filters, or navigation |
| Automation | DurinDevTool owns repository command routing and machine-readable command patterns | No asset-upgrade audit command |

## Implementation Stages

### Stage 0: Freeze The Audit And Migration Contract

Dependencies: coordinate with the active DAsset Format Compaction plan so its
v2/v3 payload context and rewrite semantics remain inputs rather than duplicate
wire-format work.

- [ ] Inventory every repository-owned path that changes authored package state
  during load, including structure upgraders, explicit schema versions,
  `PostLoad()` migrations, and supported package-envelope rewrites.
- [ ] Classify each path as reportable safe migration, risky migration,
  rewrite-only opportunity, unsupported input, or non-upgrade runtime rebuild.
- [ ] Define the immutable package/session report types, stable handler IDs,
  severity ordering, and machine-readable schema.
- [ ] Specify the package fingerprint and registry-revision checks used between
  audit and execution.
- [ ] Prove the load/release algorithm for a root package, preloaded packages,
  newly introduced dependencies, circular dependencies, active workspace
  assets, and cancellation.
- [ ] Measure representative package load costs and select an explicit
  per-frame audit budget and pause policy for workspace transitions, modal
  decisions, PIE, and shutdown.
- [ ] Record the module dependency direction for AssetCore, Engine, DurinEd,
  MainFrame, and LevelEditor before adding editor types.

#### Acceptance Gate

- Every existing authored-state migration has one recorded disposition and no
  Dirty-state heuristic is required.
- Report ownership, payload lifetime, thread affinity, load/unload ordering,
  staleness, and failure behavior contain no unresolved decisions.
- The selected incremental budget keeps the editor responsive on the measured
  representative project and is reproducible in a focused test harness.
- The contract consumes DAST version metadata without changing or competing
  with the DAsset Format Compaction plan.

### Stage 1: Add AssetCore Audit And Execution Primitives

Dependencies: Stage 0.

- [ ] Add unified package and session report types, classifications,
  fingerprints, progress counters, and deterministic ordering.
- [ ] Extend package loading or add a scoped audit entrypoint so root and
  dependency packages can each produce reports without changing ordinary
  caller behavior.
- [ ] Add explicit asset-migration contributor registration and convert
  repository-owned load-time migrations from the Stage 0 inventory.
- [ ] Report supported older package formats as rewrite opportunities and
  future unsupported formats as blocked inputs.
- [ ] Implement audit-only release rules that preserve preloaded, active, and
  externally required packages.
- [ ] Implement fresh-load execution with stale-result rejection, ordinary safe
  save, explicit package-scoped data-loss consent, atomic publication, and
  structured per-package results.
- [ ] Add AssetCore and Engine native tests for safe, risky, unknown,
  rewrite-only, unsupported, stale, read-only, corrupt, missing-dependency,
  circular-dependency, cancellation, and save-failure cases.

#### Acceptance Gate

- Every registry package can produce one terminal audit state without an
  authored-file write.
- Audit-only packages and dependencies do not remain loaded after their safe
  release boundary, while pre-existing packages remain untouched.
- Safe execution reproduces the audited migration and publishes atomically;
  changed bytes are rejected as stale before save.
- Risky, unknown, unsupported, and read-only packages cannot enter the safe
  batch path.

### Stage 2: Add The Process-Wide Audit Coordinator

Dependencies: Stage 1.

- [ ] Add a DurinEd audit service that snapshots the registry queue, advances
  one game-thread audit transaction within the selected frame budget, and
  atomically publishes immutable progress/results.
- [ ] Start the service from MainFrame only after project mounts, registry
  reconciliation, and workspace registration complete.
- [ ] Add pause, resume, cancel, re-audit, registry-revision invalidation, and
  orderly shutdown behavior.
- [ ] Publish concise progress and completion notifications with an action that
  opens the Asset Upgrade Center.
- [ ] Accept reports discovered by normal workspace loads and reconcile saves,
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
- Startup remains within the Stage 0 responsiveness budget, audit-only loading
  remains bounded, and all selected safe packages reload without upgrade issues
  after batch publication.
- Focused suites, the full editor build, startup smoke test, and all-plan
  validation pass.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Inventory | Every repository-owned load-time authored-state mutation has a registered report or explicit non-upgrade disposition |
| Discovery | Project, engine, and plugin registry packages appear once in deterministic order |
| Structure compatibility | Safe cleanup, migrated fields, data-loss risk, unknown newer schema, grouped objects, and retained payloads |
| Asset schema migration | Explicit version, handler, reason, represented-state change, and clean reload |
| Package format | Supported rewrite opportunity, current format, and unsupported future format |
| Lifecycle | Preloaded, audit-only, dependency, circular, active-workspace, cancelled, and shutdown cases |
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
- Chained multi-version schema migrations and downgrade tooling.
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
- [DAsset Format Compaction Plan](DAssetFormatCompaction.md)
- [Level Editor Modularization Plan](LevelEditorModularization.md)
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
