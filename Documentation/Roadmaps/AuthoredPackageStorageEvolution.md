# Authored Package Storage Evolution Roadmap

Summary: Evolve authored bulk storage from generation-named DABK companions toward trailer-indexed, content-addressed, and optionally virtualized package payloads without changing domain-owned schemas or weakening durability.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

Durin currently publishes authored bulk bytes as either inline DAST v4 values
or immutable generation-named DABK v1 companions. The package stores enough
physical identity to resolve and verify each external companion, and save,
bundle publication, relocation, deletion, inspection, repair, and canonical
resave treat the package plus all reachable companions as one consistency unit.
This behavior is implemented and remains the production baseline.

The completed
[Domain-Owned Large Asset Payload Architecture](LargeAssetPayloadArchitecture.md)
roadmap deliberately deferred authored virtualization and package-container
evolution until corpus and source-control telemetry justified the additional
system. The current corpus is too small to justify immediate replacement, but
future editable mesh, terrain, texture, animation, and other dense authored
sources may make visible per-generation companions, checkout cost, and local
payload residency material constraints.

This roadmap records the intended evolution before those consumers proliferate.
It uses Unreal Engine's package trailer, editor bulk data, payload identifier,
and Virtual Assets separation as a non-normative reference. It does not copy UE
wire formats, names, workspace-domain implementation, or backend policy.
Implementation plans remain gated by measured need and must preserve the
qualified DAST v4/DABK v1 route until their own migration gates pass.

## Outcome

Provide one authored-package payload architecture in which:

1. the reflected object stream retains stable payload identity and domain-owned
   semantic metadata without owning physical paths or backend policy;
2. an optional package trailer indexes local, referenced, or virtualized
   payload facts by content identity and can be inspected without constructing
   package objects;
3. local payload publication is failure-atomic and permits bounded tail-only or
   append-generation updates where the selected wire makes them safe;
4. a future persistent content-addressed backend can remove large bytes from the
   package workspace without making disposable DDC state authoritative;
5. old DAST v4/DABK v1 assets remain readable throughout a deliberate,
   reversible canonical-resave migration; and
6. authored, derived, cooked, and runtime payload authorities remain separate.

## Scope

- Qualify whether a package trailer is an additive outer envelope, a new DAST
  authored-package version, or another bounded package segment.
- Separate stable authored payload identity and domain semantics from content
  hash, compressed size, placement, offset, and backend location.
- Define trailer discovery, header, lookup table, payload-data, and footer
  contracts with explicit versioning, bounds, canonical ordering, and hashes.
- Define local, referenced, and virtualized payload states while implementing
  only the states justified by an active child plan.
- Preserve construct-free inspection, corruption reporting, orphan discovery,
  repair, canonical resave, relocation, copy, deletion, bundle publication, and
  source-control workflows.
- Pilot the selected local trailer route with one existing production authored
  bulk consumer before migrating a larger corpus.
- Define persistent and cache backend roles, hydration, offline durability,
  garbage collection, and permissions before enabling virtualization.
- Establish compatibility and rollback rules for existing `.dasset` and
  `.dabulk` generations.

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
- Requiring remote connectivity to open, save, recover, move, or submit every
  authored asset.
- Converting every existing package in one release or deleting DABK support
  before the migrated corpus and rollback path are proven.
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

- The object stream owns a stable logical payload identity. A content hash
  identifies one immutable byte generation and may change on every authored
  edit.
- Physical placement, stored size, compression, offset, container generation,
  and backend provenance belong to the trailer or authority-specific storage
  record unless compatibility forces a duplicated field during migration.
- Duplicate physical facts are transitional only. Readers must reject
  disagreement instead of choosing one copy silently.
- Package-domain callers receive verified bytes and diagnostics; they never
  construct backend paths or interpret trailer offsets directly.

### Trailer layout and access

- A qualified trailer has an independently versioned header, canonically
  ordered lookup entries, optional local payload bytes, and a footer that can
  locate and validate the trailer from end of file.
- Readers use checked arithmetic, bounded counts/sizes, explicit little-endian
  encoding, zero padding, non-overlapping ranges, and complete trailing-byte
  validation.
- Construct-free inspection can enumerate payload identifiers, hashes, logical
  and stored sizes, storage states, and integrity without loading domain
  objects.
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

### Compatibility and rollout

- Production readers continue to accept qualified DAST v4/DABK v1 assets until
  a dedicated retirement plan proves that no supported corpus, branch, or
  recovery workflow needs them.
- New writers do not change default placement until dual-read tooling,
  transactional save, inspection, repair, and rollback fixtures pass.
- Canonical resave is the normal migration path. A package publication succeeds
  before old companions become cleanup candidates.
- Domain schema versions do not change solely because payload placement moves.
- Any new package or trailer wire has golden bytes, corruption fixtures, and a
  documented compatibility matrix before the first production asset is saved.

## Current Foundations and Gaps

| Area | Current foundation | Gap before the target architecture |
| --- | --- | --- |
| Domain ownership | VolumeTexture and Texture2D prove domain metadata plus opaque bulk bytes | More authored dense consumers and cost evidence are needed |
| Authored storage | DAST v4 inline values and immutable DABK v1 generations are transactional and inspectable | Physical facts still live in the DAST bulk descriptor and visible companion generation |
| Container mechanics | Private bounded DABK/DBLK primitives provide canonical layout, hashing, and range validation | No authored package trailer/footer discovery contract exists |
| File publication | Atomic single-file replacement and staged package/companion publication are qualified | No qualified tail-rewrite, append-generation, or trailer-compaction protocol exists |
| Inspection and repair | Package and texture inspection report referenced and orphaned DABK state | No local/referenced/virtualized trailer state or hydration diagnostics exist |
| Migration | Canonical resave and compatibility inspection handle current DAST/DABK wires | No dual-read trailer migration matrix or corpus conversion tool exists |
| Virtualization | Content hashes and storage-neutral bulk values already exist | No persistent authored backend, hydration API, offline policy, permissions model, or garbage collector exists |
| Evidence | Two current DABK companions prove correctness | No material checkout, storage, transfer, save-latency, or deduplication pressure is measured |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 0. Qualification and wire decision | Current DAST/DABK contracts and representative corpus | Measured corpus/source-control baseline, UE-reference analysis, alternatives record, selected outer-package and descriptor boundary, compatibility matrix, and child-plan split | Roadmap accepted as the future direction | Measurements cover payload counts/sizes/change rates, save and checkout costs, crash windows, source-control backends, and candidate trailer layouts; the selected design proves why it is preferable to retaining DABK | Proposed |
| 1. Package trailer foundation | Milestone 0 | Bounded versioned trailer/footer model, detached builder/reader, local/virtualized/reference entry vocabulary, construct-free inspection, golden bytes, and corruption fixtures | Milestone 0 selects a trailer and object-stream boundary without changing domain schemas | Focused tests prove canonical ordering, backwards discovery, all structural bounds, duplicate/range/trailing rejection, and no object construction; no production writer uses the format yet | Proposed |
| 2. Local authored payload publication | Milestone 1 | Dual-read package loading, failure-atomic local trailer save, recovery/compaction policy, package operations integration, and opt-in writer | A local trailer shows measured or operational benefit and the publication protocol preserves the prior generation under every injected failure | Save/reload, bundle, move, copy, delete, Fix Up, canonical resave, crash recovery, and catalog failure tests pass while DAST v4/DABK v1 remain readable | Proposed |
| 3. VolumeTexture migration pilot | Milestone 2 | One production asset route writes trailer-backed authored voxels without domain-schema change and can roll back to the legacy route during qualification | Local publication is qualified and VolumeTexture golden/source/DDC/Cook fixtures are green | Editor save/reload, reimport, DDC miss/rebuild, Cook/runtime, inspection/repair, source control, canonical resave, and rollback pass with exact disposition of legacy companions | Proposed |
| 4. Corpus migration and default writer | Milestone 3 plus representative repository telemetry | Canonical corpus conversion, submit validation, cleanup, compatibility policy, default placement selection, and lasting contracts | The pilot has operated through representative edits and source-control workflows without unresolved durability or performance regressions | Every tracked package is classified, migrated packages have no unreachable required payloads, old readers fail explicitly where required, rollback assets exist, and default-writer policy is documented | Proposed |
| 5. Persistent authored virtualization | Milestone 3 or 4 | Content-addressed persistent backend, optional cache hierarchy, hydration, offline fallback, permissions, submit integration, provenance, repair, and garbage collection | Telemetry demonstrates material checkout/storage/transfer benefit; at least two authored consumers share the need; a durable backend and ownership model are selected | Offline, cache-miss, backend outage, permission, partial-submit, concurrent edit, recovery, retention, and GC tests prove that no reachable authored payload depends solely on disposable cache state | Evidence-gated |
| 6. Trailer and backend optimization | Milestones 4-5 as applicable | Compression, cross-package deduplication, range IO, compaction scheduling, or package-container aggregation selected independently from measurements | Profiling identifies a specific storage, latency, memory, patch, or transfer budget violation | The selected optimization meets a frozen budget without weakening compatibility, durability, diagnostics, or domain ownership | Evidence-gated |
| 7. Legacy DABK write retirement | Milestone 4 and, if adopted, Milestone 5 | Removal of default DABK writes and explicit long-term legacy-read disposition | All supported branches/corpora are migrated or have a documented compatibility route and rollback window has elapsed | No production workflow writes new DABK, legacy fixtures remain intentionally supported or are removed by an approved format-break policy, and orphan cleanup cannot delete reachable historical data | Conditional |

## Child Plan Boundaries

| Proposed child plan | Activation condition | Owns | Must not own |
| --- | --- | --- | --- |
| Authored Package Trailer Qualification | Roadmap approval | Measurements, UE comparison, format alternatives, selected package/descriptor boundary, compatibility matrix | Production wire changes or corpus mutation |
| Authored Package Trailer Foundation | Milestone 0 exit gate | Trailer/footer codec, builder/reader, inspection model, golden and corruption fixtures | Asset-domain codecs, production default writer, virtualization backend |
| Local Authored Trailer Publication | Milestone 1 exit gate | Atomic/tail/append publication choice, recovery, compaction, package operation integration, dual-read and opt-in write | Remote backend or broad consumer migration |
| VolumeTexture Trailer Migration | Milestone 2 exit gate | VolumeTexture pilot, source/DDC/Cook equivalence, rollback, inspection/repair evidence | Generic texture schema redesign or unrelated consumer conversion |
| Authored Trailer Corpus Migration | Milestone 3 exit gate | Canonical resave, source-control submit checks, cleanup, default writer, compatibility disposition | Persistent backend implementation unless separately activated |
| Authored Payload Virtualization | Milestone 5 entry gate | Persistent/cache backend hierarchy, hydration, offline behavior, permissions, provenance, retention and GC | Making DDC authoritative or changing domain semantics |
| Legacy DABK Write Retirement | Milestone 7 entry gate | Writer retirement, final corpus audit, compatibility fixtures, lasting documentation | Premature removal of legacy reads or historical evidence |

Child plans are created only when their activation condition is met. Each plan
owns concrete files, implementation stages, test selection, benchmark budgets,
and commit provenance. This roadmap owns ordering and cross-plan invariants.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Wire format | Golden header/entry/footer bytes; canonical ordering; version, endian, alignment, bounds, duplicate, overlap, gap, truncation, padding, hash, offset, footer, and trailing-byte failures |
| Compatibility | Old DAST/DABK read; new format read/write; canonical resave; unsupported-new-reader diagnostics; domain schema unchanged; exact rollback corpus |
| Publication | Injected failure before payload durability, during trailer construction, flush, footer publication, package replacement, catalog publication, cleanup, and bundle member publication |
| Recovery | Incomplete tail, stale complete generation, corrupt latest footer, orphan local payload, missing persistent payload, interrupted compaction, and interrupted migration |
| Package operations | Save, reload, unload, move, rename, copy, delete, Fix Up, source replacement, bundle save, inspection, repair, and orphan cleanup |
| Asset lifecycle | Authored edit, reimport, DDC hit/miss/rebuild, Cook, cooked runtime load, decoded CPU publication, and GPU resource handoff remain authority-correct |
| Source control | Checkout size/time, submit closure, concurrent edits, rename history, LFS behavior where used, partial sync, rollback, and branch compatibility |
| Virtualization | Persistent/cache separation, first hydration, warm hydration, offline operation, backend outage, permissions, concurrency, retention, GC reachability, and disaster recovery |
| Performance | Package open, construct-free inspection, save latency, bytes rewritten, checkout/transfer volume, peak memory, compression cost, and compaction amplification against frozen baselines |
| Repository | Focused native tests, selected aggregates, required build targets, documentation validation, and representative asset corpus validation follow the root agent guides |

## Risks and Control Gates

- Moving content hashes out of DAST can make object-level identity appear stable
  while bytes change. Trailer identity must participate in dirty detection,
  DDC keys, catalog fingerprints, transactions, and diagnostics explicitly.
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
- Virtualized authored bytes can be mistaken for cached derived data. Persistent
  backend health, retention, backup, permissions, and offline fallback are hard
  entry gates, and DDC-only availability is always a failure.
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
- Lasting rules replace roadmap-only specifications in the owning contracts,
  and every activated child plan is completed or deliberately superseded.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File IO](../Runtime/Core/FileIO.md)
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
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/FileHelper.h`
- `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp`
