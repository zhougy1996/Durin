# Authored Asset Bulk Data Foundation Plan

Summary: Add atomic authored bulk-payload ownership, descriptors, transactional companion storage, and one production migration.

Last reviewed: 2026-08-22

Status: Archived
Completed: 2026-08-22

## Current Status

Completed on 2026-08-22. Core now owns immutable shared byte buffers and the
bounded Archive bulk transfer, while AssetCore owns `FAuthoredBulkData`, its
distinct reflection/default-delta identity, and deterministic DAST bulk opcode.
The version-1 `DABK` companion uses `.dabulk`, 16-byte alignment, a 256 KiB
external threshold, strong hashes, strict 1 GiB/65,536-entry bounds, and
generation-qualified names. Ordinary and bundle save, load, catalog inspection,
relocation, deletion, reimport rollback, and stale-generation cleanup include
the companion lifecycle.

Volume source custom version 2 stores normalized voxels in authored bulk data
and retains both historical Array and Blob migration routes. Missing and corrupt
companions fail before object publication and remain retryable. The production
`16384 x 128` PNG workflow passes import, save, reload, reimport, and Cook with
exact 128-cubed R8 source bytes. Its editable package measured 1,479 bytes and
its 2,097,152-byte source occupied a 2,097,312-byte companion (160 bytes of
framing/alignment), replacing the former multi-megabyte DAST value.

Qualification passed the DHT reflection suites (90 tests), focused Archive,
CoreObject, AssetPackage, and Texture suites (9, 79, 100, and 80 tests), the
native `test all` aggregate (77 targets), and the full Win64 Debug Editor build.
Lasting contracts were published in the owning runtime documents. The parent
roadmap records milestone 1 complete and keeps portable typed atomic buffers
behind its two-consumer entry gate.

## Goal

Add a stable, atomic authored bulk-data value whose descriptor remains in the
object package while its verified bytes may be inline or in a local companion.
Make save, load, move, delete, compatibility migration, and failure recovery
transactional, then prove the contract by moving normalized volume-texture
source bytes out of ordinary DAST values without changing import, DDC, TXPL,
Cook, or runtime texture results.

## Scope

- A Core byte-owner/residency substrate and an AssetCore-owned authored bulk
  value with explicit descriptor and synchronous access.
- Reflection, Archive, logical identity, snapshots, duplication, default
  planning, authored override, and Details summary behavior for that value.
- A bounded, versioned local authored companion format and deterministic
  inline/external placement policy.
- Transactional save/load/publication and recovery across `.dasset`, companion,
  catalog, move/copy/rename/delete, and unknown-field retention.
- Inspection diagnostics for payload id, format/version, logical/stored bytes,
  hash, placement, residency, and failure.
- Versioned migration of `FVolumeTextureSourceData::Voxels` from the current
  inline Blob into authored bulk data, retaining the older Array compatibility
  route.
- Focused Core, CoreDObject, AssetCore, TextureBuild, AssetForge, Cook, editor,
  fault-injection, corpus, aggregate, and documentation qualification.

## Non-Goals

- Generic typed atomic arrays for `float`, `FVector`, `FColor`, indices, mesh
  records, or arbitrary trivially copyable C++ types.
- Asynchronous IO, priorities, cancellation, eviction, memory budgets, memory
  mapping, remote virtualization, global deduplication, encryption, or payload
  compression.
- Migrating Texture2D, TextureCube, StaticMesh, SkeletalMesh, animation,
  terrain, collision, or cloud consumers in this plan.
- Redesigning cooked DBLK framing, Cook manifests, DDC storage, TXPL, runtime
  texture formats, or GPU upload paths.
- Automatically externalizing ordinary Arrays or changing their schema based
  on byte count.
- Storing object references or dependency discovery inside opaque bulk bytes.
- Per-byte Details editing, partial Blob patches, or indexed authored override
  paths within the payload.

## Design Decisions and Invariants

### Ownership and layering

- Core owns an immutable/shared byte allocation and bounded Archive operation;
  it has no asset path, package, catalog, or filesystem policy.
- AssetCore owns the authored bulk value, descriptor, physical placement,
  companion IO, package transaction, inspection, and recovery. Engine owns the
  volume-source format and validation.
- The public asset value is explicit rather than an annotated
  `std::vector<T>`. Ordinary Arrays and the existing byte Blob retain their
  current meanings.
- Bulk bytes contain no implicit object references. Every durable dependency
  remains a normal reflected/package field visible without loading the payload.

### Descriptor and identity

- Stage 0 freezes public names, but the descriptor must include a stable
  payload id, semantic format id and version, logical byte count, stored byte
  count, content hash, and storage kind. Physical absolute paths and vector
  capacity are never persistent identity.
- Logical equality includes format/version, size, and exact content. A verified
  strong hash may avoid loading or comparing resident bytes, but collision or
  unverified metadata cannot make different content identical.
- Copy and duplication share immutable payload storage where safe; mutation
  creates a new candidate and publishes it atomically. A bulk value never
  exposes writable resident memory without an explicit replacement transaction.
- The reflected/default-delta representation is one atomic logical value with
  field-level explicit/forced provenance. Structural node limits count it once;
  independent byte and allocation limits remain authoritative.

### Placement and format

- Logical schema is independent of placement. A deterministic authoring policy
  may select inline or external storage using declared producer policy and size,
  but crossing the threshold never changes property kind or compatibility
  identity.
- Cooked `.dbulk` remains manifest-owned output. Stage 0 selects and freezes a
  distinct authored companion suffix, magic, version, descriptor table,
  alignment, limits, and source-control/ignore policy before implementation.
- Companion entries sort by stable payload id and validate unique ids, bounded
  offsets, non-overlap, alignment, sizes, hashes, complete consumption, and no
  trailing data. Native struct memory is never a payload format.
- DAST stores the bulk descriptor and any admitted inline bytes; external bytes
  are not duplicated into the Value section. Unknown retained values preserve
  external reachability or fail closed before canonical resave.

### Transaction and failure behavior

- Saving writes and validates candidate companion bytes before publishing a
  package that references them. The previous package and payload set remain
  readable until the new package is committed. A crash may leave an orphan
  candidate, never a published dangling reference.
- Package save, load, move, copy, rename, delete, reimport, and source repair
  operate on one logical package/payload transaction. Cleanup only removes
  payloads proven unreachable after successful publication.
- Loading validates descriptor, location, size, and hash into detached storage
  before replacing the live value. Missing, truncated, corrupt, excessive, or
  stale payloads preserve the prior live object and resource.
- The first plan provides explicit synchronous states at least equivalent to
  `Unloaded`, `Resident`, and `Failed`; it does not conceal blocking IO behind
  an unqualified raw-data accessor.
- Diagnostics preserve package path, property path, payload id, storage kind,
  expected/observed size or hash, and the applicable limit without exposing
  unsafe physical paths to ordinary runtime callers.

### Volume compatibility

- A new volume-source custom-version step migrates current Blob `Voxels` into
  bulk storage; the existing historical `Array<UInt8>` route remains load-only.
  Conversion occurs in detached state and validates dimensions, format, and
  exact byte count before commit.
- Current saves emit only the bulk representation and current version. A
  canonical resave does not retain deprecated Array or Blob payload fields.
- Normalized source bytes, import provenance, DDC key inputs, mip chains, TXPL,
  Cook output, and upload bytes remain exact. Only authored physical placement
  changes.

## Current Foundations and Gaps

| Area | Existing foundation | Selected gap |
| --- | --- | --- |
| Byte ownership | Blob provides owned contiguous bytes and transactional Archive loading. | No shared immutable payload/residency abstraction. |
| Reflection and planning | Blob is one logical `Bytes` node and ordinary Arrays remain element-addressable. | No bulk property kind or descriptor-only external value. |
| Archive | `BulkData` purpose and `Inline`/`Skip`/`External` policy are defined. | No semantic operation or adapter contract implements the policy. |
| Authored package | DAST v4 is bounded, versioned, deterministic, and transactional for one file. | No authored companion, cross-file commit, or retained-payload reachability. |
| Cook | DBLK descriptors, hashes, companion publication, manifests, and load validation are production-owned. | Cooked ownership cannot be reused unchanged for editable authored data. |
| Asset mutation | Packages support catalog publication, relocation, deletion, and source transactions. | Companion lifecycle is not part of those transactions. |
| Volume source | Blob migration and real `128^3` lifecycle regression are complete. | The 2 MiB normalized source remains inline and always resident with the package. |

## Implementation Stages

### Stage 0: Freeze authored bulk semantics and wire boundaries

- [x] Audit every Archive bulk-policy branch, cooked DBLK invariant, package
  publication/recovery path, unknown-field retention path, asset mutation, and
  large authored field; record exact reuse and separation boundaries.
- [x] Freeze the Core byte owner and AssetCore bulk value/descriptor API,
  residency states, copy/mutation rules, logical identity, reflection kind,
  authored intent, Details presentation, and module dependencies.
- [x] Select and freeze authored inline/external policy, companion suffix,
  magic/version, table layout, alignment, byte/count/depth ceilings, content
  hash, and canonical ordering without changing cooked `.dbulk` semantics.
- [x] Freeze the cross-file publication, crash windows, orphan policy, move,
  copy, rename, delete, source-control, and cleanup algorithms.
- [x] Freeze the volume custom-version step, deprecated Blob route, conversion
  order, supported corpus baseline, canonical-resave evidence, and rollback.
- [x] Add failing contract fixtures for inline/external boundary placement,
  malformed descriptors, missing/truncated/corrupt companions, crash windows,
  unknown retained bulk fields, duplicate ids, limit rejection, and current plus
  historical `128^3` volume packages.

#### Acceptance Gate

- Public ownership, wire bytes, placement, limits, transaction order,
  compatibility, recovery, and the production regression are explicit; failing
  fixtures demonstrate the current absence without modifying package limits.

### Stage 1: Add the atomic authored bulk value

- [x] Implement the Core immutable/shared byte owner and bounded synchronous
  transfer needed by inline and external payload adapters.
- [x] Implement the AssetCore bulk value and descriptor with explicit residency,
  verified identity, detached replacement, copy/duplication, and diagnostics.
- [x] Add generated/runtime reflection metadata, one atomic logical value,
  snapshots, editable copy, duplication, default/no-delta planning, authored
  field intent, GC neutrality, and read-only Details summary.
- [x] Make Archive `Inline`, `Skip`, and `External` policies observable through
  one semantic operation; reject unsupported policy/capability combinations
  before mutation.
- [x] Prove empty/nonempty values, shared copies, copy-on-replacement, exact
  identity, hash verification, unsupported references, all Archive purposes,
  limit failures, and transactional rollback.

#### Acceptance Gate

- One explicit bulk value has stable reflection and process-local semantics,
  contributes one planner node regardless of payload size, and cannot expose
  partial, unverified, or implicitly blocking bytes.

### Stage 2: Publish and load authored companion payloads

- [x] Implement the frozen authored companion reader/writer with independent
  frozen bounds, deterministic directory ordering, and complete validation.
- [x] Extend DAST discovery/emission/load adapters to retain bulk descriptors,
  inline admitted values, external references, custom versions, unknown fields,
  and identical manifests without duplicating external bytes.
- [x] Integrate candidate companion creation, validation, package commit,
  rollback, orphan tolerance, and post-commit cleanup into ordinary and bundle
  saves.
- [x] Integrate catalog inspection, load, unload, move, copy, rename, delete,
  redirect, source repair, canonical resave, and compatibility audit with the
  payload set.
- [x] Add fault injection at every write, flush, rename, validation, catalog,
  and cleanup boundary; prove the last published package/payload set remains
  usable and recovery is idempotent.

#### Acceptance Gate

- Authored packages may reference verified local bulk bytes without exceeding
  DAST value limits, and every package mutation either commits the complete new
  payload set or retains the complete old set.

### Stage 3: Migrate volume source as the production proof

- [x] Replace current inline Blob storage for normalized volume voxels with the
  authored bulk value while retaining dimensions, format, and provenance as
  ordinary structured fields.
- [x] Add the new versioned Blob-to-bulk route behind the existing
  Array-to-Blob history; load both supported historical shapes transactionally
  and resave only the current descriptor/payload representation.
- [x] Preserve exact normalized bytes, DDC keys, mip chains, derived payloads,
  TXPL, cooked DBLK, GPU upload, reimport, source repair, and last-known-good
  failure behavior.
- [x] Qualify inline/external policy boundaries plus horizontal, vertical, and
  compact atlas layouts; retain the real `16384 x 128` PNG as the mandatory
  import/save/reload/reimport/Cook regression.
- [x] Audit the supported content corpus and record migrated counts, rejected
  schemas, payload sizes, package-size reductions, and deterministic resave
  hashes without deleting compatibility routes.

#### Acceptance Gate

- Current and supported historical volume assets retain exact behavior while
  normalized source bytes are owned by the authored bulk transaction rather
  than an oversized DAST value.

### Stage 4: Qualify and publish the foundation

- [x] Run focused Core, CoreDObject, AssetCore package/mutation, TextureBuild,
  AssetForge, Cook, editor property, volume/cloud, and fault-injection targets
  under the repository testing workflow.
- [x] Run native aggregate, full Debug Editor build, documentation validation,
  corpus compatibility/canonical-resave audit, and a validation-enabled editor
  import/save/move/reopen/reimport/Cook smoke.
- [x] Measure package bytes, payload bytes, save/load allocations, synchronous
  access latency, and retained residency for empty, boundary, `128^3` R8, and
  `128^3` RGBA8 inputs; record bounded evidence without using timing as a
  correctness gate.
- [x] Publish lasting Core Archive/buffer, reflection, AssetCore authored-bulk,
  mutation/recovery, and volume authoring contracts in their owning documents.
- [x] Update the parent roadmap with landed APIs, compatibility evidence,
  remaining gaps, and the entry-gate decision for Portable Typed Atomic Buffers.

#### Acceptance Gate

- Focused, fault, corpus, aggregate, build, documentation, and real-editor gates
  pass with no dangling payload, Array-schema regression, silent blocking
  accessor, or change to DDC/Cook/runtime bytes.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Type distinction | Array, Blob, and authored bulk retain explicit stable identities independent of size | DHT/CoreDObject tests |
| Value lifecycle | Empty/resident/unloaded/failed bulk values copy, compare, duplicate, replace, and destroy safely | Core/AssetCore tests |
| Archive policy | Inline, Skip, and External perform the frozen behavior or fail before mutation | Core/adapter tests |
| Structural planning | Multi-megabyte bulk contributes one field in enabled and no-delta modes | Default-delta tests |
| Companion wire | Golden bytes, canonical order, bounds, offsets, hashes, and complete consumption are deterministic | AssetCore format tests |
| Cross-file transaction | Every injected failure retains the prior readable package/payload pair; recovery tolerates orphans | Asset mutation fault tests |
| Unknown fields | Retention keeps external reachability or rejects resave before losing data | DAST compatibility tests |
| Asset operations | Save, bundle save, load, unload, move, copy, rename, delete, redirect, and repair include payload lifecycle | AssetCore integration tests |
| Compatibility | Historical Array and Blob volume packages load and canonical-resave only current bulk data | Corpus fixtures/audit |
| Volume identity | Import, DDC, mip, TXPL, Cook, upload, and reimport bytes remain exact | Engine/TextureBuild/RHI tests |
| Real workflow | `16384 x 128` PNG completes import/save/reload/reimport/Cook and cloud assignment | AssetForge/editor integration |
| Diagnostics | Every failure reports owner/property/payload, expected/observed facts, and limit | AssetCore/editor tests |
| Aggregate | Focused tests, native aggregate, full build, docs, corpus, and editor smoke stay green | Repository validation |

## Definition of Done

- Durin has one explicit authored bulk value with stable descriptor, atomic
  reflection/default planning, bounded synchronous residency, and no implicit
  object references.
- A versioned authored companion stores external payloads deterministically and
  validates all counts, offsets, extents, sizes, hashes, and trailing bytes.
- Package and companion publication, recovery, mutation, and cleanup preserve a
  complete last-known-good payload set across every injected failure.
- Supported historical volume Array/Blob packages migrate transactionally;
  current volume source uses authored bulk without changing DDC, Cook, TXPL, or
  runtime output.
- The production `128^3` workflow and all focused/aggregate/editor gates pass.
- Lasting contracts are documented and the parent roadmap records whether the
  typed-buffer milestone is ready to activate.

## Deferred Follow-ups

- Portable typed atomic codecs and values for scalar, vector, color, index, and
  domain record arrays.
- Migration of non-volume texture, mesh, terrain, animation, and collision
  authored data.
- Async requests, priorities, cancellation, eviction, memory budgets, mapping,
  compression, deduplication, virtualization, and remote payload services.
- Cooked descriptor/container convergence where measurements show useful reuse.

## Related Documentation

- [Large Asset Payload Architecture Roadmap](../../../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Generated Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Reflected Byte Blob Serialization Plan](ReflectedByteBlobSerialization.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/Core/Private/Serialization/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/Cook.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetMutationTransaction.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Editor/AssetForge/Private/VolumeTextureSourceTranslation.cpp`
- `Engine/Tests/Native/AssetCoreTests`
- `Engine/Tests/Native/EngineTests/Private/Texture`
