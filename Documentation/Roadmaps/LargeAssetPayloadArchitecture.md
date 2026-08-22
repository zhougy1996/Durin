# Large Asset Payload Architecture Roadmap

Summary: Evolve Durin from large reflected inline arrays toward one logical BulkData API with domain-specific authored, derived, and cooked storage.

Last reviewed: 2026-08-22

Status: Active
Completed:

## Current Status

Durin now distinguishes ordinary Arrays, atomic byte Blobs, and authored bulk
data. Core owns immutable shared bytes and the semantic Archive operation;
AssetCore owns the descriptor, explicit synchronous residency, DAST opcode,
deterministic DABK v1 companion, cross-file publication, relocation, deletion,
inspection, and recovery. Normalized volume source is the production consumer:
its real `128^3` workflow plans as one node and completes save/reload, failure
recovery, reimport, and Cook with external verified authored bytes.

Milestone 1 is complete through
[Authored Asset Bulk Data Foundation](../Plans/AuthoredAssetBulkDataFoundation.md).
The next selected milestone is a Unified BulkData API. Authored DABK and cooked
DBLK now provide the two concrete lifecycle implementations needed to extract a
shared logical descriptor, immutable byte owner, residency/request surface, and
storage-provider boundary without guessing from a single producer. Existing
`FAuthoredBulkData` remains the compatibility facade until that migration is
complete. Portable Typed Atomic Buffers follows the unified API and still waits
for two consumers that require portable element metadata.

## Outcome

Provide one long-lived logical `BulkData` API in which consumers use the same
identity, immutable byte view, residency, synchronous load, and later async
request operations regardless of physical placement. Ordinary reflected
containers retain element-addressable semantics, while atomic typed buffers
retain a portable element schema without consuming per-element planner nodes.
Authored source, derived cache entries, and cooked runtime payloads remain
distinct authority and transaction domains behind storage providers; unifying
the API does not merge their files, wire formats, durability, or rebuild rules.

## Scope

- An explicit authored bulk-data value and AssetCore storage boundary.
- A unified logical bulk value, descriptor, residency/request API, and provider
  contract used by authored and cooked payloads and reusable by DDC adapters.
- Deterministic inline-versus-external placement without changing logical type.
- Portable typed atomic buffers for concrete dense numeric/struct consumers.
- Versioned migrations from large inline Array/Blob fields.
- Common logical identity, hashes, bounds, diagnostics, and access semantics
  across authoring, DDC, Cook, and runtime load, with domain-owned publication.
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
- Requiring DABK, DBLK, DDC records, and future IoStore-style containers to use
  one physical wire format or one transaction coordinator.
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
- `FBulkData` is the consumer-facing logical value. It owns a common descriptor,
  immutable resident bytes, residency/failure state, and provider handle;
  authored, derived, and cooked wrappers may add policy but must not expose a
  second incompatible byte-access or request protocol.
- The common descriptor freezes placement-independent payload id, semantic
  format/version, logical and stored sizes, and strong content hash. Storage
  domain, provider key, placement flags, compression, and container coordinates
  are provider metadata and never change logical equality.
- A storage provider resolves, verifies, and publishes bytes for one lifecycle
  domain. Providers may use DABK, DBLK, DDC, loose files, or later container IO;
  callers do not construct physical paths or interpret backend offsets.
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
- Mutation capability is domain-specific even though read access is unified.
  Authored values support detached replacement through package publication;
  derived values are replaced by cache production; cooked values are immutable
  build outputs. A generic mutable lock is not part of the common API.
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
| Reflection | Array, Blob, and authored `BulkData` are distinct atomic/logical identities. | No portable typed atomic buffer value. |
| Archive | Immutable shared bytes and observable Inline/Skip/External bulk transfer are landed. | No provider-neutral request handle or async operation. |
| Authored packages | `FAuthoredBulkData`, DAST descriptors, and DABK v1 publish and mutate transactionally. | The authored name and loader are still the only public value API. |
| Cooked packages | Descriptor-backed `.dbulk`, hashes, manifests, and publication exist. | Cook uses a parallel descriptor/access vocabulary instead of the authored logical API. |
| Derived data | DDC uses deterministic keys and validated asset-specific payloads. | No adapter exposes cached bytes through the common residency/request surface. |
| Consumers | Volume source proves authored BulkData semantics; textures, meshes, terrain, animation, and collision already expose dense data. | Other consumers still use producer-specific ownership and access APIs. |
| Residency | Authored bulk exposes unloaded/resident/failed synchronous state; cooked data already supports deferred IO. | State, failure, and request behavior are not one contract; eviction and budgets remain absent. |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 1. Authored bulk-data foundation | Reflected Blob and DAST v4 | Atomic bulk owner, descriptor, transactional authored companion, synchronous load, and one volume-source migration | Blob production regression and cooked bulk contracts are green | Historical/current volume assets save, load, move, delete, reimport, and Cook with verified external authored bytes | Completed |
| 2. Unified BulkData API | Milestone 1 plus existing cooked DBLK | Common `FBulkData`, logical descriptor, immutable view, sync residency, provider interface, authored compatibility facade, and cooked adapter | Authored and cooked implementations provide two proven contracts to compare | Volume authored source and one cooked runtime payload use one access/identity API while DABK/DBLK transactions and bytes remain unchanged | Selected next |
| 3. Portable typed atomic buffers | Milestone 2 | Stable codec boundary and reflected typed-buffer value over common BulkData for selected scalar/struct formats | At least two concrete consumers require element metadata without per-element editing | Codec identity, canonical bytes, comparison, migration, tooling summary, and bounds pass for selected formats | Proposed |
| 4. Consumer migration program | Milestones 2-3 as required per consumer | Separate bounded plans for texture, mesh, terrain, animation, collision, or other dense sources | Consumer has measured package/memory/planning cost and a frozen compatibility baseline | Selected consumers no longer retain oversized ordinary Arrays; DDC/Cook/runtime bytes remain compatible or versioned | Proposed |
| 5. Unified lifecycle diagnostics and repair | Milestone 2 plus two migrated producers | Cross-provider inspection, audit, repair, provenance tracing, and orphan cleanup | Common API exposes stable provider/domain identity from real producers | Tools trace every payload through authored, derived, and cooked states without backend-specific caller logic | Proposed |
| 6. Async residency and memory budgets | Milestones 2 and 5 | Common async request/cancel/publish API, residency accounting, eviction policy, and optional mapping | Synchronous profiling shows startup or peak-residency pressure | Stress tests prove bounded memory, cancellation safety, unload/reload, stale-result rejection, and deterministic fallback | Evidence-gated |
| 7. Compression, deduplication, and virtualization | Milestone 5, optionally 6 | Provider-selected compression and local/remote content-addressed storage | Corpus telemetry demonstrates material storage or transfer savings | Recovery, source-control, patching, security, and offline workflows pass without weakening authored durability | Optional |

## Child Plan Boundaries

| Child plan | Owns | Must not own |
| --- | --- | --- |
| [Authored Asset Bulk Data Foundation](../Plans/AuthoredAssetBulkDataFoundation.md) | Authored bulk value, descriptor, local companion transaction, synchronous residency, Blob-to-bulk volume migration | Generic typed vector codecs, async streaming, global consumer conversion, remote storage |
| Unified BulkData API (selected next) | Common logical descriptor/value, immutable access, residency/failure semantics, provider contract, authored facade migration, one cooked adapter | Merging DABK and DBLK wires, changing DDC durability, async budgets, or broad consumer migration |
| Portable Typed Atomic Buffers (proposed) | Codec identity, canonical scalar/record encoding, reflected atomic typed values over BulkData, editor summaries | Physical payload placement or package transaction |
| Asset Payload Consumer Migrations (one plan per bounded domain) | Domain schema versions, exact byte compatibility, authoring and Cook workflow | Redefining common bulk APIs for one producer |
| Payload Lifecycle Inspection and Repair (proposed) | Audit graph, diagnostics, orphan detection, repair and cleanup tooling | Runtime streaming policy |
| Asset Payload Residency and Streaming (evidence-gated) | Async requests, cancellation, priorities, budgets, eviction and mapping on the common API | Authored/cooked format reinvention |

Unified BulkData API is selected as the next child-plan boundary because its
entry gate is satisfied. Its implementation plan is created only when work is
started; the roadmap change does not authorize refactoring the landed authored
or cooked formats by itself.

## Program Validation Matrix

| Concern | Required program evidence |
| --- | --- |
| Semantic stability | Ordinary Arrays never change schema or override behavior because of size; atomic/bulk declarations are explicit. |
| Unified API | Authored, derived, and cooked consumers observe one logical descriptor, immutable byte view, residency/failure model, and request contract without learning backend paths or offsets. |
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
- A unified API can accidentally unify storage authority. Provider capabilities
  keep authored replacement, DDC regeneration, and cooked immutability distinct;
  common callers receive no generic write lock or cross-domain publish method.
- A lowest-common-denominator API can hide useful Cook or DDC behavior. The
  common surface freezes identity, access, state, and requests only; typed
  provider metadata and domain services remain available behind explicit
  capability queries rather than leaking into every consumer.
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
  providers, bounds, diagnostics, and domain-owned transactional publication.
- Authored and cooked payloads use one consumer-facing BulkData identity,
  immutable access, residency, failure, and request API without merging their
  physical formats or durability rules.
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
- `Engine/Source/Runtime/Core/Public/Serialization/SharedByteBuffer.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/Cook.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
