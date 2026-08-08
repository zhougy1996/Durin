# DAST V4 Qualification and Rollout Plan

Summary: Qualify editor workflows, explicitly migrate the tracked authored corpus to DAST v4, switch ordinary saves, and retire the temporary v3 path.

Last reviewed: 2026-08-09

Status: Active
Completed:

## Current Status

Activated after the mixed-version migration exit gate passed on 2026-08-09.
AssetCore can read v3/v4 packages and explicitly migrate journal-backed bundles
to deterministic v4, while ordinary saves and all 17 tracked packages remain
v3. No tracked authored content has been changed by activation.

## Goal

Qualify the mixed-version editor path, explicitly migrate the complete tracked
corpus to v4, select v4 for ordinary saves, and remove the temporary v3 reader
and migration edge only after editor restart and hash/baseline gates pass.

## Scope

- Record the authorized 17-package rollout inventory and pre-migration hashes.
- Qualify editor load, render, save, unload, reload, and restart on mixed content.
- Apply one explicit atomic migration to the complete tracked corpus and review
  every changed package.
- Switch ordinary authored saves and the repository baseline to v4.
- Retire the temporary v3 reader, migration edge, fixtures, and compatibility
  branches after the corpus has no v3 package.

## Non-Goals

- Silent migration during discovery, inspection, loading, or editor startup.
- External-project compatibility promises or downloadable migration bundles.
- Reopening the frozen v4 wire, default, provenance, retention, or codec contract.
- Adding custom Struct codecs without new production audit evidence.

## Design Decisions and Invariants

- Content changes require an explicit reviewed migration apply over the complete
  authorized inventory; discovery and qualification remain read-only beforehand.
- The bundle journal, stale-input checks, compatibility/data-loss gates, and
  registry/cache compensation remain the sole publication transaction.
- Ordinary writer activation follows successful mixed editor qualification and
  corpus migration; v3 retirement follows a clean all-v4 baseline.
- Every failure preserves or restores authored bytes, registry/cache state,
  residency, dirty state, reports, and editor reopenability.

## Current Foundations and Gaps

The completed mixed-version plan owns bounded v3/v4 dispatch, transactional live
graphs, deterministic no-delta v4 migration output, and journal-backed rollback.
The tracked corpus is still v3, the ordinary writer and baseline still select
v3, editor render/save/restart has not been qualified against mixed or migrated
content, and the temporary v3 path remains required.

## Implementation Stages

### Stage 0: Freeze rollout authorization and evidence

- [ ] Record the exact tracked-package inventory, Git object hashes, format
  versions, dependency closure, and expected migration selection.
- [ ] Freeze editor scenarios, visual checks, backup/rollback evidence, and
  acceptance outputs before any authored byte changes.
- [ ] Prove dry-run selection is the complete 17-package corpus and changes no
  file, registry/cache, residency, report, or dirty state.

#### Acceptance Gate

- The reviewed inventory, dry-run, hashes, and rollback protocol identify the
  exact atomic content change and no authored byte has changed.

### Stage 1: Qualify the mixed-version editor path

- [ ] Exercise representative v3/v4 dependency graphs through editor open,
  render, save, unload, reload, and hidden-window restart.
- [ ] Verify read-only startup/audit paths and ordinary saves do not migrate
  packages, and injected failures preserve the previous editor/runtime state.

#### Acceptance Gate

- Mixed content renders and survives save/reload/restart without implicit
  migration, stale cache reuse, registry drift, or state leakage.

### Stage 2: Migrate the tracked corpus atomically

- [ ] Apply the authorized complete-corpus migration once through the bundle
  journal and review every changed `.dasset` plus the migration report.
- [ ] Verify every package is deterministic clean v4, dependency-closed, and
  byte-identical after a no-delta v4 resave.
- [ ] Repeat editor load/render/unload/reload/restart qualification on the
  migrated corpus.

#### Acceptance Gate

- All 17 tracked packages are reviewed deterministic v4 outputs, the v3 corpus
  is empty, and editor/runtime qualification passes with no sidecars remaining.

### Stage 3: Activate v4 ordinary saves and retire v3

- [ ] Select v4 for ordinary authored saves and update the repository baseline.
- [ ] Remove the temporary v3 reader, exact migration edge, obsolete dispatch,
  fixtures, and compatibility branches after the all-v4 gate passes.
- [ ] Prove new and repeated saves are deterministic v4 and unsupported versions
  fail before mutation or publication.

#### Acceptance Gate

- V4 is the only authored reader/writer and baseline; no production v3 path or
  tracked v3 package remains.

### Stage 4: Complete rollout qualification

- [ ] Run focused codec, package, compatibility, registry/cache, editor,
  rendering, baseline/hash, documentation, and full-build validation.
- [ ] Move final v4-only policy and retirement contracts into Runtime docs and
  complete the serialization roadmap.

#### Acceptance Gate

- The all-v4 repository passes editor load/render/save/restart, deterministic
  resave, hashes, baseline, focused suites, documentation, and full build.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Authorization | Exact 17-package inventory, hashes, dry-run selection, reviewed apply |
| Editor | Mixed and all-v4 load, render, save, unload, reload, restart |
| Migration | Complete dependency closure, journal compensation, no sidecars |
| Determinism | V4 decode/re-encode and repeated ordinary saves are byte-identical |
| Compatibility | Unknown retention and data-loss consent remain fail-closed |
| Registry/cache | Versioned fingerprints, full/incremental parity, no stale reuse |
| Retirement | No tracked v3 package or production v3 reader/migration branch remains |
| Qualification | Focused suites, baseline/hashes, docs, and full `all` build |

## Definition of Done

- The complete tracked corpus is explicitly migrated and reviewed as v4.
- Editor load/render/save/restart and deterministic resave pass on all-v4 content.
- Ordinary saves and the repository baseline select v4.
- The temporary v3 reader and migration edge are removed.
- Lasting v4-only contracts reside in Runtime documentation and the roadmap is complete.

## Deferred Follow-ups

- External release compatibility and downloadable migration bundles.
- Custom Struct asset codecs only if a production audit establishes need.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetMigration.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageVersionPolicy.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Content/Materials/DefaultMaterial.dasset`
