# Authored Package Storage Evolution Roadmap

Summary: Evolve authored bulk storage from generation-named DABK companions toward trailer-indexed, content-addressed, and optionally virtualized package payloads without changing domain-owned schemas or weakening durability.

Last reviewed: 2026-08-24

Status: Completed
Completed: 2026-08-24

## Current Status

Durin currently publishes authored bulk bytes through DAST v5 packages as
either inline trailer-indexed values or immutable generation-named DABK v1
companions, while retaining DAST v4 reads and canonical rollback. The package
stores enough physical identity to resolve and verify each external companion, and save,
bundle publication, relocation, deletion, inspection, repair, and canonical
resave treat the package plus all reachable companions as one consistency unit.
This behavior is implemented and is the production baseline.

The completed
[Domain-Owned Large Asset Payload Architecture](LargeAssetPayloadArchitecture.md)
roadmap deliberately deferred authored virtualization and package-container
evolution until corpus and source-control telemetry justified the additional
system. On 2026-08-24 the tracked working corpus contains 32 `.dasset` files
totalling 71,797 bytes and two `.dabulk` files totalling 2,359,616 bytes.
`.dasset` uses ordinary Git while `.dabulk` uses Git LFS. This corpus is too
small to justify immediate replacement, and embedding the current bulk bytes in
`.dasset` would cross an existing source-control boundary rather than merely
change package IO.

This roadmap records the intended evolution before those consumers proliferate.
It uses Unreal Engine's package trailer, editor bulk data, payload identifier,
and Virtual Assets separation as a non-normative reference. It does not copy UE
wire formats, names, workspace-domain implementation, or backend policy.
Implementation plans remain gated by measured need and must preserve the
qualified DAST v4/DABK v1 route until their own migration gates pass.
The first child plan,
[Authored Package Trailer Qualification](../Plans/AuthoredPackageTrailerQualification.md),
completed on 2026-08-24 with a **Retain** decision. The complete tracked corpus
has 32 DAST packages and two reachable external payloads totalling 2,359,296
logical bytes, with no corruption, missing payload, orphan, or exact duplicate.
An isolated 1 MiB metadata edit added 4 KiB of main-Git objects with an
LFS-backed companion but about 1.005 MiB when payload bytes were embedded in an
ordinary-Git package. No new boundary showed measured benefit sufficient to
pay its compatibility and migration cost.

DAST v4/DABK v1, companion-first publication, and the current Git/LFS split
therefore remain authoritative. Current-computer timings are recorded as
diagnostic only because this is not the designated performance reference
machine. The subsequent user directive to generate and execute plans until this
roadmap is complete is recorded separately by
[Authored Package Storage Strategic Activation](../Plans/AuthoredPackageStorageStrategicActivation.md).
It preserves the Retain evidence but authorizes strategic Proceed, selects DAST
v5 with a versioned EOF trailer/footer and external DABK v1 placement, and
activated and completed the
[Authored Package Trailer Foundation](../Plans/AuthoredPackageTrailerFoundation.md).
Its detached v1 trailer/footer codec, EOF inspection, golden wires, and
corruption contracts are qualified without registering production DAST v5.
Milestone 2 completed through
[Selected Local Authored Payload Publication](../Plans/SelectedLocalAuthoredPayloadPublication.md).
DAST v5 became reader-complete and explicitly publishable with external DABK v1,
while v4 remained the ordinary writer and canonical rollback at that gate. Milestone 3
completed through
[VolumeTexture Trailer Migration](../Plans/VolumeTextureTrailerMigration.md):
the explicit VolumeTexture pilot qualified inline/external source, DDC, Cook,
corruption recovery, deletion, and v4 rollback without schema change.
Milestone 4 completed through
[Authored Trailer Corpus Migration](../Plans/AuthoredTrailerCorpusMigration.md).
The ordinary writer is v5, and all 30 remaining tracked packages use v5.
Headless apply now waits for asynchronous
source recovery, validates domain readiness, and exposes Engine third-party
runtime DLLs for cold StaticMesh DDC reconstruction. The two supported
ImportRecords were converted from the historical byte-array payload
representation to the current Blob representation, then the one-time field
compatibility route was retired. The two redirectors were transactionally fixed
up and deleted. Two reachable external companions were
republished as new LFS generations with no missing payload or orphan. Canonical
v4 rollback remains available for all 30 packages.

The roadmap is complete because its strategically required Milestones 1-4 are
done and every later conditional gate is dispositioned. Milestone 5 is not
activated: the corpus still has only one dense authored consumer and 2.36 MiB
of reachable external bytes, so virtualization entry evidence is absent.
Milestone 6 is not activated because no reference-machine budget violation is
recorded. Milestone 7 is retained as conditional: v4 rollback remains required
until its compatibility window elapses. Those milestones require a new roadmap
activation if their evidence gates later pass.

## Outcome

Provide one authored-package payload architecture in which:

1. the reflected object stream retains stable payload identity and domain-owned
   semantic metadata without owning physical paths or backend policy;
2. an optional package trailer indexes only the placement states selected by
   activated plans and can be inspected without constructing package objects;
3. local payload publication is failure-atomic, with tail-only or
   append-generation updates permitted only where measured benefit and fault
   injection justify them over complete-file replacement;
4. a future persistent content-addressed backend can remove large bytes from the
   package workspace without making disposable DDC state authoritative or
   assuming the current integrity hash is a sufficient persistent key;
5. old DAST v4/DABK v1 assets remain readable throughout a deliberate,
   reversible canonical-resave migration; and
6. authored, derived, cooked, and runtime payload authorities remain separate.

## Scope

- Compare retaining DAST v4/DABK v1 with a new DAST version, a versioned outer
  envelope, a metadata trailer that retains LFS-backed companion bytes, and any
  other bounded package segment supported by evidence.
- Separate stable authored payload identity and domain semantics from content
  hash, compressed size, placement, offset, and backend location.
- Define logical payload-id scope and copy/duplication/reimport behavior, plus
  algorithm-versioned content-key and collision policy, before a new wire or
  persistent backend depends on them.
- Define trailer discovery, header, lookup table, payload-data, and footer
  contracts with explicit versioning, bounds, canonical ordering, and hashes.
- Define only the placement vocabulary needed by an activated child plan;
  reserve extensibility without treating referenced or virtualized states as
  implemented.
- Preserve construct-free inspection, corruption reporting, orphan discovery,
  repair, canonical resave, relocation, copy, deletion, bundle publication, and
  source-control workflows.
- Pilot the selected local trailer route with one existing production authored
  bulk consumer before migrating a larger corpus.
- Define persistent and cache backend roles, hydration, offline durability,
  garbage collection, and permissions before enabling virtualization.
- Establish compatibility and rollback rules for existing `.dasset` and
  `.dabulk` generations.
- Qualify supported-filesystem crash behavior and ordinary-Git/LFS checkout,
  history, submit, and partial-sync costs before selecting package-local bytes.

## Non-Goals

- Changing texture, mesh, terrain, animation, collision, or other domain payload
  schemas merely to adopt a new placement mechanism.
- Merging authored DABK, derived DDC, cooked DBLK, or runtime GPU payloads into
  one provider, descriptor, format, or failure policy.
- Treating DDC, a local hydration cache, or a remote cache as the sole durable
  authority for authored bytes.
- Copying Unreal Engine's package trailer or Virtual Assets wire format.
- Replacing cooked DBLK publication, Cook manifests, or IoStore-style cooked
  aggregation as part of the authored-package migration.
- Promising transparent offline access to a virtual-only payload that has never
  been hydrated. Virtualization must provide explicit prefetch, local-fallback,
  and offline-failure policy; fully local assets remain offline-capable.
- Converting every existing package in one release or deleting DABK support
  before the migrated corpus and rollback path are proven.
- Moving `.dasset` to Git LFS or rewriting repository history as an incidental
  format change; either requires an explicit source-control migration decision.
- Introducing a generic typed bulk schema; domains continue to own meaning and
  codecs over opaque verified bytes.

## Reference Model

Unreal Engine supplies the design reference through four separations:

- editor-time bulk payloads have content identifiers independent of user-facing
  asset names;
- a package trailer can hold local payload entries or references after the main
  object/export stream and can be discovered from a footer;
- virtualization moves payload bytes to a persistent content-addressed backend
  while the package retains payload metadata; and
- cache storage accelerates hydration but does not replace persistent storage.

Durin adopts the separation of concerns, not the implementation. In
particular, a content hash need not remain visible in a companion filename once
the package trailer or persistent backend owns generation lookup, but removing
generation-named DABK companions is an outcome of a passed migration plan, not
an initial assumption.

## Program Decisions and Invariants

### Authority and semantic ownership

- A payload is authored because its owning asset treats the bytes as durable
  source state. Placement in a trailer or backend does not make it derived.
- The owning domain retains schema version, dimensions/counts, portable format,
  validation, canonical codec, and DDC-key contribution.
- `FEditorBulkData` remains an opaque verified byte value with atomic
  replacement. It does not gain texture, mesh, element-type, backend-path, or
  runtime-resource semantics.
- DDC values remain disposable rebuild products. A persistent virtualization
  backend must be independently durable even when a DDC participates as a
  hydration cache.

### Identity and placement

- The object stream owns a stable logical payload identity. Its uniqueness
  scope and regeneration rules for duplication, copy, reimport, and cross-package
  moves are frozen before a new writer; a payload id is not a backend locator.
- A content key identifies one immutable byte generation and may change on
  every authored edit. The current XXH3-128 value remains a bounded integrity
  check; a persistent backend key must carry an algorithm/version and define
  collision rejection rather than silently assuming XXH3-128 is sufficient.
- Physical placement, stored size, compression, offset, container generation,
  and backend provenance belong to the trailer or authority-specific storage
  record unless compatibility forces a duplicated field during migration.
- Duplicate physical facts are transitional only. Readers must reject
  disagreement instead of choosing one copy silently.
- Package-domain callers receive verified bytes and diagnostics; they never
  construct backend paths or interpret trailer offsets directly.

### Trailer layout and access

- DAST v4 requires exactly five contiguous sections whose final extent equals
  the physical file size. A bare trailer appended to DAST v4 is therefore a new
  compatibility boundary, not an additive change accepted by the v4 reader.
- A qualified trailer has an independently versioned header, canonically
  ordered lookup entries, optional local payload bytes, and a footer that can
  locate and validate the trailer from end of file.
- Readers use checked arithmetic, bounded counts/sizes, explicit little-endian
  encoding, zero padding, non-overlapping ranges, and complete trailing-byte
  validation.
- Construct-free inspection can enumerate payload identifiers, hashes, logical
  and stored sizes, storage states, and integrity without loading domain
  objects.
- Logical-byte hashes, stored-byte hashes, codec identifiers, and sizes have
  explicit meanings. A reserved codec or placement value is unsupported until
  its owning plan implements and tests it.
- Tail-only rewrite is an optimization, not a correctness premise. The selected
  publication protocol must preserve the last complete generation across
  process termination, partial writes, flush failure, and catalog failure.
- Append-generation designs must bound stale generations and define explicit
  compaction; they may not permit unbounded package growth.

### Publication and recovery

- A save publishes metadata and every newly reachable payload as one logical
  consistency unit. No committed package may reference an unavailable local or
  persistent payload generation.
- Publication must validate detached candidate bytes before making them
  reachable. Failure preserves the last published package and payload set.
- Relocation, copy, deletion, Fix Up, bundle save, source replacement, and
  canonical resave participate in the same reachability model.
- Recovery distinguishes an incomplete new generation from corruption of the
  last committed generation and never repairs by silently rebuilding authored
  bytes from DDC.
- Source-control submit validation proves that every referenced authored
  payload is either included in the submitted package/companion closure or
  durably present in the configured persistent backend.
- Package-local bulk bytes do not become a production default until the
  ordinary-Git/LFS policy for `.dasset` is explicit and measured against the
  companion baseline.

### Compatibility and rollout

- Production readers continue to accept qualified DAST v4/DABK v1 assets until
  a dedicated retirement plan proves that no supported corpus, branch, or
  recovery workflow needs them.
- New writers do not change default placement until dual-read tooling,
  transactional save, inspection, repair, and rollback fixtures pass.
- Dual-read means a new reader accepts both legacy and selected new wires. It
  does not imply that an unmodified DAST v4 reader accepts a new trailer or
  outer envelope; the compatibility matrix records that old-reader result
  explicitly.
- Canonical resave is the normal migration path. A package publication succeeds
  before old companions become cleanup candidates.
- Domain schema versions do not change solely because payload placement moves.
- Any new package or trailer wire has golden bytes, corruption fixtures, and a
  documented compatibility matrix before the first production asset is saved.

## Current Foundations and Gaps

| Area | Current foundation | Gap before the target architecture |
| --- | --- | --- |
| Domain ownership | VolumeTexture and Texture2D prove domain metadata plus opaque bulk bytes | More authored dense consumers and cost evidence are needed |
| Authored storage | DAST v4 inline values and immutable DABK v1 generations are transactional and inspectable | Physical facts still live in the DAST bulk descriptor and visible companion generation; DAST v4 rejects physical trailing bytes |
| Identity | `FEditorBulkData` has a logical GUID and XXH3-128 integrity hash | Same-package duplicate GUIDs fail save, copies retain GUIDs, and no persistent content-key algorithm/collision contract exists |
| Container mechanics | Private bounded DABK/DBLK primitives provide canonical layout, hashing, and range validation | No authored package trailer/footer discovery contract exists |
| File publication | Atomic single-file replacement and staged package/companion publication are qualified | No qualified tail-rewrite, append-generation, or trailer-compaction protocol exists |
| Inspection and repair | Package and texture inspection report referenced and orphaned DABK state | No selected trailer placement vocabulary or hydration diagnostics exist |
| Migration | Canonical resave and compatibility inspection handle current DAST/DABK wires | No dual-read trailer migration matrix or corpus conversion tool exists |
| Virtualization | Content hashes and storage-neutral bulk values already exist | No persistent authored backend, hydration API, offline policy, permissions model, or garbage collector exists |
| Source control | `.dasset` is ordinary Git binary data and `.dabulk` is LFS-backed | Package-local bulk would change main-Git growth, checkout, merge, and LFS policy |
| Evidence | 32 tracked DAST packages total 71,797 bytes; two DABK companions total 2,359,616 bytes | No material change-rate, checkout, history, transfer, save-latency, or deduplication pressure is measured |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 0. Qualification and wire decision | Current DAST/DABK contracts and representative corpus | Measured corpus/source-control baseline, UE-reference analysis, alternatives record, compatibility matrix, and a recorded Proceed, Defer, or Retain decision; Proceed also selects the package/descriptor boundary and next child plan | Roadmap accepted and the qualification plan activated | Reproducible evidence covers payload counts/sizes/change rates, save and checkout costs, Git/LFS effects, supported-filesystem crash windows, identity/hash policy, and candidate layouts; Proceed proves benefit over DABK, while Defer or Retain records measurable revisit triggers and activates no production-format plan | Completed — Retain 2026-08-24 |
| 0.5 Strategic activation and boundary selection | Milestone 0 Retain evidence plus explicit user completion directive | Truthful strategic Proceed record selecting DAST v5, versioned EOF trailer/footer, external DABK v1 in Git LFS, full-file package replacement, and canonical v4 rollback | User explicitly directs completion of the full roadmap while accepting pre-threshold investment | Retain evidence remains intact; one boundary/placement is selected; no later evidence gate is waived; exactly one foundation plan is activated | Completed — Strategic Proceed 2026-08-24 |
| 1. Package trailer foundation | Milestone 0.5 | Bounded versioned trailer/footer model, detached builder/reader, only `ExternalDabkV1`, construct-free inspection, golden bytes, and corruption fixtures | Strategic activation records Proceed and selects DAST v5 plus trailer-indexed DABK v1 without changing domain schemas | Focused tests prove canonical ordering, EOF discovery, all structural bounds, unsupported-state rejection, duplicate/range/trailing rejection, and no object construction; no production writer uses the format yet | Completed 2026-08-24 |
| 2. Selected local authored payload publication | Milestone 1 | Dual-read package loading, failure-atomic publication for the selected package-local or companion-local placement, recovery/compaction policy, package operations integration, and opt-in writer | The selected placement shows measured or operational benefit, has an explicit Git/LFS policy, and preserves the prior generation under every injected failure | Save/reload, bundle, move, copy, delete, Fix Up, canonical resave, crash recovery, and catalog failure tests pass while DAST v4/DABK v1 remain readable | Completed 2026-08-24 |
| 3. VolumeTexture migration pilot | Milestone 2 | One production asset route writes through the selected trailer-indexed authored-voxel placement without domain-schema change and can roll back to DAST v4/DABK v1 during qualification | Selected local publication is qualified and VolumeTexture golden/source/DDC/Cook fixtures are green | Editor save/reload, reimport, DDC miss/rebuild, Cook/runtime, inspection/repair, source control, canonical resave, and rollback pass with exact disposition of legacy companions | Completed 2026-08-24 |
| 4. Corpus migration and default writer | Milestone 3 plus representative repository telemetry | Canonical corpus conversion, submit validation, cleanup, compatibility policy, default placement selection, and lasting contracts | The pilot has operated through representative edits and source-control workflows without unresolved durability regressions; current-machine performance remains diagnostic rather than blocking | Every tracked package is classified, migrated packages have no unreachable required payloads, old readers fail explicitly where required, rollback assets exist, and default-writer policy is documented | Completed 2026-08-24 |
| 5. Persistent authored virtualization | Milestone 3 or 4 | Content-addressed persistent backend, optional cache hierarchy, hydration, offline prefetch/failure policy, permissions, submit integration, provenance, repair, and garbage collection | Telemetry demonstrates material checkout/storage/transfer benefit; at least two authored consumers share the need; a durable backend and ownership model are selected | Prefetched and never-hydrated offline cases, cache miss, backend outage, permission, partial submit, concurrent edit, recovery, retention, and GC tests prove that no reachable authored payload depends solely on disposable cache state | Not activated — evidence gate unmet 2026-08-24 |
| 6. Trailer and backend optimization | Milestones 4-5 as applicable | Compression, cross-package deduplication, range IO, compaction scheduling, or package-container aggregation selected independently from measurements | Profiling identifies a specific storage, latency, memory, patch, or transfer budget violation | The selected optimization meets a frozen budget without weakening compatibility, durability, diagnostics, or domain ownership | Not activated — no reference budget violation 2026-08-24 |
| 7. Legacy DABK write retirement | Milestone 4 and, if adopted, Milestone 5 | Removal of default DABK writes and explicit long-term legacy-read disposition | All supported branches/corpora are migrated or have a documented compatibility route and rollback window has elapsed | No production workflow writes new DABK, legacy fixtures remain intentionally supported or are removed by an approved format-break policy, and orphan cleanup cannot delete reachable historical data | Retained conditional — rollback window remains 2026-08-24 |

Milestone 0 remains the required measured qualification and its Retain result is
not rewritten. Milestone 0.5 records the later strategic Proceed directive.
Milestones 1-4 therefore become required in order. Milestones 5-6 remain
evidence-gated and Milestone 7 remains conditional; strategic activation does
not waive their separate entry gates.

## Child Plan Boundaries

| Child plan | Activation condition | Owns | Must not own |
| --- | --- | --- | --- |
| [Authored Package Trailer Qualification](../Plans/AuthoredPackageTrailerQualification.md) | Completed 2026-08-24 with Retain | Measurements, UE comparison, format/placement alternatives, identity and hash policy, compatibility matrix, and Proceed/Defer/Retain decision | Production wire changes, source-control migration, or corpus mutation |
| [Authored Package Storage Strategic Activation](../Plans/AuthoredPackageStorageStrategicActivation.md) | Completed 2026-08-24 from explicit user completion directive | Strategic Proceed authority, DAST v5 boundary selection, external DABK v1 placement, compatibility/rollback selection, and foundation activation | Rewriting qualification measurements or waiving later evidence gates |
| [Authored Package Trailer Foundation](../Plans/AuthoredPackageTrailerFoundation.md) | Completed 2026-08-24 | Trailer/footer codec, detached builder/reader, construct-free inspection, golden and corruption fixtures | Production package codec registration, unselected placements, default writer, or virtualization backend |
| [Selected Local Authored Payload Publication](../Plans/SelectedLocalAuthoredPayloadPublication.md) | Completed 2026-08-24 | Atomic whole-file publication, recovery, package operation integration, dual-read and opt-in write | Remote backend or broad consumer migration |
| [VolumeTexture Trailer Migration](../Plans/VolumeTextureTrailerMigration.md) | Completed 2026-08-24 | VolumeTexture pilot, source/DDC/Cook equivalence, rollback, inspection/repair evidence | Generic texture schema redesign or unrelated consumer conversion |
| [Authored Trailer Corpus Migration](../Plans/AuthoredTrailerCorpusMigration.md) | Milestone 3 completed 2026-08-24 | Canonical resave, source-control submit checks, cleanup, default writer, compatibility disposition | Persistent backend implementation unless separately activated |
| Authored Payload Virtualization | Milestone 5 entry gate | Persistent/cache backend hierarchy, hydration, offline behavior, permissions, provenance, retention and GC | Making DDC authoritative or changing domain semantics |
| Legacy DABK Write Retirement | Milestone 7 entry gate | Writer retirement, final corpus audit, compatibility fixtures, lasting documentation | Premature removal of legacy reads or historical evidence |

Later child plans are created only when their activation condition is met. Each
plan owns concrete files, implementation stages, test selection, benchmark
budgets, and commit provenance. This roadmap owns ordering and cross-plan
invariants.

## Retain Revisit Gates

Milestone 0 reopens when any one gate is observed; reopening does not select a
wire or activate Milestone 1:

- reachable external authored bytes reach 256 MiB;
- tracked authored payload count reaches 100;
- measured monthly LFS transfer or rewritten authored bytes reach 2 GiB;
- two distinct dense authored consumers demonstrate the same storage need;
- publication warm p95 reaches 250 ms on the designated reference machine; or
- the exact construct-free workload remains above 50 ms warm p95 in two
  consecutive quiet Release runs on that reference machine.

A reopened candidate still requires at least 25% measured improvement in the
violated cost, plus every compatibility, durability, submit-closure, and
rollback gate. Measurements from a non-reference computer remain diagnostic.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Wire format | Golden header/entry/footer bytes; canonical ordering; version, endian, alignment, bounds, duplicate, overlap, gap, truncation, padding, hash, offset, footer, and trailing-byte failures |
| Identity | Payload-id uniqueness scope; copy, duplicate, reimport, and move behavior; logical-versus-stored hash meaning; algorithm/version; collision rejection |
| Compatibility | New-reader legacy read; selected new-format read/write; explicit old-reader result; canonical resave; unsupported-version diagnostics; domain schema unchanged; exact rollback corpus |
| Publication | Injected failure before payload durability, during trailer construction, flush, footer publication, package replacement, catalog publication, cleanup, and bundle member publication |
| Recovery | Incomplete tail, stale complete generation, corrupt latest footer, orphan local payload, missing persistent payload, interrupted compaction, and interrupted migration |
| Package operations | Save, reload, unload, move, rename, copy, delete, Fix Up, source replacement, bundle save, inspection, repair, and orphan cleanup |
| Asset lifecycle | Authored edit, reimport, DDC hit/miss/rebuild, Cook, cooked runtime load, decoded CPU publication, and GPU resource handoff remain authority-correct |
| Source control | Ordinary-Git object growth, LFS object/transfer growth, checkout size/time, submit closure, concurrent edits, rename history, `.gitattributes` policy, partial sync, rollback, and branch compatibility |
| Virtualization | Persistent/cache separation, first hydration, warm hydration, explicit never-hydrated offline behavior, prefetch, backend outage, permissions, concurrency, retention, GC reachability, and disaster recovery |
| Performance | Package open, construct-free inspection, save latency, bytes rewritten, checkout/transfer volume, peak memory, compression cost, and compaction amplification against frozen baselines |
| Repository | Focused native tests, selected aggregates, required build targets, documentation validation, and representative asset corpus validation follow the root agent guides |

## Risks and Control Gates

- Moving content hashes out of DAST can make object-level identity appear stable
  while bytes change. Trailer identity must participate in dirty detection,
  DDC keys, catalog fingerprints, transactions, and diagnostics explicitly.
- `FEditorBulkData` copies currently retain a payload GUID while one package
  rejects duplicate GUIDs. A new index cannot silently choose aliasing or
  regeneration semantics; qualification freezes the lifecycle before writing.
- XXH3-128 is currently an integrity hash, not an approved durable global key.
  Persistent content addressing requires an algorithm-versioned key and an
  explicit collision response.
- Keeping both DAST and trailer copies of physical facts can create split-brain
  packages. Transitional duplication is validated for exact agreement and
  removed on a scheduled compatibility boundary.
- In-place tail replacement can corrupt the only generation. No production
  writer uses it until fault injection proves recoverability on supported file
  systems; full-file atomic replacement remains the fallback.
- Append-only generations can grow without bound. Every append design freezes
  compaction thresholds, crash behavior, disk-space bounds, and cleanup rules
  before activation.
- Embedding large local payloads can increase Git/LFS rewrite and checkout cost
  even when engine IO touches only the tail. Source-control telemetry, not local
  seek/write behavior alone, selects local trailer versus companion placement.
  Because `.dasset` is ordinary Git today, package-local bulk also requires an
  explicit decision about main-repository object growth or LFS migration.
- Virtualized authored bytes can be mistaken for cached derived data. Persistent
  backend health, retention, backup, permissions, and offline prefetch/failure
  policy are hard entry gates, and DDC-only availability is always a failure.
- A never-hydrated virtual payload cannot be promised offline without another
  durable local copy. Prefetch and explicit offline failure are part of the
  product contract rather than hidden recovery behavior.
- Backend garbage collection can destroy reachable source. GC operates from a
  conservative reachability snapshot, honors retention and branch policy, and
  supports audit/dry-run before deletion.
- Broad migration can strand old branches or tools. Dual-read precedes new
  writes, the pilot precedes the default writer, and legacy retirement is a
  separate conditional milestone.
- Copying UE abstractions without Durin evidence can add unused complexity. Each
  capability beyond local trailer lookup requires a measured consumer and its
  own acceptance budget.

## Completion Criteria

- The selected package/trailer contract is documented in the owning asset and
  serialization documentation with stable golden bytes and compatibility rules.
- Reflected assets retain stable logical payload identity and domain semantics;
  physical placement and backend facts are resolved by AssetCore storage
  services and are construct-free inspectable.
- At least one production authored bulk consumer completes save/reload,
  reimport, DDC miss/rebuild, Cook/runtime, source-control, repair, and rollback
  through the new route without domain-schema change.
- Every supported old asset has an explicit read, migrate, reject, or retain
  disposition, and canonical resave never deletes the last reachable authored
  payload generation.
- Required milestones are complete. Evidence-gated optimization and
  virtualization milestones are completed or explicitly dispositioned from
  measurements rather than assumption.
- If qualification records Defer or Retain, its revisit triggers remain in the
  roadmap and no downstream plan is activated. The roadmap is not marked
  complete until the target outcome is delivered or deliberately superseded.
- Lasting rules replace roadmap-only specifications in the owning contracts,
  and every activated child plan is completed or deliberately superseded.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File IO](../Runtime/Core/FileIO.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Domain-Owned Large Asset Payload Architecture](LargeAssetPayloadArchitecture.md)
- [Unreal Engine `FPackageTrailer`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FPackageTrailer)
- [Unreal Engine `FPackageTrailerBuilder`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FPackageTrailerBuilder)
- [Unreal Engine `FEditorBulkData`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FEditorBulkData)
- [Unreal Engine Virtual Assets backend graphs](https://dev.epicgames.com/documentation/en-us/unreal-engine/backend-graphs-for-virtual-assets-in-unreal-engine)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorageTypes.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageAuthoring.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/FileHelper.h`
- `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp`
