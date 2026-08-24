# Bulk Container Infrastructure Plan

Summary: Extract a shared bounded binary container foundation for DABK and DBLK while preserving domain-specific formats and lifecycle ownership

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

AssetCore now owns one private `BulkContainerInfrastructure.h` foundation for
bounded little-endian IO, checked arithmetic/alignment, detached ordering,
canonical layout construction, zero-padding validation, and safe range
projection. DABK, DBLK, and CMNF use those primitives while retaining their
existing schemas, diagnostics, providers, suffixes, and transaction owners.

Golden gates lock the focused DABK fixture at 274 bytes with XXH3-128 words
`3311620820794941896`/`17520128536900976125`, DBLK at 259 bytes with words
`12417320302211656157`/`3049470508272984121`, and CMNF at 137 bytes with words
`1127403949174504654`/`9302219320893799974`. The prior cooked-local generic
codec, alignment helper, padding loops, and range loops have been removed.

## Goal

Create one small AssetCore-private binary-container foundation that makes
bounded little-endian IO, checked arithmetic/alignment, deterministic directory
construction, hashing, padding, and payload-range validation reusable and hard
to misuse. Migrate DABK and DBLK to that foundation without changing either
format, suffix, public API, ownership model, or authored/Cook transaction.

The outcome is common mechanism with domain-specific policy: DABK remains an
authored, source-controlled companion and DBLK remains a target-specific,
manifest-owned Cook product.

## Scope

- Freeze byte-for-byte DABK v1, DBLK v1, and adjacent CMNF v1 golden outputs
  before refactoring their implementations.
- Add an internal bounded little-endian reader/writer with latched failure,
  checked cursor arithmetic, explicit byte limits, zero-padding operations, and
  detached output publication.
- Add checked `uint64` addition, multiplication, and power-of-two alignment
  helpers that never wrap and never narrow to `size_t` before qualification.
- Add a neutral payload-layout model and validator for sorted unique directory
  keys, aligned non-overlapping ranges, zero padding, exact file consumption,
  and caller-selected bounds.
- Add reusable hashing helpers for exact byte ranges and directory/table bytes;
  algorithms and the location of stored hashes remain format-owned.
- Migrate DABK construction/parsing and DBLK construction/parsing independently,
  retaining domain-owned headers, entries, validation policy, and diagnostics.
- Migrate CMNF's use of the duplicate `CookedAsset.cpp` reader/writer when that
  is necessary to retire the old generic codec; CMNF schema behavior is not
  otherwise redesigned.
- Add focused internal contract tests plus existing authored package, Cook,
  corruption, deterministic-byte, and integration coverage.
- Update lasting serialization and asset lifecycle documentation with the
  shared-mechanism/domain-policy boundary.

## Non-Goals

- Renaming `.dabulk` to `.dbulk`, merging DABK and DBLK, or introducing a new
  container version.
- Changing any magic, header size, entry size/order, endian rule, alignment,
  maximum count/size, hash algorithm, padding, or canonical ordering rule.
- Moving authored data into Cook manifests or treating cooked data as authored
  source; source-control, relocation, deletion, staging, and deployment
  ownership remain separate.
- Replacing `FCanonicalMemoryReader/Writer` as the repository-wide semantic
  Archive implementation. The new codec is a narrow physical-container tool.
- Generalizing every binary format in AssetCore, Engine, DDC, texture, mesh, or
  shader code in this plan.
- Exposing the new codec publicly or letting Engine modules depend on its
  concrete types.
- Adding asynchronous IO, mapping, compression, deduplication, virtualization,
  or a shared authored/cooked container cache.
- Changing `FBulkData`, providers, DAST/DABK publication ordering, DBLK/manifest
  publication, or current eager authored package loading.
- Rewording stable public diagnostics without a concrete consistency or safety
  benefit; diagnostic normalization is secondary to behavioral compatibility.

## Design Decisions and Invariants

- The shared implementation belongs in `AssetCore/Private`, because DABK,
  DBLK, and CMNF are AssetCore physical formats. Core remains independent of
  asset domains and container policy.
- The abstraction stops below headers and entries. It may know bytes, cursors,
  bounds, alignment, ranges, padding, hashes, and opaque sortable keys; it must
  not know DABK/DBLK magic, authored descriptors, Cook targets/profiles,
  compression enums, manifests, package paths, or suffixes.
- Reader state is a non-owning span plus cursor and first failure. Reads validate
  the complete requested range before copying or advancing. A failed read never
  partially mutates its output or makes later operations appear successful.
- Writer state owns a detached candidate buffer, a configured maximum size, and
  first failure. Checked reserve/append/pad operations cannot publish partial
  output to a caller; domain writers commit only after complete self-validation.
- Persistent integers are explicitly little-endian. Supported primitives are a
  deliberately small set of unsigned fixed-width integers, GUID/hash words, and
  exact byte spans; no native struct layout, implicit `sizeof(record)`, pointer,
  allocator, or host-endian serialization is admitted.
- Checked arithmetic precedes allocation and narrowing. `TryAdd`, `TryMultiply`,
  and `TryAlignUp` accept explicit bounds; alignment is nonzero and a power of
  two. Callers cannot obtain a wrapped offset or silently truncated size.
- Directory construction does not mutate caller-owned payload order. Domain
  code selects the stable key; the shared helper sorts a detached projection
  and rejects duplicate or non-strict keys before offsets are assigned.
- Layout validation operates on normalized `{offset, stored size, alignment}`
  ranges plus policy. It proves that directory/header bytes end before payload
  data, ranges are aligned and ordered, padding is zero, ranges do not overlap
  or escape the file, and the format-specific trailing-byte policy holds.
- DABK retains fixed 16-byte alignment, strict no-gap canonical layout,
  XXH3-128 payload hashes, generation container hash, 65,536-entry limit, and
  1 GiB container/payload bound.
- DBLK retains per-entry 16..4096 power-of-two alignment, table XXH64,
  XXH3-128 payload hashes, target/profile/compression metadata, 64-entry limit,
  8 GiB payload bound, and 64 GiB container bound.
- CMNF remains a Cook manifest rather than a bulk container. It may consume the
  shared codec primitives but not the payload-layout abstraction unless its
  existing wire actually requires them.
- Existing valid bytes must be byte-for-byte identical. Existing malformed
  inputs must still fail before allocation/publication; tightening a previously
  accidental acceptance requires an explicit test, compatibility assessment,
  and plan update rather than being hidden in the refactor.
- Shared failures are structured internal categories and offsets. DABK/DBLK
  adapters retain domain context and own user-facing messages, so the low-level
  layer does not leak generic diagnostics into package or Cook APIs.
- No compatibility shim or parallel legacy implementation remains after both
  formats migrate. The migration proceeds one format at a time so each stage
  remains reviewable and bisectable.

## Current Foundations and Gaps

| Area | Current foundation | Gap addressed by this plan |
| --- | --- | --- |
| Consumer API | `FBulkData` already unifies identity, immutable bytes, residency, failure, and synchronous load. | Physical container safety remains duplicated below the provider boundary. |
| DABK | Canonical Archive primitives, fixed alignment, sorted payload ids, XXH3-128 validation, strict bounds. | Local unchecked `Align` and format-inline layout logic are not reusable or independently specified. |
| DBLK | Private little-endian codec, checked alignment, sorted ids, table/payload hashes, strong corruption tests. | A second generic reader/writer and layout implementation can drift from DABK. |
| CMNF | Shares the private cooked codec and has deterministic manifest tests. | Retiring the cooked-local generic codec must not duplicate it again for manifests. |
| Arithmetic | DBLK checks alignment overflow; both formats enforce overall limits. | Addition, multiplication, narrowing, and alignment policy are not expressed through one audited API. |
| Ranges | Both readers reject overlap, invalid padding, bounds errors, and trailing data. | Equivalent invariants are encoded in separate loops and diagnostics. |
| Golden bytes | DABK round-trip/determinism and DBLK fixture/cook tests exist. | Refactor gates need explicit pre/post byte hashes and direct shared-primitive adversarial tests. |
| Ownership | Authored save/relocate/delete and Cook manifest/publication are transactional and distinct. | A shared mechanism must be prevented from erasing this boundary. |

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory every local binary reader/writer, alignment helper, directory
  sort, hash, padding loop, and range check used by DABK, DBLK, and CMNF.
- [x] Capture canonical DABK, DBLK, and CMNF fixture bytes plus exact hashes,
  sizes, header/entry constants, and accepted bounds.
- [x] Map malformed-input coverage against truncation, overflow, duplicate
  keys, disorder, invalid alignment, overlap, gaps, nonzero padding, wrong hash,
  and trailing bytes.
- [x] Freeze the private API boundary, structured failure model, ownership of
  user-facing diagnostics, and allocation/publication guarantees.
- [x] Confirm that no public header, suffix, format version, provider, or
  lifecycle transaction needs to change.

#### Acceptance Gate

- The duplication inventory and golden-wire baseline are complete; the shared
  layer has a narrow responsibility and every valid/invalid compatibility gate
  is explicit before production code moves.

### Stage 1: Introduce the bounded binary codec

Depends on Stage 0.

- [x] Add AssetCore-private reader/writer types with explicit maximum size,
  little-endian fixed-width primitives, exact byte spans, cursor queries, zero
  padding, latched first failure, and detached result transfer.
- [x] Add checked add/multiply/align/narrow helpers and reject invalid or
  overflowing operations before allocation or cursor mutation.
- [x] Add a dedicated `AssetBulkContainerTests` contract target, or an equally
  isolated focused target, covering every primitive at zero, boundary, and
  overflow values.
- [x] Prove truncated reads and failed writes leave caller-visible destinations
  unchanged and never expose partially valid output.
- [x] Keep production DABK/DBLK paths unchanged in this stage; the new layer is
  independently reviewable before migration.

#### Acceptance Gate

- The codec has adversarial unit coverage, no asset-domain concepts, no public
  exposure, and no production wire behavior has changed.

### Stage 2: Introduce canonical directory and layout validation

Depends on Stage 1.

- [x] Add detached stable sorting and strict-unique-key validation suitable for
  GUID payload ids without embedding GUID or descriptor policy in the layout
  layer.
- [x] Add normalized range construction with checked aligned offset assignment
  and explicit per-format maximum count, payload, and container bounds.
- [x] Add validation for directory/data separation, monotonic aligned ranges,
  overlap, canonical gaps, zero padding, exact/trailing consumption, and safe
  byte-span projection.
- [x] Add exact-range and table hashing helpers while leaving algorithm choice
  and serialized hash fields with each format adapter.
- [x] Exercise mixed alignments, empty payload policy, maximum counts, one-byte
  truncations, near-`uint64` arithmetic, aliasing ranges, reordered entries, and
  nonzero padding.

#### Acceptance Gate

- One audited layout engine expresses the shared invariants without assuming
  DABK or DBLK header/entry schemas, and its policy surface cannot weaken a
  format's existing bounds implicitly.

### Stage 3: Migrate DABK without changing bytes

Depends on Stages 1-2.

- [x] Rebuild DABK encoding on the shared writer, checked arithmetic, detached
  ordering, offset assignment, padding, and hashing helpers.
- [x] Rebuild DABK parsing on the shared reader and layout validator while
  retaining authored descriptor equality, container-hash checks, and current
  domain diagnostics.
- [x] Remove DABK-local alignment/range mechanics that become unreachable; do
  not leave forwarding compatibility wrappers.
- [x] Compare every canonical DABK output with the Stage 0 bytes and hashes and
  rerun inline/external save, load, corruption, relocation, deletion, bundle,
  recovery, and reimport coverage.

#### Acceptance Gate

- DABK v1 output is byte-for-byte identical, malformed-input behavior remains
  fail-closed, authored transactions are unchanged, and DABK owns only schema
  and lifecycle policy above the shared foundation.

### Stage 4: Migrate DBLK and retire duplicate cooked codecs

Depends on Stage 3.

- [x] Rebuild DBLK encoding/decoding on the shared primitives and layout engine,
  retaining target/profile, compression, per-entry alignment, table hash,
  payload hash, bounds, and exact-descriptor rules.
- [x] Migrate CMNF encoding/decoding to the shared bounded codec where required
  to remove `CookedAsset.cpp`'s duplicate generic reader/writer; keep manifest
  schema and publication behavior unchanged.
- [x] Remove the cooked-local generic reader/writer, alignment, padding, and
  range utilities once no production or test caller remains.
- [x] Compare canonical DBLK and CMNF outputs with Stage 0 fixtures and hashes;
  exercise all malformed fixture categories and target/profile mismatches.
- [x] Run real texture, static mesh, skeletal, animation, material, terrain, and
  environment Cook consumers that rely on DBLK.

#### Acceptance Gate

- DBLK v1 and CMNF v1 are byte-for-byte identical, all cooked consumers pass,
  and AssetCore contains one generic bounded codec/layout implementation rather
  than parallel authored and cooked variants.

### Stage 5: Document and qualify the architecture

Depends on Stages 0-4.

- [x] Update serialization, asset package/lifecycle, and Large Asset Payload
  documentation to distinguish shared physical mechanism from domain-owned
  formats, suffixes, providers, and transactions.
- [x] Run focused codec, AssetPackage, AssetCook, import/relocation, and Cook
  integration targets, then `fast-all` and the full native aggregate because a
  shared AssetCore primitive changed.
- [x] Run the Win64 Debug Editor `all` build and final asset audit/baseline.
- [x] Run changed/all documentation, plan, and roadmap validators.
- [x] Confirm source inventory has no duplicate DABK/DBLK reader/writer,
  alignment, or range implementation and no accidental public dependency.

#### Acceptance Gate

- All qualification is green, golden wires are unchanged, the shared layer is
  private and minimal, and lasting contracts accurately describe both reuse and
  retained authored/Cook separation.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Wire compatibility | Pre/post DABK v1, DBLK v1, and CMNF v1 bytes, sizes, and hashes match exactly. |
| Bounded IO | Zero/boundary/overflow/truncation tests prove no cursor wrap, narrowing, overread, partial destination mutation, or partial output publication. |
| Canonical output | Detached sorting, strict unique keys, alignment, zero padding, and exact file consumption produce deterministic bytes from reordered inputs. |
| Range safety | Header overlap, range overlap, gaps, out-of-file ranges, invalid alignment, nonzero padding, and trailing bytes fail before allocation/publication. |
| Integrity | Existing table, payload, content, and container hash algorithms and failure behavior remain exact. |
| Authored lifecycle | Inline/external save/load, bundle publication, relocation, deletion, recovery, reimport, and stale companion cleanup pass unchanged. |
| Cook lifecycle | Target/profile qualification, DBLK/manifest publication, cleanup, runtime lookup, and all production payload codecs pass unchanged. |
| Architecture | Shared code contains no DABK/DBLK magic, descriptors, paths, suffixes, target/profile, manifest, or provider policy; domain adapters contain no duplicate generic codec. |
| Performance | Parsing remains allocation-bounded; the refactor introduces no per-byte heap allocation or extra whole-container copy on the production read path. |
| Aggregate | Focused targets, full native aggregate, Debug Editor build, asset corpus, and documentation lifecycle validation pass. |

## Definition of Done

- DABK, DBLK, and CMNF valid bytes are unchanged and all existing assets/Cook
  outputs remain readable without migration or compatibility branches.
- AssetCore has one private bounded binary codec and one reusable canonical
  payload-layout validator with direct adversarial coverage.
- Authored and cooked adapters retain only their schema, metadata, diagnostics,
  and lifecycle responsibilities.
- Duplicate reader/writer, checked alignment, directory ordering, padding, hash
  range, and payload-range mechanics are removed rather than wrapped.
- `.dabulk` and `.dbulk`, their source-control/deployment policies, and their
  DABK/DBLK identities remain intentionally distinct.
- Full validation and documentation gates pass and the plan records exact
  golden-wire evidence.

## Completion Evidence

- `AssetBulkContainerTests`, focused DABK, and `AssetCookTests` passed before
  aggregate qualification; the contract target covers arithmetic boundaries,
  invalid alignment, latched failures, unchanged destinations, detached
  publication, sorting/duplicate rejection, mixed layouts, padding, overlap,
  trailing bytes, and unsafe range projection.
- `fast-all` and the full native `all` aggregate passed on
  `Win64-Debug-DurinEditor`, including the production Cook consumers selected
  by the plan. The Win64 Debug Editor `all` build also passed.
- The read-only Sandbox audit reported 32/32 compatible packages with zero
  incompatible, unsupported, failed, or stale records; baseline reported 32
  current DAST v4 packages.
- Changed/all documentation plus all plan and roadmap lifecycle validators
  passed. Source inventory found no DABK/DBLK-local generic reader/writer,
  alignment helper, or hand-written padding/range loop.

## Deferred Follow-ups

- Add explicit `.dabulk` Git LFS policy and storage documentation before the
  first authored companion is committed; keep this separate from container-code
  refactoring so storage normalization is reviewable.
- A shared cooked-container provider/cache for remaining low-level DBLK callers.
- A real deferred authored DABK provider tied to package lifetime,
  cancellation, and memory budgets.
- A later IoStore/Zen-style content-addressed backend that may host authored,
  derived, and cooked provider payloads without merging their authorities.
- Compression, mapping, deduplication, virtualization, async requests, and
  memory budgets after measurements establish their entry gates.
- Broader adoption of the bounded codec by other formats only through separate
  evidence-backed plans; this implementation must not become a mandatory
  repository-wide serialization framework by accident.

## Related Documentation

- [Large Asset Payload Architecture Roadmap](../../../Roadmaps/Archive/2026-08/LargeAssetPayloadArchitecture.md)
- [Unified BulkData API](UnifiedBulkDataAPI.md)
- [Authored Asset Bulk Data Foundation](AuthoredAssetBulkDataFoundation.md)
- [BulkData Compatibility Retirement](BulkDataCompatibilityRetirement.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AuthoredBulkStorage.cpp`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/BulkData.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Data/CookedBulk/README.md`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureCookTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
