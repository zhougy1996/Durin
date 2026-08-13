# Asset Canonical Resave Plan

Summary: Add package-level canonical resave detection, interactive actions, and bounded batch maintenance so legacy reflected type names can be retired without asset-specific upgrade buttons.

Last reviewed: 2026-08-13

Status: Active
Completed:

## Current Status

`DCLASS`, `DSTRUCT`, and `DENUM` accept read-only `LegacyNames` aliases. Asset
loading resolves those serialized aliases to the current reflected identity, and
the current writer emits the current identity on the next serialization.
`DImportRecord` and its durable value types use this mechanism for the
`Durin::AssetImport` to `Durin::Asset::Import` namespace move, with native tests
covering legacy load and canonical reserialization.

The editor has no package-level resave action, no persistent distinction between
ordinary authored changes and a clean package that should be canonically
rewritten, and no bounded scan that identifies which packages actually contain
legacy reflected identities. Users can reveal an import record from one of its
managed outputs, but reimport is currently the only prominent write-oriented
action and is semantically too broad for identity canonicalization.

No implementation stage has started.

## Goal

Make reflected-name canonicalization an observable, safe, package-level
maintenance workflow that:

- detects the exact current-format packages that consumed or contain a registered
  legacy class, struct, or enum identity;
- keeps canonical-resave recommendations separate from unsaved authored changes;
- lets users resave one package, a selection, an associated import record, or an
  explicitly selected project scope without reimporting source data;
- writes only current reflected identities and verifies the result before
  reporting success; and
- provides sufficient audit and CI evidence to retire a legacy alias after all
  supported content baselines have been upgraded.

## Scope

- Current ordinary-writer `.dasset` packages on authoring-writable mounts.
- Legacy class, struct, and enum identities registered through `LegacyNames`.
- Structured load and metadata-probe evidence for canonical-resave decisions.
- A generic package resave service shared by editor and command-line callers.
- Content Browser actions for individual and selected assets.
- An associated-record action reachable from an output managed by
  `DImportRecord`.
- Explicit folder, mount, and whole-project batch planning and application.
- Deterministic reports, stale-input rejection, verification, cancellation, and
  recovery appropriate to the selected publication boundary.
- Native, editor-model, and command-line coverage plus lasting contract and user
  workflow documentation.

## Non-Goals

- Reimporting source files, regenerating imported outputs, or changing import
  settings, provider state, output fingerprints, or reconciliation policy.
- Package-format migration. Non-current formats continue to use the explicit
  `asset migrate` exact-edge workflow.
- Recovering unknown fields, incompatible field signatures, unavailable classes,
  corrupt packages, or any operation requiring data-loss consent.
- Automatically rewriting authored files merely because they were browsed or
  loaded.
- Adding a separate save implementation or toolbar button to every asset class or
  asset editor.
- Removing any `LegacyNames` declaration as part of the first rollout.
- Resaving cooked, transient, redirector, engine-owned read-only, or otherwise
  non-authorable content.

## Design Decisions and Invariants

1. Canonical resave is a package capability, not an asset-class capability.
   Every authorable ordinary-writer package can use the same service, while only
   packages with positive legacy-identity evidence are recommended for upgrade.
2. A canonical-resave recommendation is not ordinary Dirty state. Loading or
   inspecting a package through a legacy alias must leave authored-change state,
   transaction checkpoints, and close-confirmation behavior unchanged.
3. Detection is exact and structured. Evidence records package path, stored
   identity, current identity, reflected kind, and serialized location. A simple
   string-prefix heuristic or blind project-wide rewrite is not acceptable.
4. Ordinary qualified-name lookup remains current-only. Detection and loading
   use serialized-name lookup; adding maintenance support must not make legacy
   aliases valid construction or runtime identities.
5. `Resave Package` serializes the currently loaded package state with the
   ordinary writer but performs no provider invocation, import plan, source read,
   or output reconciliation. `Reimport` remains a separate action.
6. Resave preflight rejects non-current formats, redirectors, read-only mounts,
   stale fingerprints, dirty-package conflicts not owned by the caller, active
   incompatible load reports, and compatibility-risk payloads. It never requests
   data-loss consent.
7. Publication uses existing atomic package-save primitives for an interactive
   package or bounded selection. Project maintenance applies deterministic
   bounded batches, reports per-package terminal state, and never claims
   all-or-nothing project atomicity.
8. Each published package is reread through the bounded inspection path. Success
   requires a current writer format, a current registry projection, and zero
   remaining legacy-identity evidence for that package.
9. The Content Browser owns presentation and selection routing; AssetCore owns
   detection, authorization, serialization, publication, and verification.
   Import framework code only resolves a managed output to its record package.
10. `DImportRecord` gets no private serializer or migration path. Its managed
    outputs expose a convenience route to the same generic package resave
    service.
11. The existing explicit package-format migration kind and command retain their
    current meaning. Canonical identity rewriting is exposed as a separate
    resave/maintenance operation and report schema.
12. Legacy aliases remain registered until every supported authored-content
    baseline reports zero use and CI rejects regressions. A local successful
    resave or an editor button is not sufficient removal evidence.

## Current Foundations and Gaps

### Foundations

- Generated reflection metadata registers read-only serialized aliases for
  classes, structs, and enums while preserving current runtime identities.
- DAST v4 load canonicalizes legacy reflected identities, and the current writer
  serializes current identities.
- `DPackage` already distinguishes Dirty and clean authored state, while
  `SavePackage` and `SavePackagesAtomically` provide ordinary-writer publication.
- AssetCore has bounded compatibility probing, deterministic package
  fingerprints, registry reconciliation, and explicit rejection of compatibility
  risk during ordinary saves.
- Content Browser context menus already resolve import-record management and can
  reveal a record from a managed output.
- The command-line asset tooling already supports explicit selection, JSON
  reports, cancellation, and migration recovery patterns that can inform, but
  must not be conflated with, canonical resave.

### Gaps

- Alias resolution does not expose durable, per-package canonicalization evidence
  to editor or maintenance callers.
- The read-only compatibility catalog/probe does not classify a compatible
  package that merely contains a registered legacy identity as maintenance due.
- Loaded packages have no non-Dirty `ResaveRecommended` state or query.
- Content Browser has no generic `Save`, `Resave Package`, or multi-selection
  resave action.
- Managed outputs have no direct `Resave Import Record` action.
- There is no preview/apply workflow or CI report for canonical-name debt.
- There is no repository rule defining when a `LegacyNames` alias may be removed.

## Implementation Stages

### Stage 0: Freeze The Maintenance Contract And Fixtures

- [ ] Define stable reflected-kind and serialized-location values for class,
  struct, and enum alias evidence in package headers, object records, schemas,
  and recursive type descriptors.
- [ ] Define a canonical-resave probe result with orthogonal inspection,
  recommendation, freshness, and blocker states; do not overload
  `EAssetPackageCompatibility` with authored-maintenance meaning.
- [ ] Define deterministic plan/apply report schemas and terminal package states
  for skipped, ready, resaved, blocked, failed, cancelled, and stale inputs.
- [ ] Freeze current-format fixtures containing legacy root class, nested object
  class, struct, enum, and mixed current/legacy identities, including an
  `DImportRecord` fixture.
- [ ] Record the selected bounded-batch publication and recovery policy, maximum
  work limits, cancellation points, and registry reconciliation boundary.

#### Acceptance Gate

- Fixture bytes demonstrably contain the intended legacy identities before load,
  and the approved result/report contracts distinguish compatible maintenance
  debt from incompatibility and Dirty state without unresolved ownership or
  publication decisions.

### Stage 1: Add Exact Legacy-Identity Evidence

- [ ] Expose an immutable serialized-identity catalog containing current class,
  struct, and enum names plus their registered legacy aliases; capture it after
  reflection registration for worker-safe inspection.
- [ ] Extend DAST v4 read telemetry so successful alias resolution records the
  stored identity, canonical identity, reflected kind, serialized location, and
  owning package without changing decoded runtime identity.
- [ ] Add a bounded current-format metadata probe that finds registered legacy
  identities without constructing assets, loading dependencies, invoking
  `PostLoad`, changing Dirty state, or writing files.
- [ ] Publish structured per-package canonicalization evidence in load reports and
  expose a non-Dirty loaded-package `IsCanonicalResaveRecommended` query.
- [ ] Preserve exact failure semantics for unknown identities: a registered
  legacy alias is maintenance debt, while an unavailable type remains a
  compatibility blocker.
- [ ] Make evidence ordering and diagnostics deterministic and bounded.

#### Acceptance Gate

- Native tests show identical evidence from unloaded probing and live loading for
  every frozen alias location; current-only packages produce no evidence, and all
  inspection/load paths leave package Dirty state unchanged.

### Stage 2: Implement The Generic Canonical Resave Service

- [ ] Add read-only planning APIs for one package and deterministic selections,
  capturing registry version, physical fingerprint, recommendation evidence,
  authoring policy, format version, residency, Dirty state, and compatibility
  blockers.
- [ ] Add an apply API that revalidates the plan, loads through the ordinary
  current-format path when needed, serializes with the ordinary writer, and uses
  existing atomic save primitives without invoking import providers.
- [ ] Preserve caller-owned loaded residency and authored Dirty state on every
  pre-publication failure; reject unrelated dirty loaded packages instead of
  silently overwriting them.
- [ ] Stage bounded batches deterministically, stop admission on cancellation,
  finish or compensate the active atomic publication unit, and retain explicit
  results for already completed packages.
- [ ] Verify published bytes and fresh metadata-probe results before clearing the
  recommendation and publishing registry changes.
- [ ] Ensure a no-evidence package is skipped by maintenance apply unless an
  interactive caller explicitly requested a plain package resave.
- [ ] Add failure injection for load, stale revalidation, serialization, staging,
  publication, verification, registry reconciliation, and compensation.

#### Acceptance Gate

- Generic service tests prove that selected legacy identities are replaced by
  current names, payload semantics are unchanged, no source/import work occurs,
  blockers perform no authored write, stale plans are rejected, and injected
  failures leave either verified prior bytes or an explicit recovery-required
  result.

### Stage 3: Add Content Browser And Import Record Actions

- [ ] Add generic `Save Package` and `Resave Package` actions for an authorable
  asset, with multi-selection `Resave Selected Packages` routed through the
  shared planner.
- [ ] Enable `Save Package` only for ordinary Dirty state; allow explicit
  `Resave Package` for a clean compatible current-format package, and surface
  exact disabled reasons from shared preflight.
- [ ] Present `Resave recommended` separately from unsaved/Dirty decoration in
  asset details and context actions.
- [ ] Add `Resave Import Record` beside `Reveal Import Record` for a managed
  output and for a selected record, resolving the association through the
  import-record index before invoking the generic service.
- [ ] Keep `Reimport`, `Recreate Missing Outputs`, repair, detach, and resave
  labels and confirmations semantically distinct.
- [ ] Refresh registry, Content Browser snapshots, selected details, and
  associated-record inspection only after verified publication.

#### Acceptance Gate

- Editor-model and integration tests show that a user can upgrade a legacy
  `DImportRecord` from either the record or a managed output, generic asset
  classes use the same package action, disabled reasons are stable, and no
  resave interaction executes a provider or changes output state.

### Stage 4: Add Explicit Batch Maintenance And CI Enforcement

- [ ] Add `Tools > Asset Maintenance > Canonical Resave` with read-only scan,
  filters, counts, evidence details, Content Browser navigation, explicit scope,
  preview, apply, progress, cancellation, and terminal results.
- [ ] Support selected packages, folders, mounts, and an explicitly confirmed
  whole-project scope; default to recommended packages rather than every package.
- [ ] Add a separate command-line canonical-resave command with dry-run default,
  explicit `--apply`, deterministic human/JSON reports, package/mount/project
  selection, cancellation, and failure exit policies.
- [ ] Reject batch apply while an incompatible audit state, dirty package,
  non-current format, read-only mount, or stale fingerprint blocks a selected
  package, while allowing the report to enumerate all independent blockers.
- [ ] Add a CI mode that fails when selected supported content contains registered
  legacy identities, without writing content.
- [ ] Add interruption/recovery and repeat-run coverage; a successful second scan
  after apply must be empty and a second apply must be a no-op.

#### Acceptance Gate

- The editor and command-line tool produce equivalent deterministic plans for the
  same snapshot, apply rewrites only recommended current-format packages, partial
  batch outcomes are explicit and recoverable, and CI detects a reintroduced
  legacy identity without mutating authored files.

### Stage 5: Document The Contract And Establish Alias Retirement Gates

- [ ] Update the reflection contract with alias evidence, canonical-resave
  recommendation semantics, and the rule that ordinary qualified lookup remains
  current-only.
- [ ] Update asset-package documentation with canonical resave authorization,
  publication, verification, blocker, and command-line boundaries, keeping it
  distinct from package-format migration and incompatible-schema recovery.
- [ ] Update Content Browser and import framework documentation with generic
  resave actions and managed-output routing.
- [ ] Add a user guide for individual, selected, and project maintenance flows,
  including source-control review and blocked-package diagnostics.
- [ ] Record a `LegacyNames` retirement checklist requiring zero findings across
  every supported content baseline, passing CI enforcement, reviewed authored
  diffs, and a compatibility-policy decision for external content.
- [ ] Run the full documentation, native-test, editor integration, and applicable
  command-line validation matrix before completing the plan.

#### Acceptance Gate

- Long-lived contracts and the user workflow describe the shipped behavior,
  validators pass, supported baselines report zero legacy identities after the
  controlled upgrade, and no alias is removed without separately recorded
  retirement evidence.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Reflection registry | Current lookup rejects legacy aliases; serialized lookup and the immutable inspection catalog resolve each registered class, struct, and enum alias exactly once. |
| DAST v4 inspection | Header, object, schema, struct, enum, nested container, and mixed-name fixtures produce bounded deterministic evidence without object construction. |
| Live load | Legacy packages load with current runtime identities, expose matching per-package evidence, and remain clean. |
| Serialization | Canonical resave produces deterministic current-writer bytes with no registered legacy identities and preserves object values, references, import state, and package identity. |
| Safety blockers | Read-only mounts, redirectors, non-current formats, incompatible/unknown fields, dirty conflicts, stale fingerprints, corruption, and unavailable types perform no authored write. |
| Atomic publication | Injected failures and cancellation preserve or restore prior bytes for the active publication unit, accurately report completed units, and reconcile the registry only after verification. |
| Content Browser | Single/multi-selection enablement, disabled reasons, recommendation decoration, refresh, and generic asset routing are covered without asset-editor-specific implementations. |
| Import records | Record selection and managed-output routing resave only the companion record and never execute reimport, recreate, repair, or detach behavior. |
| Batch editor tool | Scan, filtering, preview, apply, cancellation, stale reconciliation, navigation, and repeat-run no-op behavior are covered. |
| Command line and CI | Dry-run/apply selection and JSON determinism match editor planning; CI detects debt and remains read-only. |
| Documentation | Changed-scope and all-plan validation pass; lasting reflection, package, Content Browser, import, and user workflow contracts are updated. |

Build and native-test selection follow
[Agent Build And Run Workflow](../Agents/BuildAndRun.md) and
[Agent Testing Workflow](../Agents/Testing.md); stage work must use the smallest
applicable targets before broader validation.

## Definition of Done

- All implementation-stage acceptance gates pass with recorded evidence.
- Any current-format authorable asset package can use the shared resave service;
  concrete asset editors contain no duplicate resave implementation.
- Legacy identity use is detected exactly for unloaded and loaded packages and is
  never represented as ordinary Dirty state.
- A `DImportRecord` can be canonically resaved from itself or a managed output
  without source reimport or output mutation.
- Editor and command-line batch workflows rewrite only planned packages, verify
  current identities, and report blockers and partial outcomes deterministically.
- CI can enforce zero legacy identities across selected supported content without
  writing files.
- Lasting contracts and the user guide are current, and every required validator
  passes.
- Alias removal remains a separate evidence-backed compatibility decision.

## Deferred Follow-ups

- Automatic source-control checkout or changelist creation for batch resave.
- Background opportunistic resave during idle editor time.
- Schema transforms for renamed fields or changed field signatures.
- Data-loss-authorized recovery of unknown compatibility payloads.
- Multi-hop or asset-schema migration graphs.
- Automatic removal of `LegacyNames` metadata after a time or version threshold.

## Related Documentation

- [Generated Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build And Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Private/DObject/Class.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/QualifiedTypeRegistry.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetCompatibility.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetMigration.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetMigration.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Editor/AssetImportCore/Public/ImportRecord.h`
- `Engine/Source/Editor/AssetImportCore/Private/ImportRecord.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/ImportRecordIndex.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/MainFrame/Private/AssetCompatibilityWindow.cpp`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/ImportRecordTests.cpp`
