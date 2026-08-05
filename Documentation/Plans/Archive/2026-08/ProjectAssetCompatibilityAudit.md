# Project Asset Compatibility Audit Plan

Summary: Remove the obsolete asset-upgrade workflow and add an explicit read-only project and CI audit for packages that are incompatible with the current authored baseline.

Last reviewed: 2026-08-05

Status: Archived
Completed: 2026-08-05

## Current Status

This plan starts from baseline `f18d581e`. The current authored-data contract is
the repository's checked-in asset baseline. AssetCore retains unknown serialized
fields, reports them through `FAssetLoadReport`, and refuses an ordinary save
that would discard compatibility-risk payloads. The production engine registers
no asset-specific structure upgrader and promises no supported resave path for
retired asset schemas.

An earlier project-upgrade implementation no longer matches that contract. It
left behind a process-wide startup audit service, full-payload package audit and
execution reports, workspace-to-audit report merging, an unimplemented Upgrade
Center request, and a Level-specific upgrade dialog. The dialog presents save,
discard, and open choices even though the current baseline has no production
migration capable of making an incompatible package current. Stage 0 removes
that workflow instead of preserving or extending it.

The selected replacement is a compatibility audit, not an upgrade system. It
runs only after an explicit editor or command-line request, reads package
structure without constructing `DObject` instances or retaining field payloads,
and publishes compact path-keyed diagnostics. Opening an incompatible asset is
rejected with a clear diagnostic and leaves the previous document active; it
does not open an upgrade modal or offer data-discard persistence. AssetCore's
save refusal remains the final safety boundary.

Stage 0 is complete from implementation baseline `34712e46` (the plan's
recorded architecture baseline remains `f18d581e`). The working set removed the
DurinEd startup service and notification, MainFrame's dead center request,
LevelEditor upgrade model and dialog, workspace report merging, and
migration-only AssetCore audit/execution APIs. Level, Material, and Texture now
route through `FWorkspaceAssetOpenCompatibility` before activation. AssetCore
captures the pre-request loaded-package set and bulk-releases only packages
introduced by a rejected request, while preserving `FAssetLoadReport`, retained
legacy payloads, low-level structure-upgrader registration, ordinary save
refusal, and general full-package inspection.

Stage 0 validation passed `AssetPackageTests` (30 tests),
`EditorAssetWorkflowTests` (30 tests), the complete `all` editor build, and a
hidden-window Sandbox editor startup/exit smoke test. Targeted obsolete-symbol
searches and `git diff --check` also passed. There are no open Stage 0 questions;
Stage 1 is complete from implementation baseline `ffb2335b`. The working set
adds AssetCore's value-only `FReflectionCompatibilityCatalog`, streaming
`ProbeAssetPackageCompatibility` reader, orthogonal compact record model,
stable finding names, schema-v1 deterministic JSON serialization, fingerprint
freshness checks, and cancellation checkpoints. The reader validates headers,
object/Outer ordering, descriptor lengths, and payload bounds while seeking
over payload bytes; it never constructs objects, resolves dependencies, calls
`PostLoad()`, changes dirty state, or writes authored data.

Stage 1 also checks in frozen hex-encoded DAST fixtures for current,
unknown-field, incompatible-signature, unavailable-class, newer-format,
invalid-object-graph, corrupt, and truncated packages. Validation passed all 34
`AssetPackageTests`, including deterministic reports, bounded metadata memory,
large-payload skipping, stale detection, cancellation, stable serialization,
and every fixture classification. `git diff --check` and the active-plan
validator passed. There are no open Stage 1 questions; this established the
Stage 2 dependency.

Stage 2 is complete from implementation baseline `4b6e0c3f`. The working set
adds DurinEd's request-scoped `FAssetCompatibilityAuditModel`, copied registry
inputs, one cancelable task-system worker, request-serial mailbox, path-keyed
live index, deterministic presentation helpers, fingerprint reconciliation,
and project-switch/shutdown draining. MainFrame now exposes the explicit
non-modal `Tools > Asset Compatibility Audit` window with Run/Cancel/Run Again,
progress, counts, filters, deterministic rows, finding details, stale and
terminal states, diagnostic copying, and Level Editor Content Browser reveal.
Opening and drawing the window only compare the published registry map; `Run
Audit` is the only operation that captures reflection or reads package bytes,
and the UI exposes no authored-data action.

Stage 2 validation passed all 34 `AssetPackageTests`, all 37
`EditorAssetWorkflowTests`, the complete `all` editor build, and a hidden-window
Sandbox editor startup/normal-exit smoke test. The model coverage includes
explicit-start behavior, deterministic ordering, cancellation without partial
records, request serials, rerun replacement, path-keyed add/remove handling,
fingerprint staleness, project changes, shutdown admission, filters, counts, and
copied diagnostics. There are no open Stage 2 questions; Stage 3 is the next
implementation stage.

Stage 3 is complete from implementation baseline `6d6fa381`. The working set
adds the `DevTool asset audit` command, a standalone non-Launch
`DurinAssetAudit` native host, schema-v1 validation and human grouping, and
independent incompatible/unsupported/error exit policies. The native host
enumerates mounted packages without using the persistent registry scan,
initializes only the project/mount/object-reflection/AssetCore boundary, and
feeds the same `ProbeAssetPackageCompatibility` and
`SerializeAssetCompatibilityReportV1`
model used by the editor. It starts no editor workspace, renderer, GPU, import,
source, or DDC service and exposes no authored mutation option.

Stage 3 validation covers command parsing, stable JSON names, deterministic
ordering, human grouping, policy OR combinations, cancellation/process
failure, the checked-in current/incompatible/unsupported/corrupt fixture
classifications, stale and missing records, AssetCore/editor serialization
parity, a focused native-host build, and a live Sandbox JSON audit. There are no
open Stage 3 questions; Stage 4 is the next validation/documentation stage.

Stage 4 is complete from implementation baseline `8d13b822`. Qualification
adds repeatable measurement coverage above the original seven-package
prototype: the 16-package mixed AssetCore corpus averaged 1040 metadata bytes
for both current and incompatible packages, peaked at 366 bytes of retained
metadata, and scanned in 13.097 ms. The existing 4 MiB-plus payload case still
kept metadata below 64 KiB and skipped the payload without proportional
allocation. The 32-package editor-model corpus completed its worker path in
0.559 ms with a 0.024 ms peak mailbox tick, and cooperative cancellation
drained in 0.047 ms on the qualification host. These tests enforce broad
regression ceilings rather than machine-specific benchmark targets.

Stage 4 validation passed all 35 `AssetPackageTests`, all 39
`EditorAssetWorkflowTests`, all DurinDevTool tooling tests, live Sandbox human
and schema-v1 JSON audit parity, the complete `all` editor build, and a hidden
Sandbox editor startup/normal-exit smoke test. Level, Material, and Texture
compatible/incompatible open safety runs through the shared policy and retains
the prior document while releasing only request-owned packages. Active-code
and owning-document searches contain no removed audit service, center request,
workspace merge, Level dialog, destructive compatibility action, or supported
batch write path. The lasting probe/report, workspace rejection, UI ownership,
task mailbox, and command contracts now live in their owning documentation.
There are no open Stage 4 questions.

The design follows the useful separation in Unreal Engine between package
version data, unloaded-asset registry data, explicit validation, and explicit
resave commandlets without copying UE's custom-version or commandlet surface.
DAST remains at format v2, tagged field identity remains authoritative, and no
package custom-version container is introduced without a real production
migration that requires one.

## Goal

Give users and CI one truthful, deterministic, read-only inventory of authored
packages that the current process can load without compatibility loss, while
removing editor controls that imply unsupported upgrading or destructive repair.

## Scope

- Remove the process-wide asset-upgrade audit coordinator, startup notification,
  dead Upgrade Center request, and workspace report-merging hooks.
- Remove the Level asset-structure upgrade model and dialog.
- Apply one shared editor-open policy to Level, Material, Texture, and future
  asset workspaces: a load report containing compatibility issues rejects the
  requested document activation with a diagnostic and releases only packages
  introduced by that request.
- Remove upgrade-specific AssetCore audit, session, and execution APIs that have
  no supported production consumer while retaining general package inspection,
  load reporting, payload retention, and save protection.
- Add a compact package-compatibility probe that reads object and field metadata,
  skips field payload bytes, constructs no objects, invokes no `PostLoad()`,
  resolves no dependencies, and performs no authored write.
- Add an immutable reflection compatibility catalog that workers can consume
  without touching live reflection registries.
- Add an explicit editor audit job with progress, cancellation, deterministic
  results, stale detection, filtering, package details, and Content Browser
  navigation.
- Add a DurinDevTool read-only audit command with equivalent human-readable and
  versioned JSON results plus explicit CI failure policies.
- Add checked-in incompatibility fixtures and native/tooling coverage without
  treating those fixtures as supported migration inputs.

## Non-Goals

- Automatically upgrading, resaving, rewriting, or deleting data from any asset.
- Offering a package-scoped data-loss save action from a document-open dialog or
  from the project audit UI.
- Running a deep compatibility audit automatically during editor startup.
- Publishing a persistent startup progress notification for work the user did
  not request.
- Introducing a package custom-version container, DAST v3, engine-release
  version rewrites, chained migration planner, or generic schema framework.
- Preserving support for retired StaticMesh, texture, material, component, Scene,
  or import-record schemas removed by the current authored baseline.
- Treating source freshness, reimport availability, DDC state, shader versions,
  cooked payload versions, provider versions, or validation rules as authored
  package compatibility.
- Loading dependencies, constructing objects, or running `PostLoad()` on a
  worker thread.
- Adding source-control checkout, remote package repair, or a write-capable CI
  command.
- Adding Content Browser status badges backed by a persistent cross-session
  cache; navigation from current audit results is in scope.

## Design Decisions and Invariants

### Compatibility contract

- The current repository-authored asset set is the only supported production
  baseline. A retired or unknown field is incompatible, not an implicit upgrade
  candidate.
- DAST format version reports only wire compatibility. Tagged reflected field
  identity and recursive type signature report authored structure compatibility.
- `FAssetLoadReport` remains the materialized-load safety report and retains the
  original legacy payloads for the lifetime of that load decision.
- Project audit reports are advisory, read-only summaries. They never retain raw
  field payloads and never authorize persistence.
- Unknown fields, incompatible signatures, unavailable classes, newer package
  formats, malformed object graphs, corrupt bytes, and I/O failures remain
  distinct machine-readable findings.
- Future support for a real historical asset schema requires a separate plan, a
  production-owned migration rule, a checked-in old-format fixture, and an
  explicit resave contract. The presence of generic compatibility plumbing is
  not a migration promise.

### Ownership

- AssetCore owns package metadata streaming, immutable probe inputs, compact
  compatibility findings, registry fingerprints, and deterministic report
  serialization.
- CoreDObject continues to own reflected class and property identities. AssetCore
  captures the live class/property information into a value-only compatibility
  catalog only after required modules have registered their types.
- DurinEd owns the explicit editor audit job, cancellation scope, completed-result
  mailbox, path-keyed in-process index, and read-only editor model.
- MainFrame hosts the Asset Compatibility window and starts work only from an
  explicit user action. `DEditorEngine::Tick()` does not own a perpetual
  compatibility producer.
- Each workspace owns its document load and release transaction but calls the
  same DurinEd compatibility policy before activation.
- DurinDevTool hosts the same AssetCore probe and classification policy in a
  non-editor process. It does not parse editor logs or duplicate classification.

### Report model

- Job state is `Idle`, `Running`, `Completed`, `Cancelled`, or `Failed` and is not
  stored as an asset compatibility classification.
- Each package record has orthogonal `Inspection`, `Compatibility`, and
  `Freshness` fields:
  - `Inspection`: `NotChecked`, `Ready`, or `Failed`.
  - `Compatibility`: `Compatible`, `Incompatible`, or `Unsupported`.
  - `Freshness`: `Current` or `Stale`.
- Mount ownership and writability are descriptive fields. They do not change an
  incompatible package into another compatibility state because this plan has
  no write action.
- Findings contain package path, object path, class identity, declaring type,
  field identity, stored kind/signature, expected kind/signature when known,
  finding code, and diagnostic. Payload size and byte offset may be recorded for
  review, but payload bytes are never copied into the session report.
- The in-process index is keyed by `FAssetPath`. Queue position and presentation
  order never identify a record.
- Presentation is sorted by virtual path. Map replacement, invalidation, and
  completed-result publication are keyed operations and cannot depend on two
  vectors remaining positionally aligned.
- Machine-readable schema version 1 serializes stable string names rather than
  C++ enum ordinals.

### Probe and thread model

- The game thread snapshots registry entries and freezes a value-only reflection
  compatibility catalog before launching work.
- One cancelable CPU task scans the deterministic package sequence. V1 uses one
  worker to avoid unmeasured parallel disk pressure; worker-pool fan-out requires
  later profiling evidence.
- The worker owns copied registry data, immutable catalog data, its file handle,
  and value-only results. It never calls `FindClassByQualifiedName`, resolves a
  `DObject`, reads editor state, mutates the registry, or invokes a contributor
  callback tied to process-global state.
- The package reader validates all lengths and object relationships while
  streaming. It reads object and field descriptors and skips payload ranges
  after bounds validation. A corrupt or truncated payload range is still a
  package failure even though its bytes are not retained.
- The worker checks cancellation between packages and at bounded intervals while
  scanning a large package. Cancellation publishes no partial package record;
  previously completed package records remain reviewable.
- Completed package records move through synchronized request-owned state. The
  DurinEd tick drains only the active request serial, consistent with the CPU
  task-system mailbox contract.
- The registry file size and stable last-write ticks identify the bytes inspected
  by a read-only report. A mismatch before publication marks that path `Stale`.
  A content hash is deliberately not computed because this plan authorizes no
  later write from the report.

### Registry changes and ordering

- An audit snapshots the visible project, engine, and plugin packages once and
  orders them by virtual path.
- Additions after the snapshot do not silently enter the active job. They appear
  as not checked when the result view compares the completed snapshot with the
  current registry.
- Removed, moved, imported, saved, or externally modified packages are reconciled
  by path plus size/timestamp comparison. Only affected paths become stale or
  disappear; an unrelated registry revision does not erase completed results.
- Re-run creates a new request serial and a new snapshot. Cancel and re-run drain
  or detach the previous request before releasing its captured catalog and file
  inputs.
- Project switch, module shutdown, and editor teardown cancel and drain the
  active request before releasing modules, the registry, editor state, or the
  task scheduler.

### Workspace behavior

- A workspace load that reports no compatibility issue follows its existing
  activation and dirty-document flow.
- A workspace load that reports any compatibility issue does not activate the
  requested document. The previous document and world remain unchanged.
- The workspace reports one concise error containing the requested package and
  instructs the user to run Asset Compatibility Audit for complete details. It
  does not label the condition as an upgrade and does not open a modal owned by
  the asset type.
- Packages introduced only by the rejected request are released using the
  existing package ownership rules. Preloaded or otherwise referenced packages
  are not unloaded as a side effect.
- No editor workspace offers `SaveAndOpen`, `OpenWithoutSaving`, or
  `DiscardIncompatibleDataSaveAndOpen` for an incompatible package.
- AssetCore's compatibility-risk save guard remains active even if another
  caller bypasses the editor-open policy.

### Editor and command-line behavior

- MainFrame exposes `Tools > Asset Compatibility Audit`. Opening the window does
  not start a scan until the user selects `Run Audit`.
- The window is non-modal and shows job progress, compatible/incompatible/
  unsupported/failed/stale counts, path and finding filters, deterministic rows,
  exact field diagnostics, re-run, cancel, copy-diagnostics, and Content Browser
  navigation.
- The window exposes no save, upgrade, repair, discard, or suppression action.
- Editor startup emits no compatibility progress notification. A rejected asset
  open may emit one scoped error notification without starting a project audit.
- The initial DurinDevTool surface is `DevTool asset audit`. `--format json`
  emits schema version 1. `--fail-on incompatible`, `--fail-on unsupported`, and
  `--fail-on error` are independently selectable and combine by logical OR.
- Without `--fail-on`, the command returns failure for scan/serialization errors
  but reports incompatible and unsupported packages without changing files.
- Editor and command-line reports use the same paths, finding codes,
  classifications, and deterministic ordering for the same package snapshot.

## Current Foundations and Gaps

| Area | Current foundation | Gap or selected change |
| --- | --- | --- |
| Load safety | `FAssetLoadReport` retains unknown fields and save refuses compatibility-risk loss | Keep the safety boundary; stop presenting unknown data as upgradeable |
| Package header | Bounded header reader reports class, format, dependencies, object count, and bytes read | Add a streaming field-manifest probe that skips payload data |
| Full inspection | `InspectAssetPackage` constructs value-only object/field snapshots for delete, source indexing, and focused tools | Keep supported consumers, but do not use its full-payload snapshot for a project audit |
| Upgrade primitives | AssetCore has audit/session/execute types created for the obsolete workflow | Remove unsupported write-oriented APIs and their migration-only tests |
| Registry | Deterministic package discovery, persistent metadata cache, revision, size, and timestamp | Snapshot and reconcile by path/fingerprint without treating one revision as global invalidation |
| Task system | Bounded cancelable CPU tasks and caller-owned result state | Add one value-only scan request and DurinEd mailbox owner |
| Editor coordinator | Startup `FAssetUpgradeAuditService` publishes copied full-session snapshots | Remove it; replace with an explicit request-scoped audit job |
| Workspace load | Level owns an upgrade modal; Material and Texture merge reports into the global service | Remove both paths and apply one rejection policy before document activation |
| Editor UI | Notification action sets an Upgrade Center request with no host implementation | Remove the dead action; later add a read-only Asset Compatibility window |
| Automation | DurinDevTool has documentation, build, test, and repository command routing | Add a read-only `asset audit` command and stable JSON schema |

## Implementation Stages

### Stage 0: Remove The Obsolete Upgrade Workflow

Dependencies: current authored baseline and existing AssetCore compatibility save
guard.

- [x] Remove `FAssetUpgradeAuditService`, its notification controller, editor
  engine ownership/tick/shutdown API, startup calls, tests, and build entries.
- [x] Remove MainFrame's `RequestOpenAssetUpgradeCenter` interface, retained open
  flag, and notification action wiring.
- [x] Remove LevelEditor `FAssetStructureUpgradeModel`, its dialog presenter,
  decision/result types, pending state, focused tests, and upgrade-specific
  controller callbacks.
- [x] Remove Material, Texture, and Level workspace calls that merge or invalidate
  reports in the obsolete global service.
- [x] Add one DurinEd workspace-open compatibility policy and route Level,
  Material, and Texture loads through it before activation.
- [x] Make incompatible opens reject cleanly, preserve the prior document/world,
  release only request-owned packages, and return a stable diagnostic.
- [x] Remove `EAssetPackageAuditState`, package/session upgrade reports,
  `AuditAssetPackage`, `ExecutePackageUpgrade`, inspection-upgrader registration,
  and tests that exist only for unsupported project-wide migration execution.
- [x] Preserve `FAssetLoadReport`, `FAssetCompatibilityIssue`, legacy payload
  retention, `RegisterAssetStructureUpgrader`, ordinary save refusal, and full
  `InspectAssetPackage` consumers unrelated to the deleted workflow.
- [x] Update Asset Packages and Workspace Framework documentation to describe
  rejection rather than a Level-specific upgrade decision.

#### Acceptance Gate

- Editor startup performs no project asset audit and publishes no upgrade
  notification.
- No editor surface contains `Asset Upgrade Center`, `Asset Structure Upgrade
  Required`, safe-upgrade, or destructive compatibility-save actions.
- Opening an incompatible Level, Material, or Texture leaves the current
  document active and reports the same unsupported-compatibility policy.
- A caller that attempts an ordinary save of a compatibility-risk package is
  still rejected by AssetCore.
- Supported package inspection, deletion, source indexing, import, and Content
  Browser behavior remain covered after migration-only APIs are removed.

### Stage 1: Add Compact Package Compatibility Probing

Dependencies: Stage 0.

- [x] Define the immutable reflection compatibility catalog, compact report
  model, stable finding codes, schema version 1, and deterministic serialization.
- [x] Add a package reader that streams every object and field descriptor,
  validates object/outer relationships and payload bounds, and skips payload
  bytes without copying them.
- [x] Classify current fields, unknown/retired fields, incompatible signatures,
  unavailable classes, unsupported formats, corrupt packages, and I/O failures.
- [x] Record size/timestamp fingerprints and reject or mark a result stale when
  the package no longer matches the registry snapshot.
- [x] Keep the probe independent of dependency loading, `DObject` construction,
  `PostLoad()`, package dirty state, source files, DDC, import providers, and
  editor modules.
- [x] Add checked-in fixtures for current, unknown-field, incompatible-signature,
  unknown-class, newer-format, invalid-object-graph, corrupt, and truncated
  packages.
- [x] Add AssetCore tests for deterministic findings, payload skipping, bounded
  memory, stale detection, cancellation checkpoints, and schema serialization.

#### Acceptance Gate

- Every registry package produces one compact terminal compatibility record
  without object construction, dependency loading, payload retention, or an
  authored-file write.
- Probe memory is bounded by metadata and diagnostic size rather than package
  payload size.
- Current and incompatible fixtures are distinguished by stable finding codes,
  and editor/automation callers need no knowledge of package byte layout.
- DAST v2 and the existing runtime load/save contract remain unchanged.

### Stage 2: Add The Explicit Editor Audit And Read-Only UI

Dependencies: Stage 1 and the existing task-system lifecycle.

- [x] Add a request-scoped DurinEd audit job that snapshots registry data and the
  compatibility catalog, launches one cancelable worker, and drains compact
  results through a synchronized mailbox.
- [x] Key the live result index by `FAssetPath`; keep presentation sorting
  separate from mutation and publication.
- [x] Implement explicit run, cancel, re-run, request-serial rejection, progress,
  project-switch cancellation, and shutdown drain.
- [x] Reconcile completed results with current registry paths and fingerprints so
  only added, removed, moved, saved, imported, or externally changed packages
  become not checked, disappear, or become stale.
- [x] Add MainFrame's non-modal Asset Compatibility window with filters, counts,
  deterministic rows, finding details, empty/cancelled/stale/failure states,
  copy diagnostics, and Content Browser navigation.
- [x] Ensure opening or drawing the window performs no hidden scan, package load,
  payload read, or save; `Run Audit` is the only start action.
- [x] Add model, task-lifecycle, cancellation, stale-reconciliation, project
  switch, shutdown, and interaction tests.

#### Acceptance Gate

- The editor performs no audit until the user explicitly requests it.
- Audit I/O and byte parsing run on a worker over copied inputs; the game thread
  owns editor models, registry comparison, and presentation.
- Cancellation and project switch leave no worker accessing released registry,
  module, or editor state.
- Results remain internally consistent under out-of-order mailbox delivery and
  registry changes because identity is path-keyed rather than position-keyed.
- The UI communicates incompatibility without exposing any write or data-loss
  action.

### Stage 3: Add Read-Only Automation

Dependencies: Stage 1. It may proceed in parallel with Stage 2 after the schema
version 1 contract is frozen, using a separate worktree and build writer.

- [x] Add the DurinDevTool `asset audit` command using the same AssetCore probe,
  report model, finding codes, and deterministic order as the editor.
- [x] Add human-readable grouping and `--format json` schema version 1 output.
- [x] Add independently selectable `--fail-on incompatible`, `--fail-on
  unsupported`, and `--fail-on error` policies with documented exit behavior.
- [x] Ensure the command initializes only the modules and reflection catalog
  required for package compatibility and never starts an editor workspace.
- [x] Validate current, incompatible, unsupported, corrupt, stale, and missing
  package cases against the checked-in fixtures.
- [x] Add tooling tests for argument validation, stable JSON names, deterministic
  ordering, policy combinations, cancellation/process failure, and editor/CLI
  report parity.

#### Acceptance Gate

- CI can detect incompatible or unsupported authored packages without modifying
  files or parsing logs.
- Editor and command-line audits produce equivalent classifications and finding
  identities for the same package snapshot.
- Default and explicit failure policies have stable documented exit behavior.
- No command option permits save, resave, repair, field discard, or source-control
  mutation.

### Stage 4: Complete Validation And Documentation

Dependencies: Stages 0 through 3.

- [x] Run focused AssetCore, DurinEd, MainFrame, LevelEditor, MaterialEditor,
  TextureEditor, and tooling tests through repository entrypoints.
- [x] Measure current-package metadata bytes read, incompatible-package metadata
  bytes read, peak audit memory, worker scan duration, cancellation latency, and
  editor mailbox/frame cost on a corpus large enough to exceed the original
  seven-package prototype.
- [x] Demonstrate that one large-payload package does not allocate memory
  proportional to its payload and does not stall the game thread while scanned.
- [x] Run a complete `all` build and editor startup smoke test because Stage 0
  removes user-visible editor behavior and Stage 2 adds a replacement window.
- [x] Smoke-test compatible and incompatible document opens across Level,
  Material, and Texture workspaces.
- [x] Move lasting probe, report, workspace rejection, UI ownership, and command
  behavior into Asset Packages, Workspace Framework, Task System where needed,
  and DurinDevTool documentation.
- [x] Run all-plan validation and mark this plan complete only after the old
  upgrade terminology and unsupported write path are absent from active code and
  owning documentation.

#### Acceptance Gate

- Focused suites, full build, editor startup, document-open smoke tests, tooling
  parity, and all-plan validation pass.
- Startup time and package I/O are unchanged when the user does not request an
  audit.
- Explicit audit parsing and memory costs satisfy measured bounds on the selected
  representative corpus.
- Owning documentation describes only implemented compatibility behavior and
  contains no promise of automatic or batch asset upgrading.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Obsolete workflow removal | No startup service, notification, dead center request, workspace merge, Level upgrade model, modal, or migration-only AssetCore API |
| Workspace safety | Incompatible Level, Material, and Texture opens preserve the prior document and release only request-owned packages |
| Persistence safety | Ordinary save still rejects compatibility-risk payload loss |
| Package probe | Current, unknown field, signature mismatch, unknown class, unsupported format, corrupt, truncated, and invalid graph fixtures |
| Object isolation | No dependency load, object construction, `PostLoad()`, dirty-state observation, source read, DDC access, or authored write |
| Memory | Payload bytes skipped and peak memory bounded by metadata/diagnostic size |
| Threading | Immutable copied inputs, one cancelable worker, bounded cancellation checks, request-serial mailbox, project-switch and shutdown drain |
| Identity | Path-keyed replacement/invalidation and deterministic presentation order |
| Freshness | Added, removed, moved, saved, imported, and externally changed package reconciliation |
| Editor | Explicit run only, progress, cancellation, filters, details, copy, navigation, and no write action |
| Automation | Human/JSON parity, stable schema, explicit failure policies, and zero file modifications |
| Regression | Focused suites, full `all` build, startup smoke, workspace open smoke tests, and plan validation |

## Definition of Done

- The editor contains no asset-upgrade startup service, notification, dead open
  request, Level upgrade dialog, or destructive compatibility action.
- All supported asset workspaces reject incompatible packages consistently and
  preserve their previous active document or world.
- Users can explicitly audit all registry packages and review deterministic,
  compact, payload-free compatibility findings in a non-modal editor window.
- CI can run the same read-only audit with stable JSON and explicit policies.
- Auditing constructs no objects, loads no dependencies, invokes no `PostLoad()`,
  writes no authored package, and performs no unrequested startup work.
- Registry changes affect only the relevant path records, and result identity
  never depends on positional alignment between queues and reports.
- Lasting contracts live in their owning documentation, required validation
  passes, and no active documentation claims a supported migration or resave
  capability that production code does not provide.

## Deferred Follow-ups

- A separate asset migration and resave plan after the first real production
  legacy schema, owning module, migration rule, retained-data policy, and
  checked-in old-format fixture exist.
- A package custom-version container or save-time compatibility summary if a
  real migration cannot be discovered efficiently from tagged field metadata.
- Write-capable commandlets, source-control checkout, package repair, and batch
  publication after a migration contract exists.
- Persistent cross-session audit caches and Content Browser badges if measured
  projects justify their invalidation and storage complexity.
- Bounded parallel package scanning after profiling shows a benefit without disk
  contention or editor responsiveness regression.
- Validation rules for naming, dependency policy, source freshness, performance,
  or content quality; those belong to a broader asset-validation system rather
  than compatibility auditing.

## Related Documentation

- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Workspace Framework](../../../Editor/Architecture/WorkspaceFramework.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Build And Run](../../../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../../../Development/Build/NativeTests.md)
- [Tagged Asset Field Upgrades Plan](../2026-07/TaggedAssetFieldUpgrades.md)
- [Unreal Engine Asset And Package Versioning](https://dev.epicgames.com/documentation/en-us/unreal-engine/versioning-of-assets-and-packages-in-unreal-engine)
- [Unreal Engine Asset Registry](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-registry-in-unreal-engine)
- [Unreal Engine Data Validation](https://dev.epicgames.com/documentation/en-us/unreal-engine/data-validation-in-unreal-engine)
- [Unreal Engine Resave Packages Commandlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UResavePackagesCommandlet)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Editor/DurinEd/Public/Asset/AssetUpgradeAuditService.h`
- `Engine/Source/Editor/DurinEd/Private/Asset/AssetUpgradeAuditService.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Editor/MainFrame/Public/Interfaces/IMainFrameModule.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/AssetStructureUpgradeModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Documents/AssetStructureUpgradeModel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/DocumentDialogPresenters.h`
- `Engine/Source/Editor/LevelEditor/Private/Documents/DocumentDialogPresenters.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.h`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/AssetUpgradeAuditServiceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/AssetStructureUpgradeModelTests.cpp`
