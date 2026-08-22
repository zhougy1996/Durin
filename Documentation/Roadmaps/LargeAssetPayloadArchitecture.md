# Large Asset Payload Architecture Roadmap

Summary: Evolve Durin from large reflected inline arrays toward explicit atomic buffers, authored bulk storage, and streamable asset payloads.

Last reviewed: 2026-08-22

Status: Active
Completed:

## Current Status

Durin now distinguishes ordinary `Array<UInt8>` from an atomic reflected byte
Blob. A real `128^3` volume source plans as one logical value and completes
import, authored save/reload, reimport, and Cook without raising structural
planner limits. Core Archive already carries a `BulkData` purpose and
`Inline`/`Skip`/`External` policy, while AssetCore owns bounded cooked `.dbulk`
descriptors, containers, transactional publication, and load validation.

The remaining gap is architectural rather than another scalar-property case.
Large authored source payloads are still resident inline values in `.dasset`;
there is no reusable authored bulk owner, companion format, residency state, or
typed portable-buffer layer. Adding isolated `FVector`, `FColor`, height, mesh,
or animation exceptions now would create parallel formats before the common
ownership and failure contract exists.

The first milestone is active through
[Authored Asset Bulk Data Foundation](../Plans/AuthoredAssetBulkDataFoundation.md).
Later plans are named here but remain uncreated until their entry gates pass.

## Outcome

Provide one long-lived large-payload architecture in which ordinary reflected
containers retain element-addressable semantics, atomic typed buffers retain a
portable element schema without consuming per-element planner nodes, and bulk
payloads may live outside the object package with bounded synchronous or
asynchronous residency. Authored source, derived cache entries, and cooked
runtime payloads remain distinct lifecycle classes while sharing descriptors,
validation, hashing, and byte ownership where their contracts agree.

## Scope

- An explicit authored bulk-data value and AssetCore storage boundary.
- Deterministic inline-versus-external placement without changing logical type.
- Portable typed atomic buffers for concrete dense numeric/struct consumers.
- Versioned migrations from large inline Array/Blob fields.
- Common payload descriptors, hashes, bounds, diagnostics, and transactional
  publication across authoring, DDC, Cook, and runtime load where appropriate.
- Synchronous residency first, followed by measured asynchronous loading,
  cancellation, memory budgeting, and optional mapping.
- Consumer plans for texture/volume source, mesh descriptions, terrain,
  animation, collision, or later dense asset domains when evidence justifies
  migration.

## Non-Goals

- Automatically changing an ordinary Array's meaning when its length crosses a
  threshold.
- Making every large payload reflected or editable at element granularity.
- Persisting native C++ layout, padding, allocator state, pointers, GLM storage,
  or platform endianness.
- Replacing source-control files, DDC, Cook manifests, or runtime resource
  streaming with one undifferentiated byte store.
- Building remote content virtualization, global deduplication, compression,
  encryption, or patch delivery before local ownership and recovery are proven.
- Redesigning networking replication; replicated collection deltas require a
  separate protocol and consumer.

## Program Decisions and Invariants

- Semantic type is explicit. `std::vector<T>` remains an ordinary reflected
  Array unless its declaration uses a dedicated atomic/bulk value type; field
  name and runtime size never select semantics.
- Structural complexity and payload volume use separate budgets. An atomic
  buffer or bulk handle contributes one logical field, while element count,
  decoded bytes, stored bytes, allocation, package, and residency limits remain
  independently enforced.
- Logical representation and physical placement are separate. The same bulk
  property may be inline or externally stored without changing its reflected
  schema, authored override path, or consumer API.
- Core owns portable byte buffers, Archive mechanics, hashes, and primitive
  codecs. AssetCore owns authored/cooked package descriptors, locations,
  publication, retention, and recovery. Engine and developer modules own
  semantic codecs and asset-specific validation.
- Typed buffers serialize an explicit stable element format. `FVector`,
  `FColor`, float, half, index, and custom records never use `sizeof(T)` as a
  persistent contract unless a named codec freezes every byte and alignment.
- Bulk payloads contain no implicit object references. Durable dependencies
  remain visible in package metadata so catalog, GC, Cook reachability, move,
  and delete policy do not need to load opaque bytes.
- Authored source, derived data, and cooked output have distinct authorities.
  A DDC miss may rebuild; authored loss may not. Cooked `.dbulk` remains
  manifest-owned deployable output and is not silently reused as authored
  storage.
- Publication is transactional across the logical package and its payload set.
  A crash may leave an unreferenced candidate file, but never a published
  package that references absent or unverified bytes; recovery and cleanup are
  deterministic.
- Payload identity includes format/version, logical size, and a strong content
  hash. Hash equality is never allowed to make incompatible formats identical,
  and integrity is verified before publication to a live consumer.
- Loading is transactional and bounded. Decode or IO failure preserves the
  prior live object/resource. Async work may not publish after cancellation,
  unload, reimport, or revision replacement.
- Synchronous correctness and corpus migration precede streaming optimization.
  Mapping, compression, deduplication, and remote virtualization require
  measurements and do not weaken the base API.

## Current Foundations and Gaps

| Area | Foundation | Program gap |
| --- | --- | --- |
| Reflection | Atomic `std::byte` Blob and ordinary Array semantics are distinct. | No typed atomic buffer or bulk-data reflected value. |
| Archive | Logical `Bytes`, Blob bounds, `BulkData` purpose, and bulk policy already exist. | No common bulk descriptor/residency operation behind those policies. |
| Authored packages | DAST v4 is bounded, field-tagged, transactional, and compatibility-aware. | Large authored values are inline and the 256 MiB package ceiling owns them. |
| Cooked packages | Descriptor-backed `.dbulk`, hashes, manifests, and publication exist. | Contracts are Cook-specific and cannot own editable authored source. |
| Derived data | DDC uses deterministic keys and validated asset-specific payloads. | Payload ownership and byte sharing are producer-specific. |
| Consumers | Volume source proves atomic Blob semantics; textures, meshes, terrain, animation, and collision already expose dense data. | Migration policy and stable typed codecs are not unified. |
| Residency | Asset packages and GPU resources have explicit lifetime/revision rules. | Large authored payloads have no unloaded/loading/resident/failed state or memory budget. |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 1. Authored bulk-data foundation | Reflected Blob and DAST v4 | Atomic bulk owner, descriptor, transactional authored companion, synchronous load, and one volume-source migration | Blob production regression and cooked bulk contracts are green | Historical/current volume assets save, load, move, delete, reimport, and Cook with verified external authored bytes | Active |
| 2. Portable typed atomic buffers | Milestone 1 descriptor and byte owner | Stable codec boundary and reflected typed-buffer value for selected scalar/struct formats | At least two concrete consumers require element metadata without per-element editing | Codec identity, canonical bytes, comparison, migration, tooling summary, and bounds pass for selected formats | Proposed |
| 3. Consumer migration program | Milestones 1-2 as required per consumer | Separate bounded plans for texture, mesh, terrain, animation, collision, or other dense sources | Consumer has measured package/memory/planning cost and a frozen compatibility baseline | Selected consumers no longer retain oversized ordinary Arrays; DDC/Cook/runtime bytes remain compatible or versioned | Proposed |
| 4. Unified payload lifecycle and diagnostics | Milestone 1 plus two migrated producers | Shared authored/DDC/Cook descriptor vocabulary, inspection, audit, repair, and orphan cleanup | Real producers expose duplicated ownership or diagnostics | Tools can trace every payload from owner through authored, derived, and cooked states without opaque paths | Proposed |
| 5. Async residency and memory budgets | Milestones 1 and 4 | Async request/cancel/publish API, residency accounting, eviction policy, and optional mapping | Synchronous profiling shows startup or peak-residency pressure | Stress tests prove bounded memory, cancellation safety, unload/reload, stale-result rejection, and deterministic fallback | Evidence-gated |
| 6. Compression, deduplication, and virtualization | Milestone 4, optionally 5 | Policy-selected compression and local/remote content-addressed storage | Corpus telemetry demonstrates material storage or transfer savings | Recovery, source-control, patching, security, and offline workflows pass without weakening authored durability | Optional |

## Child Plan Boundaries

| Child plan | Owns | Must not own |
| --- | --- | --- |
| [Authored Asset Bulk Data Foundation](../Plans/AuthoredAssetBulkDataFoundation.md) | Authored bulk value, descriptor, local companion transaction, synchronous residency, Blob-to-bulk volume migration | Generic typed vector codecs, async streaming, global consumer conversion, remote storage |
| Portable Typed Atomic Buffers (proposed) | Codec identity, canonical scalar/record encoding, reflected atomic typed values, editor summaries | Physical payload placement or package transaction |
| Asset Payload Consumer Migrations (one plan per bounded domain) | Domain schema versions, exact byte compatibility, authoring and Cook workflow | Redefining common bulk APIs for one producer |
| Payload Lifecycle Inspection and Repair (proposed) | Audit graph, diagnostics, orphan detection, repair and cleanup tooling | Runtime streaming policy |
| Asset Payload Residency and Streaming (evidence-gated) | Requests, cancellation, priorities, budgets, eviction and mapping | Authored/cooked format reinvention |

Only the first child plan is active. A later plan is created when its entry gate
is satisfied and its working set can be validated independently.

## Program Validation Matrix

| Concern | Required program evidence |
| --- | --- |
| Semantic stability | Ordinary Arrays never change schema or override behavior because of size; atomic/bulk declarations are explicit. |
| Portable bytes | Golden vectors cover every admitted codec, endian rule, NaN policy, channel order, and malformed input. |
| Structural planning | Multi-million-element atomic/bulk values contribute bounded logical nodes while byte/allocation ceilings still reject excess. |
| Authored durability | Save, crash-window recovery, move, copy, rename, delete, source-control checkout, and canonical resave retain the exact payload set. |
| Compatibility | Supported inline Array/Blob packages load transactionally and resave into the current representation. |
| Derived and cooked identity | DDC keys, asset-specific payloads, Cook manifests, and runtime outputs remain identical or receive an explicit version migration. |
| Residency | Sync and later async load, cancellation, eviction, unload, and stale completion cannot expose partial bytes or revive retired objects. |
| Security and bounds | Counts, offsets, extents, hashes, decompression ratios, paths, and allocation limits reject before mutation. |
| Tooling | Inspectors report owner, logical format, version, hash, size, storage state, residency, and actionable failure. |
| Aggregate | Focused tests, full native aggregate, Debug Editor build, documentation validation, and editor workflow smokes remain green per child plan. |

## Risks and Control Gates

- Cross-file publication can create dangling references or delete the last good
  payload. Milestone 1 must freeze write order, fsync/rename expectations,
  orphan tolerance, and cleanup only after successful package commit.
- Content hashes can be mistaken for complete type identity. Descriptors always
  include format/version and sizes, and loaded bytes are verified.
- A generic typed-buffer template can accidentally persist ABI layout. No codec
  lands without golden canonical bytes and a named version.
- Externalization can make common editor operations unexpectedly blocking.
  Milestone 1 exposes residency explicitly; Milestone 5 starts only after sync
  behavior and measurements are available.
- Unknown-field retention can lose external payload reachability. DAST
  compatibility and inspection must retain or deliberately reject every
  referenced payload before the format is qualified.
- Converting every producer at once would make regressions untraceable. Each
  consumer migration remains a separate plan with exact old/new byte evidence.

## Completion Criteria

- Large authored payloads use explicit atomic or bulk types rather than
  oversized ordinary reflected Arrays.
- Authored, derived, and cooked payload lifecycles have stable owners,
  descriptors, bounds, diagnostics, and transactional publication.
- Selected typed buffers use portable versioned codecs and never native-memory
  persistence.
- Every supported historical representation has a tested migration or an
  explicit unsupported-baseline decision.
- Required milestones pass their exit gates; evidence-gated and optional
  milestones are completed or dispositioned from measurements.
- Lasting contracts live in Core, Asset, and consumer documentation rather than
  only in child plans.

## Related Documentation

- [Serialization](../Runtime/Core/Serialization.md)
- [Generated Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)
- [Reflected Byte Blob Serialization Plan](../Plans/ReflectedByteBlobSerialization.md)
- [Testing](../Agents/Testing.md)
- [Build and Run](../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/Cook.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
