# Authored Trailer Corpus Migration Plan

Summary: Classify the authored package corpus and qualify the DAST v5 default writer with reversible migration.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Completed. The initial inventory classified 32 tracked packages. Headless apply
now waits for per-package asynchronous source recovery, rejects domain assets
that remain unready, and registers the Engine third-party runtime directory so
cold StaticMesh DDC recovery can load Assimp. The six Engine content packages
that were initially retained passed this gate and were migrated. A scoped
`DImportRecord` serialization version migrated the historical
`Bytes:Array<uint8>` representation to the current `Bytes:Blob` field, and both
records were resaved. After confirming they are the complete supported
ImportRecord corpus, the one-time deprecated field and migration hook were
removed. The two redirectors were transactionally fixed up and
deleted. The remaining corpus contains 30 packages, all at v5. Both external
VolumeTexture payloads are reachable in new generation-named DABK companions; the two
superseded generations are deleted, and post-migration inspection finds zero
missing payloads and zero orphans. The submit closure is 30 ordinary-Git
`.dasset` rewrites, two redirector `.dasset` deletions, two LFS `.dabulk`
additions, and two LFS generation deletions. A project v4 rollback dry-run
reports exactly 30 ready units. The
ordinary writer is now v5; v4/v5 reads and explicit v4 rollback remain. Package
tests passed 108/108, AssetImportCore tests passed 62/62, Texture tests passed
86/86, StaticMesh tests passed 73/73, and EditorRendering tests passed 77/77.
No-project Material/Spline tests still
expose their separate cold Engine-DDC asynchronous-consumer race, while
EditorAssetWorkflow has an unrelated unavailable persisted image-translator
fixture. Current-machine timing remains diagnostic as directed.

## Goal

Classify every tracked authored package, provide a deterministic dry-run and
bounded migration workflow, and select the repository-wide ordinary writer
only after reversible corpus and compatibility evidence passes.

## Scope

- Inventory tracked `.dasset` packages and reachable/orphaned `.dabulk`
  generations without constructing objects or mutating the checkout.
- Add deterministic migration planning/reporting, explicit package selection,
  failure-atomic application, post-apply verification, and v4 rollback output.
- Freeze old-reader diagnostics, branch/submit closure, cleanup ownership, and
  the default-writer decision.
- Migrate tracked fixtures or content only when their exact Git/LFS closure can
  be isolated and reviewed; automated tests use temporary corpora.

## Non-Goals

- Persistent virtualization, content-addressed backends, DDC authority, or
  remote hydration.
- In-place trailer append/compaction, package-local bulk, compression, or
  deduplication.
- Removing v4 reads, removing DABK v1, or retiring legacy writes.
- Treating current-machine performance measurements as reference telemetry.

## Design Decisions and Invariants

- Migration is plan-then-apply. The plan freezes fingerprints, source/target
  versions, required companion closure, and disposition before any write.
- Application uses the existing complete-package and companion-first
  transaction; a stale plan, unsupported package, missing companion, or any
  injected failure changes no cataloged closure.
- V5 remains a v4 logical object stream plus mandatory EOF trailer. Domain
  schemas, payload ids, DDC keys, Cook products, and source-control attributes
  do not change.
- Rollback to canonical v4 is available for every migrated package before v5
  becomes ordinary. Cleanup is explicit and never part of inspection.
- Old readers reject v5 as an unsupported version. New readers continue to
  read v4 and v5. The default writer changes only after corpus verification.

## Current Foundations and Gaps

- Reader-complete v5, explicit v4/v5 publication, canonical v4 resave,
  relocation/deletion integration, VolumeTexture lifecycle evidence, target-aware
  deterministic migration reports, and the v5 ordinary writer are complete.
- Headless apply waits for pending DDC/source recovery and rejects incomplete
  domain readiness before serialization. The complete ImportRecord corpus now
  uses the current Blob field and its one-time byte-array compatibility route
  has been retired; project redirectors were fixed up and deleted. Persistent
  virtualization and package-format legacy retirement retain
  their independent evidence gates.

## Implementation Stages

### Stage 0: Freeze corpus and compatibility policy

- [x] Inventory tracked package versions, domains, external descriptor counts,
  required DABK generations, orphan candidates, and Git/LFS ownership.
- [x] Freeze migration report schema, supported selections, stale-plan checks,
  old-reader diagnostics, rollback artifacts, and cleanup disposition.

#### Acceptance Gate

- Every tracked package and companion is classified without checkout mutation,
  and every migration/rollback state has one explicit owner.

### Stage 1: Implement deterministic migration planning

- [x] Add construct-free plan/report APIs and a bounded command entry that
  defaults to dry-run and accepts explicit packages.
- [x] Reject missing/corrupt companions, v5 descriptor/trailer disagreement,
  duplicates, unsupported versions, stale fingerprints, and broad implicit
  selections before publication.
- [x] Cover canonical ordering and report golden/round-trip validation.

#### Acceptance Gate

- Repeating a dry-run on unchanged input yields identical decisions and bytes,
  with no object construction or filesystem mutation.

### Stage 2: Qualify apply, rollback, and submit closure

- [x] Apply selected migrations transactionally and verify package, trailer,
  descriptor, DABK, catalog, relocation, deletion, and orphan state.
- [x] Inject failures at companion, package, root, catalog, verification, and
  cleanup boundaries; prove the prior closure remains loadable.
- [x] Generate and verify canonical v4 rollback plus exact `.dasset` Git and
  `.dabulk` LFS submit sets.

#### Acceptance Gate

- Every success and failure ends in one verified loadable closure and an exact,
  reviewable Git/LFS disposition.

### Stage 3: Select and activate the ordinary writer

- [x] Run the representative corpus qualification and record migration and
  rollback evidence without treating diagnostic performance as a hard gate.
- [x] Change the ordinary writer only if compatibility, durability, and closure
  gates pass; otherwise record a measurable blocker and retain v4.
- [x] Update lasting package, lifecycle, source-control, roadmap, and
  compatibility documentation, then disposition Milestones 5-7 by their gates.

#### Acceptance Gate

- The selected default is explicit, fully reversible, and leaves no tracked
  package or required companion unclassified.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Inventory | Every tracked `.dasset` and `.dabulk` classified construct-free |
| Planning | Deterministic report, bounded explicit selection, stale-plan rejection |
| Apply | Atomic v4-to-v5 publication and post-apply trailer/descriptor verification |
| Failure | Prior closure preserved at every injected publication boundary |
| Compatibility | New reader v4/v5, old reader explicit v5 rejection, canonical v4 rollback |
| Source control | Exact `.dasset` Git and `.dabulk` LFS submit/cleanup sets |
| Default | Ordinary writer decision backed by compatibility and durability gates |
| Repository | Package, bulk, operation, content, and documentation validation |

## Definition of Done

- The complete tracked corpus is classified and every selected migration has a
  verified rollback and submit closure.
- The ordinary writer is deliberately set to v5 or deliberately retained at
  v4 with an evidence-backed blocker; no implicit mixed policy remains.
- Milestone 4 is complete and evidence-gated later milestones are activated or
  dispositioned without weakening their entry gates.

## Deferred Follow-ups

- Persistent authored virtualization and backend optimization remain
  evidence-gated roadmap work.
- Legacy DABK write retirement remains conditional on corpus and branch policy.
- Replace the live-object format-only migration path with a construct-free
  v4-to-v5 transcode, and design layered project/Engine DDC lookup separately.
- Qualify stable submitted `Asset.dabulk` names with hidden transaction
  generations and crash recovery before replacing hash-named companions.

## Related Documentation

- [Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
- [VolumeTexture Trailer Migration](VolumeTextureTrailerMigration.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/CanonicalResave.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageAuthoring.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageAuthoring.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
