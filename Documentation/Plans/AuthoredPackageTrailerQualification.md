# Authored Package Trailer Qualification Plan

Summary: Measure authored payload costs and select, defer, or retain a compatible package-trailer boundary and publication strategy.

Last reviewed: 2026-08-24

Status: Completed
Completed: 2026-08-24

## Current Status

Milestone 0 completed with a **Retain** decision. No new package boundary,
placement vocabulary, production writer, or source-control migration is
selected. DAST v4/DABK v1 remains authoritative, including companion-first
publication and atomic package replacement.

The reproducible Windows Debug run inspected all 32 tracked `.dasset` files and
two reachable `.dabulk` companions without mutation. It found two external
payload descriptors totalling 2,359,296 logical bytes, no corrupt or missing
payload, no orphan companion, and no exact duplicate content. The same logical
payload GUID occurs in the two different packages; this is valid under the
qualified package-scoped uniqueness rule and the contents differ.

Storage and current-corpus pressure remained below their frozen gates. The
current computer is not the performance reference machine, so its 69.965 ms
construct-free Debug warm p95 and publication timings are retained as diagnostic
evidence only and do not select or reopen a wire. No new boundary has measured
benefit sufficient to justify compatibility and migration cost. Raw evidence is
ignored under
`Saved/AuthoredPackageStorageQualification/2026-08-24-win64-debug-retain/`.

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

## Frozen Qualification Protocol

The checked-in protocol is
`Tools/DurinDevTool/data/authored-package-storage-qualification-v1.json`. Run it
from the repository root with:

```powershell
.\DevTool.bat asset qualify-storage --project Sandbox\Sandbox.dproject --output Saved\AuthoredPackageStorageQualification\2026-08-24-win64-debug-retain
```

The report directory contains `native-inventory.json`, `git-baseline.json`,
`source-control-experiment.json`, `synthetic-model.json`,
`publication-benchmark.json`, and the aggregate `qualification-report.json`.
The isolated Git repositories remain below the run's `_scratch/` directory so
their objects and LFS pointers can be audited without touching project history.

| Workload | Classification | Reproduction and ownership |
| --- | --- | --- |
| Every tracked package and companion | Current corpus | Mounted Engine and Sandbox package snapshot plus Git-tracked `.dasset`/`.dabulk` enumeration |
| `synthetic-mixed-v1` | Synthetic only | Seed 841592647; 128 packages; repeated 0 B through 64 MiB distribution; 0%, 1%, 25%, and 100% edit fractions |
| VolumeTexture source | Named future consumer | Atomic tightly packed voxel payload owned by [Volume Textures](../Runtime/Assets/VolumeTextures.md) |
| Texture source | Named future consumer | Domain-owned Texture2D/TextureCube source payload boundary in [Texture System](../Runtime/Rendering/TextureSystem.md); no projected count is reported as corpus fact |

| Metric | Unit and collection | Samples | Frozen gate or revisit trigger |
| --- | --- | --- | --- |
| Package/payload inventory and fan-out | counts and bytes from construct-free package inspection and verified DABK loads | complete corpus | corrupt, missing, same-package duplicate id, or unreachable required bytes fail correctness |
| Inspection IO and latency | package bytes read; first call cold, following calls warm | 5 inspections per package | diagnostic on this non-reference machine; a reference run uses 50 ms warm p95 |
| Publication latency and rewrite bytes | write, flush, `fsync`, close, and atomic replace of detached copies | 7 per package/operation | diagnostic on this non-reference machine; temporary amplification remains bounded at 2.0x |
| Git history and change rate | unique Git blob bytes, LFS pointer-declared bytes, commit touches/month | complete tracked history | revisit at 2 GiB monthly LFS transfer/rewrite |
| Checkout/partial sync | isolated Git object bytes and LFS payload bytes with skip-smudge projection | baseline, metadata edit, 1% payload edit, rename | candidate must reduce a measured cost by at least 25% without changing an unapproved Git/LFS boundary |
| Corpus pressure | reachable external bytes and authored payload count | complete corpus | revisit at 256 MiB or 100 payloads |
| Consumer pressure | distinct dense authored consumers | owning contracts | revisit at two consumers with measured shared need |
| Failure model | state/failure pair with last-good-generation outcome | every reachable transition named below | every correctness/durability case must pass; performance cannot compensate |

The run used Windows 10 build 2009 on x64, NTFS, Git 2.45.2, Git LFS 3.5.1,
the `windows-msvc-x64` profile, `Win64-Debug-DurinEditor` preset, and Debug
configuration. This is not the performance reference computer, so timings are
diagnostic and cannot change the storage decision. A
cold sample is the first inspection in a new native process; warm samples reuse
that process without an OS cache flush. The timing policy requires no editor,
test, build, or storage-heavy competitor.

## Measured Results

| Corpus result | Measurement |
| --- | ---: |
| Tracked DAST packages | 32 files; 71,797 working-tree bytes |
| Tracked DABK companions | 2 files; 2,359,616 working-tree bytes including 320 bytes of container framing |
| Authored payloads | 2 external, 0 inline; 2,359,296 logical/stored bytes |
| Package fan-out | 30 packages with 0 payloads; 2 packages with 1 payload each |
| Integrity/reachability | 0 corrupt packages, 0 missing payloads, 0 stale generations, 0 tracked or discovered orphans |
| Identity/content | one cross-package repeated GUID; 0 same-package duplicate ids; 0 equal-hash candidates; 0 exact duplicates |
| Construct-free IO | 71,797 package bytes per complete pass |
| Construct-free warm timing | diagnostic 5.818 ms median; 69.965 ms p95 on the non-reference Debug machine |
| Publication | diagnostic maximum warm p95 13.302 ms; largest external save wrote 2,098,805 bytes |
| Estimated current rewrite rate | 2,610,827 bytes/month from tracked history touch rates |
| Historical object accounting | 953,867 bytes of unique main-Git blobs by path; 2,359,616 bytes of LFS payload generations by path |

The deterministic synthetic workload contains 128 packages, 112 nonempty
payloads, and 1,431,371,776 logical bytes. Its median payload is 1 MiB and p95
and maximum are 64 MiB. It is scaling evidence only. With immutable generations,
any nonzero edit fraction rewrites the affected full payload; it is not evidence
that the current corpus has this scale or change rate.

The isolated 1 MiB experiment separated main-Git and LFS growth:

| Layout/edit | Main-Git object delta | LFS object delta |
| --- | ---: | ---: |
| Companion-local baseline | 4,096 B | 1,048,576 B |
| Companion-local metadata edit | 4,096 B | 0 B |
| Companion-local 1% payload edit | 1,024 B | 1,048,576 B |
| Package-local ordinary-Git baseline | 1,052,672 B | 0 B |
| Package-local metadata edit | 1,053,696 B | 0 B |
| Package-local 1% payload edit | 1,052,672 B | 0 B |

After the rename, the companion experiment projected a 9,216-byte Git-only
checkout with LFS smudge skipped and 2,106,368 bytes when both LFS generations
were included. The package-local layout required 3,159,040 main-Git object
bytes and had no partial-payload sync boundary. Rename introduced no new payload
object in either layout. Package-local ordinary Git is therefore rejected for
the current source-control policy.

## Identity, Hash, and Placement Qualification

| Lifecycle event | Qualified rule while retaining the baseline |
| --- | --- |
| New logical payload | Generate a nonzero GUID unique within its package |
| Repeated authored edit or reimport | Retain `PayloadId`; replace bytes atomically and update size/integrity values |
| Package copy | Retain payload GUIDs; cross-package equality is valid and is not implicit aliasing or deduplication |
| Object duplicate within one package | Regenerate the duplicated payload GUID before save; same-package duplicates fail save |
| Cross-package move/relocation | Retain logical GUID and bytes while publication moves the full reachable closure |
| Exact content duplicate | Equal XXH3-128 values are candidates only; exact byte comparison is required before reporting identity or deduplication |

`PayloadId` remains logical identity and never addresses a backend. DAST v4 and
DABK v1 store uncompressed logical bytes, so their current XXH3-128 value checks
both logical and stored bytes but remains an integrity check, not a durable
global content key. Retain selects no persistent key. A reopened persistent route
must encode `(algorithm, version, digest)`, compare exact bytes before aliasing,
and reject/quarantine a same-key byte mismatch. Compression remains domain- or
future-container-owned; current DABK accepts no codec and unknown required codec
or placement values fail. Only current `Inline` and `External` placements are
qualified; referenced and virtualized values remain unsupported.

## Boundary and Compatibility Qualification

Only the retained baseline survives to a byte-level contract. DAST v4 begins
with `DAST`, little-endian version 4, a bounded summary length, section count 5,
then exactly five 9-byte Name/Type/Schema/Object/Value entries. Sections are
contiguous, unpadded, canonical, and the Value extent ends at physical EOF.
DABK v1 has a 64-byte header, sorted 96-byte entries, 16-byte payload alignment,
XXH3-128 container/content integrity, unique ids, and no trailing bytes. The
implemented contract remains authoritative in [Asset Packages](../Runtime/Assets/AssetPackages.md).

| Candidate | Legacy reader | New reader/write hypothesis | Rollback bytes | Result |
| --- | --- | --- | --- | --- |
| DAST v4 + DABK v1 | Reads exact v4 and resolves v1 companion | Current canonical reader/writer and construct-free inspection | previous DAST plus every named immutable DABK | Pass; production baseline |
| DAST v5 + indexed companion | Rejects version 5 before body decoding | Dual dispatch by package version; would require exact new header/section layout | canonical v4 plus retained/rebuilt DABK v1 | Deferred before wire freeze: no measured benefit |
| Outer envelope + indexed companion | Rejects envelope magic; never treats appended bytes as v4 | Would bound an exact inner stream and separately validate trailer/footer | exact inner v4 plus every DABK it names | Deferred before wire freeze: no measured benefit |
| Package-local payload | Rejects new boundary | Would require bounded payload region and explicit `.dasset` Git/LFS policy | v4 plus reconstructed DABK | Rejected under current ordinary-Git policy and amplification evidence |
| Bare bytes after DAST v4 EOF | Rejects trailing bytes | Cannot be described as compatible v4 | none | Rejected as structurally incompatible |
| Tail rewrite / append generation | Rejects new boundary | Requires proven footer discovery, generation commit, and compaction | previous discoverable generation | Rejected: durability/fault gate failed |

No candidate changes a domain schema, makes DDC authoritative, hides a physical
fact in `FEditorBulkData`, or changes catalog/dirty/DDC authority. Because the
decision is Retain, no new writer/reader pair, downgrade codec, trailer golden,
or production placement state is activated.

## Publication and Failure Qualification

| Protocol | Commit model and injected failures | Last-good result |
| --- | --- | --- |
| Complete-file replacement | construct, flush, close, replace, catalog, cleanup; termination, short write, flush/close/rename, disk, and catalog failures | Pass: replacement is the only file commit point; catalog retains/reconciles its prior revision |
| Companion-first | construct/flush/close/publish companion, construct/flush/close/publish package, catalog, submit closure, cleanup; corrupt candidate and partial submit added | Pass: unpublished companion is orphanable and the old package continues naming the old immutable companion; submit must include the full closure |
| In-place tail rewrite | write/flush/publish footer; termination, short write, stale footer, disk failure | Fail: interruption can destroy the only committed footer on NTFS |
| Append generation | append/flush data and footer, corrupt latest, interrupted compaction, disk failure | Fail: no bounded redundant-footer discovery or atomic footer guarantee is qualified |

Temporary disk for accepted protocols is bounded by one detached candidate
closure (at most 1.0x additional live bytes in the measured model), below the
2.0x gate. Full replacement remains the fallback. Fully local and companion
placements remain offline-capable when their submit closure is present; no
never-hydrated virtual state was selected.

## Decision Scorecard

| Candidate | Compatibility | Durability | Source control | Measured benefit | Decision |
| --- | --- | --- | --- | --- | --- |
| Retain DAST v4/DABK v1 | Pass | Pass | Pass | Baseline | Retain |
| DAST v5 indexed companion | Hypothesis only | Full replacement could pass | Preserves Git/LFS split | Not measured | Reject for current corpus |
| Outer envelope indexed companion | Hypothesis only | Full replacement could pass | Preserves Git/LFS split | Not measured | Reject for current corpus |
| Package-local ordinary Git | Requires new reader | Full replacement could pass | Fail | Negative metadata/history amplification | Reject |
| Tail/append | Requires new reader | Fail | Unselected | No accepted gain | Reject |

The exact decision is **Retain** with high confidence for storage and durability:
corpus integrity, source-control amplification, and publication rewrite bytes
are direct. Current-machine performance data is diagnostic only. Revisit when
any one of the following occurs:

- the exact construct-free workload remains above 50 ms warm p95 in two
  consecutive quiet Release runs on the designated reference machine;
- reachable external authored bytes reach 256 MiB;
- the tracked authored payload count reaches 100;
- measured monthly LFS transfer/rewrite reaches 2 GiB;
- publication warm p95 reaches 250 ms; or
- two distinct dense authored consumers demonstrate the same storage need.

Revisit does not imply Proceed. Thresholds reopen qualification; a production
route still needs a measured 25% benefit plus all compatibility and durability
gates. No Package Trailer Foundation plan is created by this result.

## Implementation Stages

### Stage 0: Freeze the qualification protocol

- [x] Freeze three workload sets: the complete tracked corpus, deterministic
  synthetic size/change distributions, and named future-consumer scenarios
  backed by an owning contract rather than speculation.
- [x] Define every measured metric with unit, workload, collection method,
  repeat count, environment facts, and an acceptance or revisit threshold
  before collecting decision-bearing results.
- [x] Record the supported filesystem, Git, Git LFS, build preset, CPU/storage
  environment, cold/warm-cache definition, and interference policy used for
  timing claims.
- [x] Freeze the candidate matrix across package boundary, local placement,
  publication protocol, source-control classification, and rollback route;
  reject combinations that cannot preserve a last complete generation.
- [x] Define the ignored raw-report layout under
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

- [x] Inspect every tracked `.dasset` construct-free, enumerate every authored
  bulk descriptor and reachable DABK, and report corrupt, missing, duplicate-id,
  duplicate-content, stale-generation, and orphan states without mutation.
- [x] Measure payload counts, logical/stored-size distributions, inline/external
  split, per-package fan-out, and deduplication upper bounds for tracked and
  synthetic workloads.
- [x] Measure Git history growth, working-tree bytes, incremental checkout and
  transfer bytes, partial-sync behavior, and rename/edit amplification for the
  ordinary-Git `.dasset` and LFS-backed `.dabulk` baseline.
- [x] In an isolated throwaway repository, compare metadata-only and payload
  edits for companion-local and package-local layouts without changing the
  repository `.gitattributes` or rewriting project history.
- [x] Measure current save/publication latency, bytes written, peak temporary
  disk, construct-free inspection bytes, and orphan cleanup amplification under
  the frozen cold/warm and repeat policy.

#### Acceptance Gate

- The report reproduces the tracked 32-package/two-companion snapshot or
  explains a reviewed corpus change.
- Every number identifies its corpus, units, environment, sample count, and
  collection method; raw evidence is retained in the ignored report directory.
- Git main-object growth and LFS object/transfer growth are reported separately.

### Stage 2: Select identity, wire, and compatibility boundaries

- [x] Record byte-level layouts for each surviving boundary candidate, including
  version discovery, inner DAST extent, header/table/data/footer ownership,
  alignment, counts, sizes, hashes, canonical ordering, and trailing-byte rules.
- [x] Decide the payload-id uniqueness scope and exact regeneration/aliasing
  behavior for object duplication, package copy, cross-package move, reimport,
  and repeated authored edits.
- [x] Decide logical-byte versus stored-byte hashes, content-key
  algorithm/version representation, collision handling, compression ownership,
  and the rule for unsupported reserved codecs.
- [x] Define only the initial placement states required by the selected local
  route and reject unknown required states without reserving behavior in code.
- [x] Complete a reader/writer matrix for legacy DAST v4/DABK v1 and every
  surviving candidate, including construct-free inspection, canonical resave,
  explicit old-reader rejection, downgrade, rollback, and orphan disposition.
- [x] Eliminate any candidate that requires changing a domain schema, makes DDC
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

- [x] Model candidate construction, flush, close, package/companion replacement,
  footer visibility, catalog publication, source-control submit, cleanup, and
  bundle ordering as explicit state transitions.
- [x] Inject or simulate termination, short write, flush/close failure, rename
  failure, stale footer, corrupt latest generation, interrupted compaction,
  insufficient disk, catalog failure, and partial submit at every reachable
  transition.
- [x] Prove the last complete generation remains discoverable, distinguish an
  incomplete candidate from committed corruption, and bound temporary plus
  stale-generation disk use.
- [x] Compare complete-file replacement, companion-first publication, tail
  rewrite, and append generation against the frozen save-latency, rewrite-byte,
  source-control, and compaction budgets on each supported filesystem.
- [x] Define the exact offline outcome for every selected local placement and
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

- [x] Publish a candidate scorecard with pass/fail durability and compatibility
  gates plus measured storage, source-control, and performance deltas.
- [x] Record exactly one Proceed, Defer, or Retain result, its rationale,
  rejected alternatives, confidence limits, and quantitative revisit triggers.
- [x] Update the roadmap Milestone 0 state and current status. Create and link
  the Package Trailer Foundation plan only for Proceed and only with the
  selected boundary and placement vocabulary.
- [x] Preserve the current DAST v4/DABK v1 contracts as authoritative until a
  production child plan passes its migration gate; do not write future wire
  decisions into implemented contract documentation prematurely.
- [x] Run repository documentation validation and record any native evidence
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

## Validation Evidence

- `DevTool.bat build --target DurinAssetTool` passed for
  `Win64-Debug-DurinEditor`.
- The recorded `asset qualify-storage` command completed with Retain and wrote
  all raw reports below the ignored run directory without changing tracked
  assets, `.gitattributes`, or project history.
- `AssetBulkContainerTests` passed 11/11 tests.
- `AssetPackageTests` passed 106/106 tests.
- Focused DevTool asset/command-contract tests passed 34/34. The complete
  Windows-applicable DevTool selection passed 375 tests with 2 skipped and 3
  explicitly deselected POSIX/macOS-only assertions. An unfiltered diagnostic
  run confirmed those three host-mismatch failures and no additional failure.
- Changed-document, all-document, all-plan, and all-roadmap validation passed
  after the final evidence update.

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
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Tools/DurinDevTool/durin_dev_tool/storage_qualification.py`
- `Tools/DurinDevTool/data/authored-package-storage-qualification-v1.json`
