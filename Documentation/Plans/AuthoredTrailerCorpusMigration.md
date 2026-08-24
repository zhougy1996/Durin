# Authored Trailer Corpus Migration Plan

Summary: Classify the authored package corpus and qualify the DAST v5 default writer with reversible migration.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

Activated after the VolumeTexture pilot qualified DAST v5 across authored,
derived, cooked, corruption, and rollback lifecycles. DAST v4 remains the
ordinary writer until this plan classifies the repository corpus, freezes the
old-reader and rollback policy, and proves that changing the default cannot
strand required DABK generations. Current-machine performance variance is
diagnostic; durability, compatibility, submit closure, and exact byte
disposition remain hard gates.

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
  relocation/deletion integration, and VolumeTexture lifecycle evidence exist.
- Corpus-wide classification, stable migration reports, submit-closure checks,
  and the ordinary-writer switch are not yet implemented.

## Implementation Stages

### Stage 0: Freeze corpus and compatibility policy

- [ ] Inventory tracked package versions, domains, external descriptor counts,
  required DABK generations, orphan candidates, and Git/LFS ownership.
- [ ] Freeze migration report schema, supported selections, stale-plan checks,
  old-reader diagnostics, rollback artifacts, and cleanup disposition.

#### Acceptance Gate

- Every tracked package and companion is classified without checkout mutation,
  and every migration/rollback state has one explicit owner.

### Stage 1: Implement deterministic migration planning

- [ ] Add construct-free plan/report APIs and a bounded command entry that
  defaults to dry-run and accepts explicit packages.
- [ ] Reject missing/corrupt companions, v5 descriptor/trailer disagreement,
  duplicates, unsupported versions, stale fingerprints, and broad implicit
  selections before publication.
- [ ] Cover canonical ordering and report golden/round-trip validation.

#### Acceptance Gate

- Repeating a dry-run on unchanged input yields identical decisions and bytes,
  with no object construction or filesystem mutation.

### Stage 2: Qualify apply, rollback, and submit closure

- [ ] Apply selected migrations transactionally and verify package, trailer,
  descriptor, DABK, catalog, relocation, deletion, and orphan state.
- [ ] Inject failures at companion, package, root, catalog, verification, and
  cleanup boundaries; prove the prior closure remains loadable.
- [ ] Generate and verify canonical v4 rollback plus exact `.dasset` Git and
  `.dabulk` LFS submit sets.

#### Acceptance Gate

- Every success and failure ends in one verified loadable closure and an exact,
  reviewable Git/LFS disposition.

### Stage 3: Select and activate the ordinary writer

- [ ] Run the representative corpus qualification and record migration and
  rollback evidence without treating diagnostic performance as a hard gate.
- [ ] Change the ordinary writer only if compatibility, durability, and closure
  gates pass; otherwise record a measurable blocker and retain v4.
- [ ] Update lasting package, lifecycle, source-control, roadmap, and
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

## Related Documentation

- [Authored Package Storage Evolution](../Roadmaps/AuthoredPackageStorageEvolution.md)
- [VolumeTexture Trailer Migration](VolumeTextureTrailerMigration.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/CanonicalResave.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageAuthoring.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageAuthoring.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
