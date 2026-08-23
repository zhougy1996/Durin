# Domain-Owned Large Asset Payload Architecture Roadmap

Summary: Reframe large asset payloads as domain-owned schemas over untyped bulk bytes with authority-specific storage, IO, and runtime resource lifecycles.

Last reviewed: 2026-08-23

Status: Active
Completed:

## Current Status

The authored bulk-data foundation, common `FBulkData` experiment, compatibility
retirement, and shared DABK/DBLK container mechanics are implemented and
validated. They proved immutable shared bytes, bounded structural planning,
transactional authored companions, deterministic cooked companions, corruption
handling, and a production VolumeTexture source workflow.

The program direction has changed after reviewing the domain-owned pattern used
by Unreal Engine. A single persistent logical descriptor should not own both
payload semantics and storage/residency policy across authored, derived, and
cooked domains. Payload meaning belongs to texture, mesh, animation, collision,
or another owning schema. The generic bulk layer owns opaque bytes and their
physical availability only. Derived-cache records, cooked runtime payloads, and
authored source remain separate lifecycle products even when they reuse byte
buffers and bounded container primitives.

Consequently, the former Portable Typed Atomic Buffers milestone is cancelled.
There will be no generic reflected typed-buffer value or registry intended to
persist arbitrary C++ element types. Domain codecs may expose compile-time typed
views or decoded arrays, but the durable contract is the owning domain schema
plus explicitly encoded bytes.

**Bulk Payload Layer Realignment** is complete. Common bulk data now owns only
opaque verified immutable bytes plus storage-neutral identity; the unproven
cross-authority provider/residency API and cooked descriptor adapter are gone.
DAST, DABK, DBLK, Cook manifest, and reflected package wires remain unchanged,
and focused, aggregate, full-build, and documentation gates pass.

**VolumeTexture Domain Payload Pilot** is complete. Source schema v1,
dimensions, format, and exact byte count now form the domain-owned authored
contract; TXPL and its reflected cooked descriptor remain the cooked contract.
Generic authored and Archive APIs no longer expose payload format/schema.
Historical semantic slots remain readable and current DAST/DABK output writes
canonical zeros without changing physical layouts. Focused workflow,
aggregate, full-build, and documentation gates pass.

The next ordered child is **Authority-Specific Payload Services**. Its entry
gate is satisfied by the authored/derived/cooked overlap and divergence exposed
by the pilot. No child plan is active yet.

## Outcome

Provide a durable large-payload architecture with three deliberately separate
concerns:

1. Asset domains own semantic metadata, payload schema versions, canonical
   codecs, validation, and decoded or runtime representations.
2. A small untyped bulk-byte layer owns immutable byte storage, byte counts,
   integrity, placement, residency, and IO without interpreting elements.
3. Authored source, derived cache entries, cooked deployment payloads, and GPU
   resources retain distinct authorities, mutation rules, and lifetimes.

This architecture keeps multi-million-element payloads atomic to reflection and
structural planners without inventing one universal persistent element schema.
It permits inline, companion, cache, container, or later virtualized placement
without forcing those backends into one semantic descriptor or transaction
model.

## Reference Model

The non-normative design reference is Unreal Engine's separation between:

- `FBulkData`/`FEditorBulkData` for opaque payload storage and availability;
- optional C++ typed access such as `TBulkData<T>` without treating native
  memory layout as a universal asset schema;
- domain metadata such as texture dimensions, source format, mip layout, or
  vertex precision outside the bulk byte owner;
- domain serializers and versioning for portable or platform-specific bytes;
- authored virtualization, derived data, cooked package IO, and runtime render
  resources as different lifecycle layers.

Durin follows these boundaries rather than copying Unreal names, wire formats,
or legacy APIs.

## Scope

- Refactor the current `FBulkData` experiment into a storage-only opaque byte
  abstraction; exact public names are selected by the first child plan.
- Remove semantic format identity and schema versioning from generic storage
  descriptors and place them in domain-owned source/cooked metadata.
- Retain one atomic reflected authored-byte property where useful, but give it
  no authority to define texture, mesh, animation, or other element meaning.
- Let authored, derived, and cooked owners select their own descriptor,
  mutation, publication, rebuild, and failure contracts.
- Preserve bounded structural planning: payload bytes and elements do not
  become per-element reflection nodes.
- Define canonical codecs and explicit schema versions per migrated consumer.
- Keep physical placement independent from domain semantics, including inline
  versus external authored data and loose versus packaged cooked data.
- Preserve or version existing DAST, DABK, DBLK, DDC, Cook, and asset bytes
  through explicit compatibility decisions.
- Separate serialized payloads from decoded CPU structures, build products,
  and RHI resources.
- Add synchronous correctness first, then evidence-driven async IO, range
  loading, residency budgets, and authored virtualization.
- Migrate dense consumers through bounded domain plans with measured evidence.

## Non-Goals

- A generic reflected `TTypedBulkData<T>` or `FTypedAtomicBuffer` persistent
  schema.
- Automatic conversion of `std::vector<T>` based on field name or element
  count.
- Persisting `sizeof(T)`, native struct layout, padding, pointers, allocator
  state, GLM layout, or platform endianness as an implicit contract.
- Teaching the storage layer about pixels, vertices, indices, keyframes,
  collision records, or other domain semantics.
- Requiring authored DABK, DDC records, cooked DBLK, and future package
  containers to expose one provider interface or share one wire format.
- Treating authored source, derived output, cooked deployment bytes, and GPU
  buffers as equal merely because their content hashes or sizes match.
- Providing generic mutable locks on package-owned payloads.
- Building remote virtualization, global deduplication, compression, or patch
  delivery before ownership, compatibility, and recovery are stable.
- Broad consumer conversion in one change.

## Program Decisions and Invariants

- **Domains own meaning.** The owning texture, mesh, terrain, animation,
  collision, or other type stores dimensions, counts, formats, strides,
  channel/layout choices, semantic version, and any relationships required to
  interpret its payload.
- **Bulk storage is untyped.** A generic bulk value knows bytes, size,
  integrity, placement/availability, and IO state. It does not carry a semantic
  `FormatId`, codec registry key, component count, or element schema.
- **Storage identity is not type identity.** Payload ids and content hashes may
  locate, deduplicate, or verify bytes; they do not prove that two domains can
  interpret those bytes the same way.
- **No universal typed-buffer serialization.** A thin typed view is permitted
  for local C++ ergonomics only when its owner has already validated a named
  domain format. Such a view never serializes arbitrary `T` by ABI layout and
  never becomes the reflected persistent contract.
- **Domain codecs freeze durable bytes.** Every persistent scalar/record layout
  defines byte order, component encoding, packing, bounds, malformed-input
  behavior, and schema version in its owning module. Platform-specific cooked
  layouts state that fact explicitly.
- **Structural and byte budgets are separate.** A payload contributes one
  logical property to reflection/planning, while encoded bytes, decoded bytes,
  element counts, allocations, ranges, and residency remain independently
  bounded.
- **Authorities stay separate.** Authored data is irreplaceable source and
  changes through asset publication; derived data is cacheable and rebuildable;
  cooked data is immutable manifest-owned deployment output. Common byte
  helpers do not grant cross-domain mutation.
- **A universal provider is not required.** Authored package loading, DDC
  lookup, cooked container IO, and future streaming may use authority-specific
  services. Common IO request primitives should be extracted only from at
  least two proven callers and must not erase their failure policies.
- **Physical mechanics may be shared.** Bounded binary readers/writers, checked
  layout arithmetic, range validation, hashing, immutable buffers, and async IO
  handles may be reused without merging descriptors, files, or transactions.
- **Placement is storage policy.** Inline versus external, loose versus
  containerized, compressed versus uncompressed, and local versus virtualized
  placement never changes a domain schema.
- **Metadata and payload publish together.** A domain object is valid only when
  its schema metadata and referenced bytes agree. Cross-file publication must
  never expose metadata that names absent or unverified bytes.
- **Decode is transactional and bounded.** Invalid counts, formats, offsets,
  hashes, compression ratios, or cross-record references fail before changing
  a live object or runtime resource.
- **Runtime resources are downstream products.** CPU decoded data, RHI buffers,
  textures, and streaming pages own their own lifetime and rebuild/release
  policy; the serialized bulk owner is not a GPU resource abstraction.
- **Compatibility is explicit, not accidental.** Each migrated domain selects
  current-only resave, versioned migration, byte preservation, or an explicit
  unsupported baseline based on the tracked corpus and release requirements.
- **Existing experiments are disposable.** `FBulkDataDescriptor`,
  `EBulkDataStorageDomain`, `IBulkDataProvider`, `FAuthoredBulkData`, and
  `CreateCookedPackageBulkData` may be renamed, split, narrowed, or removed.
  Landed behavior and compatibility evidence matter; current API shapes do not.
- **Synchronous correctness precedes streaming.** Async requests, range IO,
  cancellation, eviction, and memory budgets land only after domain ownership
  and synchronous failure behavior are stable.

## Current Foundations and Gaps

| Area | Reusable foundation | Required realignment or gap |
| --- | --- | --- |
| Byte ownership | Core `FSharedByteBuffer` provides immutable shared bytes. | Add only storage-oriented ownership/access operations proven by callers. |
| Archive and reflection | Bulk transfer is observable and authored payloads contribute one atomic node. | Reflection must not imply a generic element schema; domain metadata remains separately reflected. |
| Authored storage | DAST descriptors, DABK companions, publication, recovery, relocation, deletion, and inspection are proven. | Remove duplicated semantic format/version from the generic byte holder; preserve authored lifecycle behavior through migration. |
| Cooked storage | DBLK descriptors, hashes, manifest publication, compression metadata, and runtime failure behavior are proven. | Retire the provider-neutral cooked adapter if it duplicates DBLK ownership; consumers should decode through domain metadata and cooked storage services. |
| Container mechanics | DABK/DBLK share bounded IO, checked layout construction, hashing, and range validation. | Keep the private mechanics while allowing each wire and descriptor to evolve independently. |
| VolumeTexture source | Width, height, depth, format, import metadata, and authored voxel bytes already form a domain wrapper. | Make this wrapper the sole authority for voxel meaning; remove generic source format ids and validate metadata-plus-bytes together. |
| VolumeTexture cooked | Domain platform data and a schema-versioned cooked descriptor exist. | Remove the synthetic common `FBulkDataDescriptor` translation and retain an explicit storage-to-domain decode boundary. |
| Derived data | Deterministic keys and domain-validated payloads already exist. | Keep DDC behind builder/domain APIs; do not add a common BulkData adapter without a distinct proven benefit. |
| Residency and IO | The common experiment proves immutable resident bytes and synchronous verified loading. | Decide which state belongs in a storage byte owner versus an authority service; async requests and budgets remain absent. |
| Consumer breadth | Textures, meshes, terrain, animation, and collision contain dense data. | Each needs measured cost, a frozen domain schema, and its own migration decision. |
| Diagnostics | Authored inspection and cooked validation expose physical facts. | Domain-qualified summaries must join schema metadata to storage state without inventing a universal element registry. |

### Default Disposition of Landed Components

| Component | Default disposition | Reason |
| --- | --- | --- |
| `FSharedByteBuffer` and Archive bulk operation | Keep | They express immutable bytes and bounded transfer without domain semantics. |
| Private bulk-container reader/writer/layout utilities | Keep | They safely share physical mechanics without sharing lifecycle authority. |
| DABK transactional publication and recovery behavior | Keep behavior; wire may version | Authored durability remains required. |
| DBLK and Cook manifest publication behavior | Keep behavior; wire may version | Cooked deployment remains a separate consistency unit. |
| `FBulkDataDescriptor::FormatId/FormatVersion` | Remove from generic storage | Semantic identity and schema version belong to the owning domain. |
| `FBulkDataDescriptor::StoredByteCount` | Move to physical storage metadata where applicable | Compression and stored size describe placement, not logical payload meaning. |
| `EBulkDataStorageDomain` on the common value | Remove or confine to diagnostics | The authority owner already determines authored, derived, or cooked policy. |
| Public `IBulkDataProvider` common to all domains | Retire unless the first child proves a narrower IO role | A common provider currently hides materially different ownership and recovery rules. |
| `FAuthoredBulkData` | Refactor into an untyped authored-byte holder or replace | It may own authored bytes and placement, but not consumer format semantics. |
| `CreateCookedPackageBulkData` | Retire | It synthesizes a second descriptor instead of preserving the cooked storage/domain boundary. |
| Generic Portable Typed Atomic Buffers | Cancel | Domain wrappers and codecs solve the requirement without a universal persistent type system. |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 1. Bulk payload layer realignment | Landed authored/common/cooked experiments | Storage-only byte owner and descriptor boundaries; removal of generic semantic fields and unjustified cross-domain provider APIs; frozen migration baseline | Current API/corpus inventory exists and incompatible experimental APIs are allowed to change | Focused and aggregate tests prove unchanged authored/cooked payload behavior, bounded planning, transactional failure, and explicit disposition of every compatibility route | Completed |
| 2. VolumeTexture domain-schema pilot | Milestone 1 | Volume source and cooked metadata become the sole authority for voxel/pixel meaning; storage supplies opaque verified bytes only | Current VolumeTexture authored, DDC, Cook, runtime, reimport, and failure fixtures are green | Source save/reload and Cook/runtime round trips validate metadata plus bytes, preserve or explicitly version golden outputs, and contain no generic format translation | Completed |
| 3. Authority-specific payload services | Milestones 1-2 | Final authored, DDC, and cooked service boundaries with shared mechanics only where demonstrated | The pilot exposes the exact overlap and divergence between source, cache, and deployment paths | Authored replacement/recovery, DDC miss/rebuild, and cooked hard-failure behavior are independently testable with no universal mutation or semantic descriptor | Proposed |
| 4. Consumer migration program | Milestones 1 and 3; domain prerequisites as needed | Separate bounded plans for texture, mesh, terrain, animation, collision, or other dense sources | A consumer has measured structural/package/memory cost and a frozen compatibility baseline | Selected consumer uses domain metadata plus opaque storage, with canonical or explicitly platform-specific codecs and no oversized ordinary reflected arrays | Proposed |
| 5. Domain-qualified inspection and repair | Milestone 3 plus two production consumers | Inspection joins domain schema/version with storage location, integrity, provenance, and actionable repair/cleanup | Two consumers demonstrate reusable diagnostic questions | Tools trace authored, derived, and cooked payloads without a universal element registry or backend paths in domain callers | Proposed |
| 6. Async IO and residency budgets | Milestones 3 and 5 | Request/cancel/wait primitives, range IO where layouts support it, accounting, eviction, and stale-result rejection | Profiling shows startup, latency, or peak-residency pressure and two authorities share a real request shape | Stress tests prove bounded memory, cancellation safety, unload/reload, priority behavior, transactional publication, and deterministic synchronous fallback | Evidence-gated |
| 7. Authored payload virtualization | Milestones 3 and 5; optionally 6 | Content-addressed external authored storage with local/shared retrieval and durable fallback | Corpus and source-control telemetry show material checkout/storage cost | Offline, source-control, recovery, permission, cache-miss, and provenance workflows retain authored durability | Evidence-gated |
| 8. Compression, deduplication, and package-container evolution | Milestone 5; optionally 6-7 | Authority-selected storage optimization and possible archive/IoStore-style cooked aggregation | Telemetry demonstrates material storage, patch, or transfer savings | Compatibility, recovery, security, staging, patching, and performance gates pass without changing domain schemas | Optional |

## Child Plan Boundaries

| Child plan | State | Owns | Must not own |
| --- | --- | --- | --- |
| [Authored Asset Bulk Data Foundation](../Plans/Archive/2026-08/AuthoredAssetBulkDataFoundation.md) | Completed historical evidence | Immutable bytes, authored companion transactions, initial VolumeTexture source migration | Future semantic/storage boundaries |
| [Unified BulkData API](../Plans/Archive/2026-08/UnifiedBulkDataAPI.md) | Completed experiment, architecture superseded | Evidence for common immutable access and synchronous residency | A requirement to preserve its descriptor/provider API |
| [BulkData Compatibility Retirement](../Plans/Archive/2026-08/BulkDataCompatibilityRetirement.md) | Completed historical evidence | Corpus canonicalization and removal of obsolete aliases/routes | Protection of experimental common APIs from refactor |
| [Bulk Container Infrastructure](../Plans/Archive/2026-08/BulkContainerInfrastructure.md) | Completed reusable foundation | Private bounded IO, layout, hashing, and range validation | Shared schema, suffix, provider, or transaction authority |
| [Bulk Payload Layer Realignment](../Plans/BulkPayloadLayerRealignment.md) | Completed | Current API/caller inventory, storage-only target, compatibility matrix, removal/migration of common semantic/provider APIs | Broad consumer migration, async streaming, or physical optimization |
| [VolumeTexture Domain Payload Pilot](../Plans/VolumeTextureDomainPayloadPilot.md) | Completed | Source/cooked schema ownership, canonical voxel bytes, storage boundary, exact compatibility decision | Generic typed-buffer registry or redesign for unrelated consumers |
| Authority-Specific Payload Services | Proposed | Authored mutation, DDC rebuild, Cook publication/load boundaries and justified shared mechanics | Merging their durability or fallback policies |
| Asset Payload Consumer Migration: `<Domain>` | Proposed per consumer | One domain's metadata, codecs, compatibility, tooling summary, and end-to-end workflow | Changing the generic layer solely for local convenience |
| Domain Payload Inspection and Repair | Proposed | Cross-lifecycle diagnostics, orphan detection, provenance, repair and cleanup orchestration | Owning domain codecs or runtime streaming policy |
| Payload IO and Residency | Evidence-gated | Async requests, cancellation, priorities, range IO, budgets, eviction, and mapping | Domain schema design or authored/cooked format unification |
| Authored Payload Virtualization | Evidence-gated | External content-addressed source storage and durable retrieval policy | DDC/cooked authority merger or required remote-only operation |

Completed plans above remain provenance for verified behavior and validation.
When their architectural decisions conflict with this roadmap, this active
roadmap governs new work and the realignment plan must record the replacement.

## Program Validation Matrix

| Concern | Required program evidence |
| --- | --- |
| Boundary ownership | Generic storage headers contain no texture/mesh/animation semantics or persistent arbitrary-element schema; domain metadata contains everything needed to interpret bytes. |
| Storage neutrality | Inline/external or loose/container placement changes do not alter domain schema or decoded results. |
| Canonical domain bytes | Golden vectors cover byte order, packing, enum/format rules, NaN policy where relevant, and malformed input for each portable codec. |
| Platform-specific bytes | Cook target/profile and schema explicitly qualify any non-portable representation; no authored source silently depends on native layout. |
| Structural planning | Multi-million-element payloads contribute bounded logical nodes while byte, element, allocation, decoded-size, and residency ceilings still reject excess. |
| Authored durability | Save, crash-window recovery, move, copy, rename, delete, source-control checkout, and canonical resave retain the exact reachable payload set. |
| Derived data | Cache keys include every domain/build input; misses rebuild safely and corrupt entries never mutate a live object. |
| Cooked deployment | Manifests contain every required payload; runtime has no source/DDC fallback; target, schema, ranges, compression, and hashes validate before decode. |
| Compatibility | Every tracked historical representation has a tested migration/resave or an explicit unsupported-baseline decision. |
| Runtime resource separation | Serialized storage, decoded CPU state, and GPU resources can be independently released/rebuilt without dangling views or stale publication. |
| Residency | Sync and later async load, cancellation, eviction, unload, and stale completion expose neither partial bytes nor retired objects. |
| Diagnostics | Inspection reports the owning domain, domain schema/version, logical sizes/counts, integrity, placement, availability, and actionable failure. |
| Aggregate | Focused tests, full native aggregate, Debug Editor build, corpus workflows, documentation validation, and editor smokes remain green as required by each child plan. |

## Risks and Control Gates

- Removing generic format ids can reduce low-level inspection quality. Domain
  inspection hooks must provide schema-qualified summaries before generic
  fields disappear from user-facing tools.
- A storage-only abstraction can become too weak and cause duplicated unsafe IO.
  Shared bounded primitives remain available, and a common request API may be
  extracted after two real authorities demonstrate the same contract.
- Domain-owned codecs can duplicate scalar encoding. Duplication is tolerated
  until two stable consumers share identical durable rules; any later helper
  centralizes codecs, not a generic reflected typed-buffer value.
- Refactoring already-landed descriptors can strand authored companions or
  cooked assets. Milestone 1 freezes the tracked corpus, exact golden bytes,
  and rollback behavior before changing wires or metadata.
- Metadata and payload can drift when they serialize separately. Every domain
  validates expected counts/sizes and hashes transactionally, and publication
  treats metadata plus reachable payloads as one consistency unit.
- Thin typed views can accidentally bless ABI layout. They are created only
  after codec validation and cannot serialize, resize, or reinterpret unknown
  durable bytes by themselves.
- Removing the universal provider can leak physical paths into consumers.
  Authority services continue to resolve locations; domain callers receive
  opaque byte results or requests, never construct companions or offsets.
- Authored and cooked code paths may diverge enough to hide regressions.
  Domain-level end-to-end fixtures compare source, DDC, Cook, reload, and
  runtime results without requiring their storage descriptors to match.
- Async or virtualization work can obscure basic correctness. Their entry gates
  require stable synchronous behavior, diagnostics, and measurements.
- Converting all dense producers together would make regressions untraceable.
  Every consumer remains a separate plan with exact before/after evidence.

## Completion Criteria

- Generic bulk storage owns opaque bytes, integrity, availability, placement,
  and IO only; it owns no cross-domain element semantics or schema registry.
- Every migrated dense asset owns explicit domain metadata, codec/version,
  validation, decoded representation, and runtime resource handoff.
- Authored, derived, and cooked payloads retain stable and independently tested
  authorities, failure policies, and publication rules.
- Ordinary reflected arrays remain element-addressable and are never silently
  converted by size; dense opaque payloads remain structurally atomic.
- No persistent path relies on arbitrary native C++ layout without a named,
  explicitly qualified domain codec.
- Current experimental common semantic/provider APIs are removed or narrowed
  to roles justified by at least two production callers.
- Every supported historical representation has a tested migration or explicit
  unsupported-baseline decision.
- Required milestones pass their exit gates; evidence-gated and optional work
  is completed or dispositioned from measurements.
- Lasting storage, asset-domain, Cook, and runtime-resource contracts live in
  their owning documentation rather than only in child plans.

## Related Documentation

- [Serialization](../Runtime/Core/Serialization.md)
- [Generated Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)
- [Testing](../Agents/Testing.md)
- [Build and Run](../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/Core/Public/Serialization/SharedByteBuffer.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/Cook.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
