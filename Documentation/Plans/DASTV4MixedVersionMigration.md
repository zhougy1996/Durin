# DAST V4 Mixed-Version Migration Plan

Summary: Separate supported readers from the latest writer and add explicit atomic v3-to-v4 migration without bulk-resaving tracked content.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

Stage 0 is complete on baseline `c3c09c13`. `AssetPackageVersionPolicy.h` now
owns the DAST magic, v3/v4 format identities, supported-reader set, reader
selection, latest writer, ordinary-save writer, and explicit-migration writer.
All former production `AssetVersion` decisions use the named policy while
retaining v3-only ordinary behavior. The frozen v4 writer and bounded header,
logical, live, compatibility, and reference readers remain behind explicit
low-level entries; Stage 1 must connect those entries to read-only dispatch.

## Goal

Support bounded v3/v4 discovery and loading during one reviewed migration
window, select v4 for explicit migration output, and migrate one authorized
package bundle atomically while preserving compatibility and rollback.

## Scope

- Separate latest-writer version from the supported-reader set.
- Activate v4 header, inspection, compatibility, reference, registry/cache, and
  live loading behind mixed-version policy.
- Add explicit failure-atomic v3-to-v4 migration for selected package bundles.
- Qualify mixed corpora, cache invalidation, rollback, determinism, and data-loss
  consent without changing tracked authored assets.

## Non-Goals

- Bulk resaving or editing tracked `.dasset` files.
- Final rollout, editor render/save/restart qualification, or v3 retirement.
- Changing frozen v4 bytes, defaults, provenance, unknown retention, or codecs.
- Silent migration during discovery, inspection, loading, or ordinary saves.

## Design Decisions and Invariants

- Supported readers and latest writer are independent version policies.
- Discovery and inspection are read-only; migration requires an explicit
  selected package set and uses the existing atomic bundle journal.
- Unknown closures and payloads remain exact, and compatibility data loss still
  requires explicit consent.
- Any decode, dependency, upgrade, write, stage, publication, cache, or registry
  failure restores files, residency, registry contents, dirty state, and reports.
- V3 remains supported until the separate rollout plan migrates the tracked
  corpus and passes editor qualification.

## Current Foundations and Gaps

AssetCore owns bounded v3 policy paths, atomic bundle publication, registry
fingerprints, migration reports, and explicit data-loss gates. The v4 reader and
writer own qualified explicit codecs. Policy dispatch still assumes one v3
version across discovery, loading, compatibility, registry, and saving.

## Implementation Stages

### Stage 0: Freeze mixed-version policy ownership

- [x] Select supported-reader and latest-writer APIs and enumerate every current
  `AssetVersion` policy use.
- [x] Map file, cache, registry, residency, report, and dirty-state rollback.
- [x] Record mixed-corpus, migration, malformed, and failure-injection fixtures.

#### Acceptance Gate

- Version dispatch has one owner per policy decision and cannot trigger a write
  from discovery, inspection, registry scanning, or loading.

#### Stage 0 Handoff

- Baseline: `c3c09c13` (`feat(asset): add bounded DAST v4 reader`).
- Working set: `AssetPackageVersionPolicy.h`; the v3 archive, compatibility,
  migration, and asset-system policy sites; `AssetPackageV4Writer.h`; and
  `AssetPackageVersionPolicyTests.cpp`.
- Key symbols: `SupportedAssetPackageReaderVersions`,
  `SelectAssetPackageReader`, `LatestAssetPackageWriterVersion`,
  `OrdinaryAssetPackageWriterVersion`, and
  `AssetPackageMigrationWriterVersion`. Selection is pure `constexpr` policy;
  it performs no I/O, construction, callbacks, registry work, or publication.
- Policy-use map: `AssetPackageArchive.cpp` owns ordinary v3 serialization;
  `AssetCompatibility.cpp` owns the current v3 probe until Stage 1 dispatch;
  `AssetSystem.cpp` owns v3 parsing, registry/reference cache policy, ordinary
  publication metadata, and v3 byte-tool defaults; `AssetMigration.cpp` still
  targets the ordinary v3 writer until Stage 3; DAST v4 codec identity aliases
  the central policy rather than defining another version decision.
- Rollback map: authored bytes use staged/pre/post/manifest files and reverse
  restoration; package residency uses `FAssetPackageLoadSnapshot` plus
  `ReleasePackagesLoadedSince`; registry/reference caches publish only after a
  successful scan/save and must retain their prior snapshot on later mixed
  failures; reports and `ChangedPaths` publish only from the final result;
  dirty flags clear only after atomic bundle file and registry publication.
  Stage 2 must extend the root live-load transaction across mixed dependency
  graphs, and Stage 3 must reuse these boundaries for v4 migration output.
- Fixture map: v3 compatibility hex fixtures cover current, unknown-field,
  unknown-class, invalid-graph, incompatible-signature, corrupt, and truncated
  inputs; v4 reader/writer fixtures cover header/directory malformation, bounds,
  exact retained unknowns, live publication, PostLoad, and injected failures;
  package tests cover deterministic dependency-aware migration planning,
  atomic bundle file/registry/dirty rollback, truncated-load cache isolation,
  and registry/reference cache corruption and reconciliation. Stage 1 adds
  paired equivalent v3/v4 mixed-corpus inputs; Stages 2-3 extend the existing
  phase injectors rather than creating an independent failure model.
- Open questions: none for Stage 1. Registry and reference cache fingerprints
  must encode the selected reader policy before v4 entries may be reused.
- Validation: `AssetPackageTests` passed all 122 tests, including the new
  compile-time/pure policy checks; no tracked `.dasset` content changed.

### Stage 1: Activate read-only v3/v4 dispatch

- [ ] Route header, inspection, compatibility, and reference operations through
  supported-reader dispatch.
- [ ] Version registry/cache fingerprints and qualify incremental mixed scans.
- [ ] Preserve construct-free behavior, freshness, findings, costs, and exact
  retained data across equivalent v3/v4 content.

#### Acceptance Gate

- Mixed corpora scan and inspect deterministically without construction,
  callbacks, writes, registry drift, or stale cache reuse.

### Stage 2: Activate transactional mixed-version live loading

- [ ] Route ordinary loading to the bounded v3 or v4 live reader by header.
- [ ] Integrate dependency cycles, compatibility-risk ownership, reports,
  custom versions, PostLoad, and root transaction rollback.
- [ ] Qualify v3/v4 dependency graphs and every injected failure boundary.

#### Acceptance Gate

- Equivalent v3/v4 packages publish equivalent live graphs, and every failure
  preserves the prior cache, registry, dirty state, report, and residency.

### Stage 3: Add explicit atomic v3-to-v4 migration

- [ ] Select v4 as migration output without changing ordinary save output.
- [ ] Plan, revalidate, stage, publish, verify, and compensate selected bundles
  through the shared journal and data-loss gates.
- [ ] Prove deterministic resave, dependency closure, stale-input rejection,
  retained-unknown preservation, and complete rollback.

#### Acceptance Gate

- An explicitly selected bundle migrates atomically to byte-deterministic v4;
  dry-run and failure paths leave authored files and runtime state unchanged.

### Stage 4: Qualify and hand off to rollout

- [ ] Run focused codec, package, compatibility, registry, migration, asset
  baseline, documentation, hash, and full-build validation.
- [ ] Move lasting version-policy and migration contracts into Runtime docs.
- [ ] Complete this plan and activate only the qualification/rollout child.

#### Acceptance Gate

- Mixed readers, v4 migration output, cache/registry policy, rollback, hashes,
  asset baseline, and full build pass without modifying tracked content.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Version policy | Supported-reader/latest-writer separation and isolated dispatch |
| Read-only paths | Mixed header, inspection, compatibility, references, freshness, cost |
| Live load | Mixed graphs, dependencies, reports, PostLoad, complete rollback |
| Migration | Explicit selection, deterministic v4 output, journal compensation |
| Unknowns | Exact closure/payload retention and unchanged data-loss consent |
| Registry/cache | Versioned fingerprints, incremental/full parity, no stale reuse |
| Isolation | Ordinary saves and tracked authored content remain unchanged |
| Qualification | Focused suites, baseline/hashes, docs, full build |

## Definition of Done

- V3 and v4 are bounded supported readers while v4 is selected only for
  explicit migration output.
- Mixed discovery, inspection, loading, registry, and cache behavior is
  deterministic and transactional.
- Explicit selected bundles migrate atomically with exact unknown retention and
  existing compatibility/data-loss guarantees.
- Tracked assets remain unchanged and only the rollout plan is activated.

## Deferred Follow-ups

- Tracked-content rollout and editor load/render/save/restart qualification.
- Latest-writer switch for ordinary saves and eventual v3 reader retirement.
- Custom Struct codecs only if a production audit establishes need.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Reader.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetMigration.h`
