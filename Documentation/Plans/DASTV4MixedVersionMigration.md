# DAST V4 Mixed-Version Migration Plan

Summary: Separate supported readers from the latest writer and add explicit atomic v3-to-v4 migration without bulk-resaving tracked content.

Last reviewed: 2026-08-09

Status: Completed
Completed: 2026-08-09

## Current Status

Complete on 2026-08-09. AssetCore now separates bounded v3/v4 readers from the
ordinary v3 writer and explicit v4 migration writer; mixed discovery,
inspection, compatibility, references, registry/cache, and live loading are
read-only and transactional. Explicit selected bundles migrate through the
existing journal to deterministic no-delta v4 with complete compensation.
Qualification passed without changing any tracked `.dasset`, lasting contracts
now reside in Runtime docs, and only the DAST V4 Qualification and Rollout child
has been activated for tracked-content migration and v3 retirement.

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

- [x] Route header, inspection, compatibility, and reference operations through
  supported-reader dispatch.
- [x] Version registry/cache fingerprints and qualify incremental mixed scans.
- [x] Preserve construct-free behavior, freshness, findings, costs, and exact
  retained data across equivalent v3/v4 content.

#### Acceptance Gate

- Mixed corpora scan and inspect deterministically without construction,
  callbacks, writes, registry drift, or stale cache reuse.

#### Stage 1 Handoff

- Baseline: `8f5524c0` (`feat(asset): define mixed-version package policy`).
- Working set: `AssetSystem.h`, `AssetSystem.cpp`, `AssetCompatibility.cpp`,
  `AssetPackageV4Reader.cpp`, `AssetPackageVersionPolicy.h`, and mixed package
  tests. No authored `.dasset` files changed.
- Key symbols and decisions: `ReadAssetPackageHeader`,
  `InspectAssetPackageBytes`, and `ProbeAssetPackageCompatibility` dispatch via
  `SelectAssetPackageReader`; v4 failures are corrupt supported input rather
  than unsupported format. `AssetPackageReaderPolicyFingerprint` invalidates
  persisted registry/reference snapshots when the supported-reader contract
  changes. `FAssetPackageFingerprint::ReaderVersion` qualifies each source and
  must match `FAssetData::FormatVersion` before reference-cache reuse.
- Read-only guarantees: v4 header and inspection use the bounded frozen reader;
  compatibility preserves physical identity, content/report hashes, freshness,
  findings, and reader cost statistics; reference projection consumes the
  construct-free v4 inspection payload. Full scans publish only their final
  registry/reference projections, and incremental scans reuse both v3 and v4
  sources without payload reads or revision drift.
- Open questions: none for Stage 2. Mixed live loading must reuse the v4 live
  reader's owned graph without publishing it until the root dependency
  transaction succeeds.
- Validation: focused mixed dispatch, malformed header, and compatibility tests
  passed; `AssetPackageTests` passed all 123 tests. Existing v4 retained-closure,
  bounded-cost, reference, and no-publication tests remain green.

### Stage 2: Activate transactional mixed-version live loading

- [x] Route ordinary loading to the bounded v3 or v4 live reader by header.
- [x] Integrate dependency cycles, compatibility-risk ownership, reports,
  custom versions, PostLoad, and root transaction rollback.
- [x] Qualify v3/v4 dependency graphs and every injected failure boundary.

#### Acceptance Gate

- Equivalent v3/v4 packages publish equivalent live graphs, and every failure
  preserves the prior cache, registry, dirty state, report, and residency.

#### Stage 2 Handoff

- Baseline: `e4ff1830` (`feat(asset): activate mixed-version read dispatch`).
- Working set: `AssetPackageV4Reader.h/.cpp`, `AssetSystem.h/.cpp`, and package
  live-load tests. No authored `.dasset` files changed.
- Key symbols and decisions: `FAssetManager::LoadPackageInternal` selects v3 or
  v4 from the package preamble. `DastV4::FLiveLoadOptions::OnSkeletonReady` and
  `OnSkeletonRollback` expose an optional high-level residency seam after the
  full skeleton exists but before dependencies resolve. The default low-level
  v4 API remains privately owned and unpublished. `FLoadedAssetPackage::Release`
  transfers a fully qualified graph only to its friend `FAssetManager`.
- Transaction boundary: in-flight mixed skeletons enter `LoadedPackages` and
  `LoadingPackages` so cycles resolve to the same objects. Nested successful
  loads queue `TransactionRegistryEntries`; only the outermost success publishes
  registry/reference projections. Any root failure removes every introduced
  package, discards queued registry entries, restores the initial report, and
  leaves pre-existing residency, registry, dirty state, and compatibility-risk
  ownership unchanged.
- Qualification: paired v3/v4 ordinary loading preserves the reflected soft
  value; a v3-to-v4-to-v3 hard-reference cycle resolves with object identity in
  both directions; a failure after the v4 dependency commits restores residency
  and registry exactly; malformed v4 failure also leaves the same state. V4
  incompatible data is reported, remains clean, and blocks ordinary save both
  with and without a caller-supplied report. Existing v4 phase injection tests
  continue to cover skeleton, dependency, value, ledger, PostLoad, publication,
  retained-closure, and custom-version boundaries.
- Open questions: none for Stage 3. Explicit migration must use the existing
  bundle journal and v4 writer without changing `OrdinaryAssetPackageWriterVersion`.
- Validation: focused mixed live-load and all low-level v4 reader tests passed;
  `AssetPackageTests` passed all 124 tests.

### Stage 3: Add explicit atomic v3-to-v4 migration

- [x] Select v4 as migration output without changing ordinary save output.
- [x] Plan, revalidate, stage, publish, verify, and compensate selected bundles
  through the shared journal and data-loss gates.
- [x] Prove deterministic resave, dependency closure, stale-input rejection,
  retained-unknown preservation, and complete rollback.

#### Acceptance Gate

- An explicitly selected bundle migrates atomically to byte-deterministic v4;
  dry-run and failure paths leave authored files and runtime state unchanged.

#### Stage 3 Handoff

- Baseline: `009a4706` (`feat(asset): activate mixed-version live loading`).
- Working set: `AssetMigration.h/.cpp`, `AssetSystem.h/.cpp`, package migration
  tests, and this plan. No authored `.dasset` files changed.
- Key symbols and decisions: `RegisterBuiltInAssetMigrations` owns the lossless
  `durin.package.3-to-4` edge; `FAssetMigrationPackagePlan::TargetFormatVersion`
  and dependency closure use `AssetPackageMigrationWriterVersion`. Apply writes
  deterministic v4 with `DastV4::WriteAssetPackage` in `NoDelta` mode while
  ordinary save and `SerializeAssetPackageBytes` remain v3.
- Transaction boundary: the existing migration sidecar journal stages and
  compensates the complete selected bundle. Every input is fingerprint-checked
  before load and again before publication; bounded v4 decode, post-audit, and
  batch registry projection complete before sidecar cleanup. Registry projection
  is a validated no-fail commit step owned by
  `FAssetManager::PublishMigratedPackageRegistryEntries`.
- Data-loss and retained-data decision: compatibility findings and live-load risk
  items remain fail-closed, so migration never drops retained unknown data. Exact
  v4 retained closure/payload behavior remains covered by the v4 writer/reader
  tests in the same target.
- Open questions: none for Stage 3. Stage 4 must run the full qualification matrix
  and move lasting migration/version-policy contracts into Runtime docs.
- Validation: `AssetPackageTests` passed 125/125, including dry-run, dependency
  closure, stale-input rejection, injected mid-publication rollback, deterministic
  v4 resave, ordinary-v3 output isolation, registry/residency rollback, and
  retained-unknown codec coverage.

### Stage 4: Qualify and hand off to rollout

- [x] Run focused codec, package, compatibility, registry, migration, asset
  baseline, documentation, hash, and full-build validation.
- [x] Move lasting version-policy and migration contracts into Runtime docs.
- [x] Complete this plan and activate only the qualification/rollout child.

#### Acceptance Gate

- Mixed readers, v4 migration output, cache/registry policy, rollback, hashes,
  asset baseline, and full build pass without modifying tracked content.

#### Stage 4 Handoff

- Baseline: `94e3e47e` (`feat(asset): add atomic v4 package migration`).
- Working set: compatibility-report serialization and DevTool baseline tests;
  Asset package/version Runtime docs; this plan, the serialization roadmap, and
  the activated rollout child. No tracked `.dasset` changed.
- Key symbols and decisions: audit report v2 exposes `formatVersion` while v1
  remains unchanged;
  `DevTool asset baseline` consumes the read-only audit report and remains an
  independent clean-v3 repository gate instead of invoking the v4 migration
  planner. Supported readers, ordinary writer, and migration writer remain
  separate policies.
- Open questions: none. Tracked-content changes, editor render/save/restart,
  ordinary-v4 activation, and v3 retirement belong only to the rollout child.
- Validation: `AssetPackageTests` passed 125/125; DurinDevTool tests passed
  299/299; read-only audit and baseline passed all 17 tracked v3 packages; all
  17 Git object hashes matched `HEAD`; all-plan documentation validation and the
  full `all` build passed.

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

- [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Reader.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetMigration.h`
