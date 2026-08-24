# Authored Package Trailer Qualification Plan

Summary: Measure authored payload costs and select, defer, or retain a compatible package-trailer boundary and publication strategy.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

This plan is the active Milestone 0 child of
[Authored Package Storage Evolution](../Roadmaps/AuthoredPackageStorageEvolution.md).
No production wire, writer default, or source-control policy has been selected.
The qualified DAST v4/DABK v1 route remains authoritative throughout this plan.

The initial 2026-08-24 snapshot contains 32 tracked `.dasset` files totalling
71,797 bytes and two tracked `.dabulk` files totalling 2,359,616 bytes.
`.dasset` is ordinary Git binary data; `.dabulk` is Git LFS data. These numbers
are routing evidence only. Stage 0 freezes representative workloads, metrics,
and decision thresholds before the qualification treats any result as an
acceptance claim.

## Goal

Produce a reproducible Proceed, Defer, or Retain decision for authored package
storage. A Proceed decision selects one package/object-stream boundary, one
initial local placement, explicit compatibility and rollback behavior, and the
next child-plan boundary. Defer or Retain records measurable revisit triggers
without introducing production format code.

## Scope

- Measure current and representative authored payload counts, sizes, change
  rates, duplicate content, orphan generations, save cost, checkout/transfer
  cost, and source-control growth.
- Compare the current DAST v4/DABK v1 baseline with a new DAST package version,
  a versioned outer envelope, and metadata-trailer variants that retain
  LFS-backed companion placement.
- Freeze the logical payload-id lifecycle, content/integrity-key semantics,
  placement vocabulary, and compatibility matrix needed by a selected wire.
- Model full-file replacement, companion-first publication, tail update, append
  generation, recovery, compaction, and catalog/source-control failure windows
  on supported filesystems.
- Record exact Proceed gates or Defer/Retain revisit triggers and update the
  roadmap handoff.

## Non-Goals

- Implementing a production trailer codec, reader, writer, or default placement.
- Mutating, resaving, or converting the tracked asset corpus.
- Changing `FEditorBulkData`, domain payload schemas, DDC, Cook, DBLK, decoded
  CPU data, or runtime resource contracts.
- Changing `.gitattributes`, migrating `.dasset` into LFS, or rewriting Git
  history as part of qualification.
- Selecting or implementing a persistent virtualization backend, hydration
  cache, permissions model, retention service, or garbage collector.
- Treating UE terminology, wire bytes, Perforce integration, or backend graphs
  as Durin requirements.

## Design Decisions and Invariants

- Qualification is outcome-neutral. Retaining DABK is a successful result when
  no candidate passes the frozen benefit and durability gates.
- Raw measurement output lives under ignored
  `Saved/AuthoredPackageStorageQualification/`. Reproducible commands,
  environment facts, summarized results, thresholds, and the decision remain in
  this plan; machine-specific absolute paths do not.
- DAST v4 is an exact five-section file whose final section ends at physical
  EOF. Appending bare trailer bytes is not a compatible DAST v4 candidate.
- Wire boundary and payload placement are evaluated separately:

  | Axis | Candidates that enter qualification |
  | --- | --- |
  | Package boundary | Retain DAST v4; introduce a new DAST version; wrap an exact bounded DAST logical stream in a versioned outer envelope |
  | Initial local placement | Retain DABK v1; use a versioned companion indexed by package metadata/trailer; store bytes package-locally only with an explicit ordinary-Git/LFS policy |

- A new reader may support both old and new packages. No candidate may describe
  that as an old DAST v4 reader accepting new trailing bytes.
- `PayloadId` is logical identity, not a content key. Qualification must resolve
  its uniqueness scope and its copy, duplication, reimport, and move behavior;
  current copies retain the GUID while same-package duplicate GUIDs fail save.
- The current XXH3-128 value is an integrity check. A persistent content key
  requires an algorithm/version and collision response; qualification does not
  silently promote the current hash into a durable global namespace.
- Equal hashes are only duplicate candidates. Deduplication evidence requires
  exact byte equality before two authored payloads are counted as identical.
- Local, companion, referenced, and virtualized names are not accepted wire
  states until their exact authority, address, availability, and inspection
  semantics are selected. Reserved values remain unsupported.
- Correctness and durability gates are pass/fail. Performance or storage gains
  cannot compensate for a lost last-published generation, ambiguous authority,
  or an unavailable submit closure.
- Complete-file atomic replacement and current companion-first publication are
  the recovery baselines. Tail or append approaches must beat a frozen budget
  and prove supported-filesystem fault behavior; they are not presumed goals.
- A never-hydrated virtual payload has no transparent offline guarantee.
  Prefetch, durable local fallback, or an explicit offline failure is required
  by any future virtualization plan.

## Current Foundations and Gaps

| Area | Foundation | Qualification gap |
| --- | --- | --- |
| Corpus | 32 tracked `.dasset` files total 71,797 bytes; two DABK companions total 2,359,616 bytes | The sample is too small and narrow to represent future dense consumers by itself |
| DAST | V4 has canonical five-section bounds, construct-free read/inspection, complete trailing-byte rejection, and a 256 MiB package ceiling | No selected new version or outer-envelope boundary exists |
| DABK | V1 companions are immutable, generation-named, bounded, canonical, LFS-backed, inspectable, and published before their package | Change-rate, orphan-amplification, checkout, history, and latency costs are not measured |
| Identity | `FEditorBulkData` owns a GUID, logical size, XXH3-128 hash, and verified resident bytes | Copy/regeneration scope and a durable content-key collision policy are undefined |
| Publication | Atomic single-file replacement and staged package/companion publication preserve the previous reachable pair | No tail/append/compaction protocol is qualified on supported filesystems |
| Source control | `.dasset` uses ordinary Git and `.dabulk` uses Git LFS | Package-local bytes can invert the documented storage policy and must be measured separately |
| Tests | `AssetBulkContainerTests` and `AssetPackageTests` cover current container and package behavior | No selected new wire exists for golden, corruption, or migration fixtures |

## Implementation Stages

### Stage 0: Freeze the qualification protocol

- [ ] Freeze three workload sets: the complete tracked corpus, deterministic
  synthetic size/change distributions, and named future-consumer scenarios
  backed by an owning contract rather than speculation.
- [ ] Define every measured metric with unit, workload, collection method,
  repeat count, environment facts, and an acceptance or revisit threshold
  before collecting decision-bearing results.
- [ ] Record the supported filesystem, Git, Git LFS, build preset, CPU/storage
  environment, cold/warm-cache definition, and interference policy used for
  timing claims.
- [ ] Freeze the candidate matrix across package boundary, local placement,
  publication protocol, source-control classification, and rollback route;
  reject combinations that cannot preserve a last complete generation.
- [ ] Define the ignored raw-report layout under
  `Saved/AuthoredPackageStorageQualification/` and the stable summary tables to
  be maintained in this plan.

#### Acceptance Gate

- Every metric has a unit, workload, command or API, repeat count, and frozen
  threshold; results have not been used to choose those thresholds.
- Tracked, synthetic, and future-consumer workloads are distinguishable and
  reproducible, and synthetic scale is never reported as current corpus fact.
- Every candidate has an explicit legacy-read, new-read, rollback,
  source-control, and last-good-generation hypothesis.

### Stage 1: Measure corpus and source-control costs

- [ ] Inspect every tracked `.dasset` construct-free, enumerate every authored
  bulk descriptor and reachable DABK, and report corrupt, missing, duplicate-id,
  duplicate-content, stale-generation, and orphan states without mutation.
- [ ] Measure payload counts, logical/stored-size distributions, inline/external
  split, per-package fan-out, and deduplication upper bounds for tracked and
  synthetic workloads.
- [ ] Measure Git history growth, working-tree bytes, incremental checkout and
  transfer bytes, partial-sync behavior, and rename/edit amplification for the
  ordinary-Git `.dasset` and LFS-backed `.dabulk` baseline.
- [ ] In an isolated throwaway repository, compare metadata-only and payload
  edits for companion-local and package-local layouts without changing the
  repository `.gitattributes` or rewriting project history.
- [ ] Measure current save/publication latency, bytes written, peak temporary
  disk, construct-free inspection bytes, and orphan cleanup amplification under
  the frozen cold/warm and repeat policy.

#### Acceptance Gate

- The report reproduces the tracked 32-package/two-companion snapshot or
  explains a reviewed corpus change.
- Every number identifies its corpus, units, environment, sample count, and
  collection method; raw evidence is retained in the ignored report directory.
- Git main-object growth and LFS object/transfer growth are reported separately.

### Stage 2: Select identity, wire, and compatibility boundaries

- [ ] Record byte-level layouts for each surviving boundary candidate, including
  version discovery, inner DAST extent, header/table/data/footer ownership,
  alignment, counts, sizes, hashes, canonical ordering, and trailing-byte rules.
- [ ] Decide the payload-id uniqueness scope and exact regeneration/aliasing
  behavior for object duplication, package copy, cross-package move, reimport,
  and repeated authored edits.
- [ ] Decide logical-byte versus stored-byte hashes, content-key
  algorithm/version representation, collision handling, compression ownership,
  and the rule for unsupported reserved codecs.
- [ ] Define only the initial placement states required by the selected local
  route and reject unknown required states without reserving behavior in code.
- [ ] Complete a reader/writer matrix for legacy DAST v4/DABK v1 and every
  surviving candidate, including construct-free inspection, canonical resave,
  explicit old-reader rejection, downgrade, rollback, and orphan disposition.
- [ ] Eliminate any candidate that requires changing a domain schema, makes DDC
  authoritative, hides a physical fact in `FEditorBulkData`, or cannot keep
  catalog and dirty/DDC fingerprints authority-correct.

#### Acceptance Gate

- One boundary and initial placement are selected for Proceed, or the evidence
  records Defer/Retain with no unresolved pseudo-selection.
- The selected identity and hash rules are unambiguous for every lifecycle
  operation and never use a non-versioned integrity hash as a durable backend
  key.
- The compatibility matrix states the result of every reader/wire pair and
  names the exact bytes required for rollback.

### Stage 3: Qualify publication and failure models

- [ ] Model candidate construction, flush, close, package/companion replacement,
  footer visibility, catalog publication, source-control submit, cleanup, and
  bundle ordering as explicit state transitions.
- [ ] Inject or simulate termination, short write, flush/close failure, rename
  failure, stale footer, corrupt latest generation, interrupted compaction,
  insufficient disk, catalog failure, and partial submit at every reachable
  transition.
- [ ] Prove the last complete generation remains discoverable, distinguish an
  incomplete candidate from committed corruption, and bound temporary plus
  stale-generation disk use.
- [ ] Compare complete-file replacement, companion-first publication, tail
  rewrite, and append generation against the frozen save-latency, rewrite-byte,
  source-control, and compaction budgets on each supported filesystem.
- [ ] Define the exact offline outcome for every selected local placement and
  leave remote hydration, backend permissions, retention, and GC to the
  virtualization child plan.

#### Acceptance Gate

- Every Proceed protocol preserves the previous committed package/payload
  closure under all injected failures and has bounded cleanup/compaction.
- A tail or append protocol is selected only if it beats a frozen budget; full
  replacement remains the recorded fallback.
- Source-control submission cannot publish a package reference before its
  required companion or persistent payload is durable.

### Stage 4: Record the decision and handoff

- [ ] Publish a candidate scorecard with pass/fail durability and compatibility
  gates plus measured storage, source-control, and performance deltas.
- [ ] Record exactly one Proceed, Defer, or Retain result, its rationale,
  rejected alternatives, confidence limits, and quantitative revisit triggers.
- [ ] Update the roadmap Milestone 0 state and current status. Create and link
  the Package Trailer Foundation plan only for Proceed and only with the
  selected boundary and placement vocabulary.
- [ ] Preserve the current DAST v4/DABK v1 contracts as authoritative until a
  production child plan passes its migration gate; do not write future wire
  decisions into implemented contract documentation prematurely.
- [ ] Run repository documentation validation and record any native evidence
  selected below using the root agent workflows.

#### Acceptance Gate

- The roadmap, this plan, the decision scorecard, and any activated next plan
  agree on status, dependencies, compatibility, budgets, and deferred scope.
- Defer/Retain activates no production format plan and records executable
  revisit triggers; Proceed provides a bounded next plan with no unresolved
  wire or placement choice.
- Documentation validation passes and all claimed native/source-control
  evidence is reproducible from recorded inputs.

## Validation Matrix

| Area | Required qualification evidence |
| --- | --- |
| Documentation | Changed-document validation throughout; all-plan and all-roadmap lifecycle validation when status or plan links change |
| Corpus | Read-only construct-free inventory of every tracked package and companion, with explicit corrupt/missing/orphan/duplicate classification |
| Wire | Byte-layout record, exact bounds and canonical rules, footer discovery, trailing-byte behavior, and unsupported-state policy for every surviving candidate |
| Identity | Payload-id lifecycle matrix and algorithm-versioned logical/stored hash plus collision policy |
| Compatibility | New-reader legacy route, selected new read/write, explicit old-reader result, canonical resave, downgrade/rollback bytes, and domain-schema stability |
| Publication | State-transition and failure-injection matrix covering full-file, companion-first, tail, append, catalog, bundle, cleanup, and submit boundaries |
| Source control | Isolated ordinary-Git versus LFS measurements for metadata and payload edits; no main-repository attribute or history mutation |
| Performance | Frozen workloads and thresholds for save latency, bytes rewritten, checkout/transfer, inspection IO, peak temporary disk, and compaction amplification |
| Native baseline | `AssetBulkContainerTests` for bounded container mechanics and `AssetPackageTests` for DAST, authored companion, inspection, publication, and recovery behavior, selected through [Agent Testing Workflow](../Agents/Testing.md) |
| Build | Any executable evidence follows [Agent Build and Run Workflow](../Agents/BuildAndRun.md); a document-only stage does not invent a native build claim |

## Definition of Done

- One Proceed, Defer, or Retain decision passes every Stage 4 acceptance item.
- Measurements distinguish current corpus facts from synthetic and projected
  workloads and retain reproducible ignored raw evidence.
- Payload identity, content-key, placement, compatibility, publication,
  recovery, source-control, and offline semantics have no unresolved branch in
  the selected result.
- No production wire, default writer, tracked asset, `.gitattributes`, or Git
  history changed during qualification.
- The roadmap reflects the decision and links only child plans whose activation
  gates passed.

## Deferred Follow-ups

- Trailer/footer codec, builder/reader, golden bytes, and corruption fixtures.
- Dual-read production loading and failure-atomic selected-local publication.
- VolumeTexture migration, corpus conversion, and default-writer selection.
- Persistent virtualization, hydration, offline prefetch, permissions,
  retention, disaster recovery, and garbage collection.
- Compression, cross-package deduplication, range IO, package aggregation, and
  legacy DABK write retirement.

## Related Documentation

- [Authored Package Storage Evolution](../Roadmaps/AuthoredPackageStorageEvolution.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File IO](../Runtime/Core/FileIO.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Domain-Owned Large Asset Payload Architecture](../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorageTypes.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/FileHelper.h`
- `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkContainerInfrastructureTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4WireContractTests.cpp`
