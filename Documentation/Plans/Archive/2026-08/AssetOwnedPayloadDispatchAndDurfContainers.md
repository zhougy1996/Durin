# Asset-Owned Payload Dispatch and DURF Containers Plan

Summary: Adopt DURF for DABK and DBLK while making asset-owned context the sole domain-payload dispatch authority and removing redundant payload magics.

Last reviewed: 2026-08-25

Status: Archived
Completed: 2026-08-25

## Current Status

All stages completed on 2026-08-25. DURF header version 1 now has exactly three
current asset-file branches: DAST v6 for `.dasset`, DABK v2 for authored
`.dabulk`, and DBLK v2 for generated `.dbulk`. Asset classes and reflected
payload slots exclusively select domain codecs; common storage has no
`PayloadTypeId`, byte probing, mutable codec registry, or nested DURF payload.

The frozen 27-package authored corpus contained two external companions. Both
were canonically resaved without changing their package bytes, descriptors,
payload bytes, or logical container identities. The resulting DABK v2 files
are `VT_Cloud_Base_Voronoi_128.dabulk` (2,097,344 bytes,
SHA-256 `8675d09f629f52c0fa5b1844c26451036a131b29ff09a3e6dd93181b417b70f4`)
and `VT_Cloud_Detail_Voronoi_64.dabulk` (262,336 bytes,
SHA-256 `384b1373cf161f214392194fe309430d14a5c14166f19af7973ed13632f0d7a0`).
Engine and Sandbox qualification inventories report all 27 packages ready,
both companions reachable, and zero orphan or legacy companions.

DABK and DBLK use 128-byte fixed fronts, 64-byte and 80-byte entries
respectively, bounded two-phase validation, and no payload copy during storage
validation. Focused validation passed AssetBulkContainerTests (11),
AssetCookTests (13), AssetPackageTests (123), TextureTests (86),
StaticMeshTests (74), SkeletalAssetTests (34), TerrainHeightmapTests (11),
TerrainHeightmapCookTests (1), EnvironmentLightingTests (3), and
TextureCookIntegrationTests (1). The complete Debug build and `fast-all`
aggregate passed 60/60 test targets. Changed/all documentation and plan
validation passed, and residual source search found no legacy container or
domain dispatch magic. The temporary DABK v1 conversion path and both legacy
container readers were removed after conversion.

## Goal

Finish the asset binary-format separation introduced by DAST v6 so independent
asset files share one durable DURF discovery and integrity layer, while payload
meaning remains owned by the asset that declares and consumes it. Replace DABK
v1 and DBLK v1 with DURF-based successors, remove redundant mnemonic type
checks from embedded domain payloads, cut over the complete authored baseline,
invalidate and rebuild generated data, and remove the obsolete readers and
temporary migration code in one bounded compatibility transition.

## Scope

- Assign permanent nonzero random `FormatId` constants and canonical debug
  names to the logical DABK and DBLK formats, alongside the existing permanent
  DAST identity.
- Define and implement DABK v2 and DBLK v2 under `DURF` header version 1 with
  format-owned headers, bounded canonical directories, exact file extents,
  deterministic hashing, and existing placement/alignment policies where they
  remain justified.
- Preserve DAST v6 as the package branch and update its external-payload
  transaction and Cook paths only where the new companion bytes, hashes, or
  format fingerprints require it.
- Keep `FBulkData`, authored storage descriptors, `FCookedPayloadDescriptor`,
  DABK entries, and DBLK entries free of domain codec identity. The owning
  reflected asset field and its code-selected payload slot choose the domain
  encoder and decoder.
- Remove the redundant four-byte magic from the current Durin-owned domain
  payload codecs: TXPL, DMSH, DCOL, DSKM, DANM, THPL, and IBLP. Retain and
  validate their domain schema, producer/builder, target/profile, topology,
  bounds, and integrity fields as applicable.
- Bump every affected domain schema, builder version, DDC key schema, Cook
  fingerprint, and compatibility fixture required to prevent old payload bytes
  from being interpreted by the new decoder.
- Add the smallest exact offline DABK v1-to-v2 converter for the frozen authored
  corpus. Convert each `.dasset`/`.dabulk` closure atomically, preserving
  payload bytes and logical descriptors while updating the companion hash and
  DAST Payload Directory facts.
- Treat DBLK, DDC values, and cooked packages as generated products: invalidate
  and rebuild them instead of introducing runtime migration.
- Remove DABK v1, DBLK v1, legacy domain-magic reader/writer branches, obsolete
  fixtures, and the temporary authored converter after baseline qualification.
- Promote lasting container, payload-dispatch, versioning, Cook, DDC, and
  recovery rules into the owning Runtime documentation at completion.

## Non-Goals

- Changing DAST v6 object, Import, Export, Value, or Payload Directory grammar,
  or assigning a new DAST `FormatId`.
- Adding a `PayloadTypeId`, payload codec GUID, global payload registry, dynamic
  payload plugin dispatcher, semantic type field, or type-erased decoder lookup
  to bulk descriptors or container entries.
- Treating DABK or DBLK as asset types. They remain physical storage formats;
  reflected classes remain asset-type authority.
- Wrapping a DBLK-embedded or DDC-owned domain payload in a nested DURF
  envelope. DDC producer/key context and the owning asset codec are sufficient
  dispatch authority.
- Reintroducing format or schema identity into `FBulkData`,
  `FEditorBulkData`, `FArchiveBulkDataTransfer`, or authored DABK entries.
- Changing the logical content of texture, mesh, collision, skeletal,
  animation, terrain, or environment-lighting payload schemas beyond the
  framing fields required by this cutover.
- Converting source imports, third-party formats, shader binaries, pipeline
  caches, asset-catalog caches, thumbnail stores, mutation journals, or the
  CMNF Cook manifest. Each is a separate authority and requires its own trigger.
- A long-lived dual-reader policy, transparent runtime migration, background
  editor resave, cross-version Cook fallback, or generic migration graph.
- Compression-policy expansion, streaming redesign, append-in-place
  publication, signing, encryption, or damaged-front-header recovery.

## Design Decisions and Invariants

- `DURF` is the common file family, not an asset or payload type. DAST, DABK,
  and DBLK are three sibling format branches selected by permanent `FormatId`
  values and independently versioned format-owned codecs.
- The three branches share only the Core-owned DURF preamble, registry
  validation, little-endian framing, checked extents, header integrity, and
  common diagnostics. DAST semantics stay in AssetCore's package codec; DABK
  and DBLK retain separate authored and cooked storage authority.
- Canonical debug names follow the permanent namespace
  `Durin.BinaryFormat.DAST`, `Durin.BinaryFormat.DABK`, and
  `Durin.BinaryFormat.DBLK`. Names are diagnostic; random checked-in GUIDs are
  wire identity and are never derived from names or four-character mnemonics.
- DABK v2 and DBLK v2 each use DURF header version 1 and their own format
  version. Their format headers retain every fact needed for construct-free
  bounds, directory, target/profile, layout, and whole-container validation;
  the DURF `HeaderHash` does not replace a required payload/container hash.
- Asset-owned dispatch is closed-world and explicit. A reflected asset class
  knows which field/slot owns a payload and calls that domain codec directly.
  `PayloadId` identifies one logical stored value; it is not a codec type and
  must not be used as one.
- Generic DABK/DBLK code validates storage facts before returning opaque bytes:
  identity, format version, file/header sizes, entry uniqueness and order,
  extents, alignment, compression, hashes, platform/profile, and allocation
  bounds. It never interprets texture, mesh, animation, or another domain
  schema.
- A domain decoder begins only after the owning asset has selected it. It
  validates its supported schema and all format-specific structural facts
  before publishing a candidate. Arbitrary cross-codec probing is not a
  supported dispatch mechanism and is not made authoritative by heuristic
  byte recognition.
- Domain payload bytes remain canonical and placement-independent: the same
  logical payload encoding may be stored as a DDC value or as a DBLK entry
  without acquiring storage-layer headers inside the payload.
- A DDC `.bin` is not a fourth DURF branch or a standalone semantic file
  format. `FDerivedDataObjectStore` remains an opaque namespaced
  content-addressed byte store; the build function/value contract and owning
  asset select the decoder for the raw cache value.
- DDC identity is producer-owned. Removing a payload magic requires an
  explicit schema/builder/key change so pre-cutover cache entries miss and
  rebuild; readers do not guess old versus new bytes.
- Cook identity is source-free and deterministic. DBLK v2 and current domain
  payload versions participate in the Cook/cache policy fingerprint, and old
  cooked output fails closed or is regenerated by the Cook pipeline.
- The DABK cutover is an authored closure migration. Companion bytes publish
  before the matching DAST v6 package, rollback restores the complete prior
  closure, and no package may reference a v1 or hash-mismatched companion after
  success.
- Unknown DURF format identities, unsupported DABK/DBLK versions, malformed
  headers, stale schemas, old DDC keys, and wrong target/profile values fail
  before object, CPU-resource, or GPU-resource publication.
- The ordinary DABK and DBLK writers switch only after detached successor
  codecs and affected domain codecs have passed their focused gates. Audit,
  catalog discovery, and editor startup remain read-only throughout cutover.
- Legacy readers and the DABK converter exist only inside this plan and are
  deleted after corpus conversion and project qualification. The repository
  does not retain a mixed current baseline.

## Current Foundations and Gaps

| Area | Current foundation | Gap closed by this plan |
| --- | --- | --- |
| Common envelope | Core owns the qualified 64-byte DURF v1 preamble, immutable format registry, bounded parsing/finalization, diagnostics, golden bytes, and mutation coverage. | DABK and DBLK do not yet register permanent identities or consume the common envelope. |
| Package branch | DAST v6 is the sole package reader/writer and records exact external payload descriptors in its front Payload Directory. | Companion transactions and fingerprints still expect DABK v1 bytes. |
| Authored bulk | DABK v1 has bounded sorted entries, whole-container hashing, stable sibling publication, backup recovery, and only two tracked companions. Historical semantic identity/version slots are already ignored and current output writes zero. | Its standalone file still begins with `DABK`, has no permanent DURF identity, and requires an authored closure migration. |
| Cooked bulk | DBLK v1 bounds entries and files, validates platform/profile, hashes the table and each payload, and publishes deterministic Cook companions. | Its standalone file still begins with `DBLK`; generated baselines and policy fingerprints need a clean rebuild cutover. |
| Payload dispatch | Texture, mesh, collision, skeletal, animation, terrain, and lighting owners already select their codecs and validate reflected descriptors before decode. | Domain payload bytes repeat TXPL/DMSH/DCOL/DSKM/DANM/THPL/IBLP magic even though no generic dispatcher consumes it. |
| DDC and Cook | Builder/schema/key versions already separate generated-data compatibility domains; DBLK and DDC consumers decode detached candidates transactionally. | Every affected version/key and fixture must change together so old magic-bearing bytes cannot enter new readers. |
| Tests and tools | AssetCore and Engine have focused bulk, Cook, package, texture, static-mesh, skeletal, terrain, environment-lighting, baseline, and failure-atomic coverage. | No independent DABK/DBLK DURF golden model, frozen cross-domain payload inventory, exact DABK closure conversion, or zero-legacy-magic gate exists. |

## Frozen Successor Wire Contracts

All integers are little-endian. `DURF` offsets below are absolute file offsets;
format-header offsets are relative to byte 64, immediately after the common
preamble. The DURF `HeaderBytes` extent includes the complete directory and
its zero alignment padding, so `HeaderHash` authenticates all storage metadata
before any payload range is trusted.

### DABK v2

- Identity: `49efbbb4-e2434e35-a7c01c34-9ed84ea0`, diagnostic name
  `Durin.BinaryFormat.DABK`, format version 2, required features zero.
- Format header is 64 bytes: `HeaderSize:u32=64` at +0,
  `EntrySize:u32=64` at +4, `Flags:u32=0` at +8, `Reserved0:u32=0` at +12,
  `EntryCount:u64` at +16, `DirectoryOffset:u64=128` at +24,
  `DataOffset:u64` at +32, `ContainerHash:xxh128` at +40, and
  `Reserved1:u64=0` at +56.
- Each 64-byte entry is `PayloadId:guid` at +0, `LogicalBytes:u64` at +16,
  `StoredBytes:u64` at +24, `ContentHash:xxh128` at +32,
  `PayloadOffset:u64` at +48, `Flags:u32=0` at +56, and `Reserved:u32=0`
  at +60. Entries are strictly ascending by `PayloadId`.
- `DataOffset = align16(128 + EntryCount * 64)` and equals DURF
  `HeaderBytes`. Payloads use canonical gap-free 16-byte placement and exact
  file extent; all directory and payload padding is zero. `ContainerHash`
  remains the package-closure identity supplied by authored transaction code;
  each content hash covers exactly its stored payload bytes.

### DBLK v2

- Identity: `76c5d46c-a3744b7e-9cda6c8f-e0dbcd17`, diagnostic name
  `Durin.BinaryFormat.DBLK`, format version 2, required features zero.
- Format header is 64 bytes: `Platform:u32` at +0, `Profile:u32` at +4,
  `Flags:u32=0` at +8, `EntrySize:u32=80` at +12, `EntryCount:u32` at +16,
  `Reserved0:u32=0` at +20, `DirectoryOffset:u64=128` at +24,
  `DataOffset:u64` at +32, `DirectoryHash:xxh64` at +40,
  `Reserved1:u64=0` at +48, and `Reserved2:u64=0` at +56.
- The existing 80-byte entry grammar is retained byte-for-byte: payload id,
  flags, payload schema, target platform/profile, compression, alignment,
  offset, stored/uncompressed sizes, and 128-bit payload hash. Entries are
  strictly ascending by payload id and contain no codec identity.
- `DataOffset = align16(128 + EntryCount * 80)` and equals DURF
  `HeaderBytes`. `DirectoryHash` covers only the entry array; the DURF header
  hash additionally covers the format header and zero padding. Payload ranges
  retain declared power-of-two alignment (16..4096), exact hashes and bounded
  compression rules. The physical file size equals DURF `FileBytes`.

### Domain payload successors

TXPL, DMSH, DCOL, DSKM, DANM, THPL and IBLP keep their existing header sizes,
field offsets, semantic fields, tables, payload bytes, hashing and alignment.
Header word/field zero changes from mnemonic magic to required-zero reserved;
schema and every producer/key generation captured by the completed migration
advance. A nonzero reserved field is
corrupt, an unsupported schema is incompatible, and old bytes cannot alias a
new DDC or Cook identity. There is no replacement type field.

### Bounds and cost budgets

| Format | Header/directory budget | File and item bounds | Parse/allocation budget |
| --- | --- | --- | --- |
| DABK v2 | 128-byte fixed front; at most 4,194,432 bytes through aligned `HeaderBytes` | 65,536 entries; 1 GiB file/payload bound; 16-byte placement | two bounded linear directory/layout passes; no payload copy or domain allocation during validation; temporary metadata at most 16 MiB |
| DBLK v2 | 128-byte fixed front; at most 5,248 bytes through aligned `HeaderBytes` | 64 entries; 8 GiB per payload; 64 GiB file; 16..4096 alignment | two bounded linear directory/layout passes; no decompression/domain allocation during storage validation; temporary metadata at most 64 KiB |

Unknown identity/version/features, malformed or noncanonical extents, nonzero
reserved/padding, duplicates, overflow, gaps/overlaps, target mismatch,
unsupported compression, bad directory/header/payload hashes, truncation and
trailing bytes all reject before returning opaque payload spans.

## Implementation Stages

### Stage 0: Freeze identities, wire contracts, corpus, and budgets

- [x] Inventory every production and test producer/consumer of DABK, DBLK,
  TXPL, DMSH, DCOL, DSKM, DANM, THPL, and IBLP, including DDC keys, Cook
  descriptors, payload fixtures, diagnostics, and cache fingerprints.
- [x] Freeze the Git-tracked `.dasset`/`.dabulk` migration manifest with path,
  size, DAST format/version, DABK v1 prefix, package hash, companion hash,
  payload entry descriptors, and exact package-companion closure.
- [x] Measure current DABK/DBLK header, directory, file, entry-count, alignment,
  parse-cost, and allocation bounds; select documented successor budgets with
  explicit headroom.
- [x] Generate and check in permanent random nonzero DABK and DBLK `FormatId`
  constants with canonical debug names and registry collision tests.
- [x] Freeze exact DABK v2 and DBLK v2 little-endian format headers, directory
  entries, hashing rules, canonical ordering, required features, reserved
  fields, and rejection diagnostics under the existing DURF v1 preamble.
- [x] Freeze each domain payload successor header after removing its magic,
  including exact schema/builder/key bumps and confirmation that no semantic
  payload field changes.
- [x] Record accepted size and parse-cost budgets and the exact focused,
  aggregate, Engine baseline, and Sandbox baseline validation set.

#### Acceptance Gate

- The three-branch DURF model, asset-owned dispatch contract, successor golden
  layouts, complete producer/consumer inventory, authored migration closure,
  generated-data invalidation map, and quantitative budgets are explicit; no
  unresolved wire or ownership decision remains.

### Stage 1: Implement detached DABK v2 and DBLK v2 codecs

- [x] Extend the explicit immutable binary-format registry with the DABK and
  DBLK descriptors without adding mutable global registration or Core semantic
  knowledge.
- [x] Implement bounded two-phase DURF parsing and canonical finalization for
  detached DABK v2 and DBLK v2 byte buffers while leaving ordinary production
  writers on v1.
- [x] Retain DABK whole-container identity, stable entry ordering, payload-byte
  equality, and exact descriptor projection under the new format-owned header.
- [x] Retain DBLK target/profile, compression, alignment, entry ordering,
  per-payload hashes, exact file size, and opaque-payload extraction under the
  new format-owned header.
- [x] Add independent reference encoders/parsers and exact minimal/nontrivial
  golden bytes for both formats.
- [x] Add failure coverage for unknown format/version/features, duplicate
  registry identities/names, malformed/truncated headers, nonzero reserved
  fields, excessive counts/sizes, bad hashes, gaps, overlaps, alignment,
  overflow, trailing bytes, and wrong target/profile.
- [x] Measure successor front-header size and parse cost against the accepted
  Stage 0 budgets before enabling a production route.

#### Acceptance Gate

- Detached DABK v2 and DBLK v2 production bytes agree with independent golden
  models, reject the frozen corruption corpus deterministically, preserve
  current logical storage semantics, and remain unreachable from ordinary
  authored and Cook writers.

### Stage 2: Make domain payload decoding exclusively asset-owned

- [x] Remove TXPL, DMSH, DCOL, DSKM, DANM, THPL, and IBLP magic fields from
  successor payload encoders and parsers without adding replacement codec
  identities to common descriptors or DBLK entries.
- [x] Update each owning asset's save/build/Cook/load path to select exactly one
  codec from its reflected field/slot contract and to validate descriptor
  schema, platform/profile, payload id, size, and hash before decode.
- [x] Bump affected domain schemas, producer/build versions, DDC key schemas,
  Cook fingerprints, fixtures, and diagnostics; prove old DDC values miss and
  new values rebuild deterministically.
- [x] Preserve byte equality between a canonical domain payload stored in DDC
  and the same payload embedded in DBLK; add no nested DURF or storage header.
- [x] Prove wrong owner slot, stale descriptor schema, corrupt structure,
  excessive allocations, wrong target/profile, truncation, and trailing bytes
  fail before CPU/GPU or object-state publication.
- [x] Update construct-free inspection to report the owner-selected domain and
  schema without recognizing bytes by mnemonic magic.

#### Acceptance Gate

- Every in-scope payload codec has one magic-free canonical successor selected
  only by its asset owner, all generated-data identities are bumped, DDC and
  Cook payload bytes agree, and no generic storage layer interprets domain
  semantics.

### Stage 3: Cut over and convert authored DABK closures

- [x] Add a bounded offline converter that accepts only the frozen DABK v1
  source contract, emits canonical DABK v2, preserves every payload byte and
  logical entry fact, and reports the new companion hash plus required DAST v6
  descriptor updates.
- [x] Prove deterministic dry-run/apply output, refusal of non-v1 and corrupt
  input, exact payload comparison, package-plus-companion rollback, backup
  recovery, interrupted publication recovery, and idempotent already-current
  reporting.
- [x] Switch authored companion inspection, save, relocation, delete, recovery,
  canonical resave, and package transaction policy to DABK v2.
- [x] Convert the complete frozen `.dasset`/`.dabulk` manifest atomically and
  verify each package Payload Directory exactly matches its DURF/DABK v2
  companion.
- [x] Re-run Engine and Sandbox asset baselines and prove no tracked package
  points to a DABK v1 companion or stale container hash.

#### Acceptance Gate

- Every tracked authored closure is current DAST v6 plus DURF/DABK v2, payload
  bytes and logical asset values are unchanged, transaction/recovery behavior
  remains failure-atomic, and both project baselines pass.

### Stage 4: Cut over generated payloads and DBLK

- [x] Switch the ordinary Cook container writer and runtime reader to DURF/DBLK
  v2 and make legacy DBLK v1 fail closed before payload selection.
- [x] Invalidate affected DDC namespaces and Cook outputs, rebuild representative
  texture, cube, volume, static-mesh, collision, skeletal-mesh, animation,
  terrain, and environment-lighting products, and compare decoded logical data
  with the frozen baseline.
- [x] Prove source-free cooked package plus DBLK loading for every payload
  family, including multi-payload packages, optional collision, missing/corrupt
  companion failure, platform/profile mismatch, and last-known-good resource
  behavior where the owning subsystem defines it.
- [x] Verify Cook manifests, package descriptors, cache fingerprints,
  deterministic repeated Cook output, and package/container atomic publication
  all select the new formats and schemas.
- [x] Qualify Engine and Sandbox Cook/baseline workflows with no generated v1
  DBLK or legacy magic-bearing domain payload accepted as current.

#### Acceptance Gate

- All supported generated asset families rebuild through the new DDC identities,
  Cook emits only DURF/DBLK v2 containing magic-free owner-dispatched payloads,
  runtime source-free loads pass, and stale or corrupt generated data fails
  before publication.

### Stage 5: Remove transition code and promote lasting contracts

- [x] Delete DABK v1 and DBLK v1 readers/writers, the DABK converter, legacy
  domain-magic constants and branches, temporary manifest/report machinery,
  and obsolete fixtures/tests.
- [x] Prove production/source searches contain no reachable DABK/DBLK legacy
  file magic and no TXPL/DMSH/DCOL/DSKM/DANM/THPL/IBLP dispatch magic; allow
  mnemonic text only in diagnostics, historical documentation, or test labels.
- [x] Update Asset Packages, Asset Data Lifecycle and Storage, Versioning, Core
  Serialization, and Content Version Control with the implemented three-branch
  DURF model, asset-owned dispatch, migration, DDC, Cook, and recovery rules.
- [x] Run the selected focused tests, affected integration and runtime tests,
  Engine and Sandbox baselines, repository aggregates, and documentation
  validators following the repository build and testing workflows.
- [x] Record exact conversion counts, hashes, rebuilt payload families, tests,
  baselines, cost measurements, residual searches, and removal evidence in
  `Current Status`, then complete the plan only after every gate passes.

#### Acceptance Gate

- The repository supports and emits only DURF/DAST v6, DURF/DABK v2, and
  DURF/DBLK v2 for the three asset-file branches; payload dispatch is exclusively
  asset-owned; generated payloads contain no redundant dispatch magic; legacy
  and transition code is absent; lasting documentation and all selected
  validation are current.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| DURF family | Permanent unique DAST/DABK/DBLK identities and names, reverse registry order, unknown/duplicate/unsupported rejection, exact preamble/header bounds, and no semantic leakage into Core |
| DABK v2 | Independent goldens, canonical entries, payload-byte preservation, whole-container hash, bounds/alignment/corruption cases, exact descriptor projection, and deterministic repeated output |
| DBLK v2 | Independent goldens, target/profile, compression flags, canonical directory, payload hashes, gaps/overlap/trailing bytes, bounded allocation, and deterministic repeated output |
| Asset-owned dispatch | Each reflected owner/slot calls one explicit codec; generic descriptors contain no type identity; wrong slot/schema/target/profile and corrupt structure fail before publication |
| Domain payloads | TXPL, DMSH, DCOL, DSKM, DANM, THPL, and IBLP successor exact bytes, schema/builder bumps, old-byte rejection, round trips, bounds, corruption, and DDC/DBLK byte equality |
| Authored migration | Frozen closure manifest, dry-run report, v1 fingerprints, exact payload comparison, updated DAST container hashes, companion-first atomic publication, rollback/recovery, and zero residual tracked v1 |
| Generated cutover | DDC miss/rebuild, deterministic Cook, source-free runtime load, missing/corrupt/wrong-platform failures, cache fingerprint changes, and zero accepted legacy DBLK/payload bytes |
| Projects and lifecycle | Engine and Sandbox asset baselines, affected import/reimport/resave/relocate/delete/Cook workflows, multi-payload packages, last-known-good behavior, and clean editor/runtime qualification |
| Cost and robustness | Accepted DABK/DBLK header size and parse budgets, pathological counts/sizes, header and payload mutations, deterministic diagnostics, and failure-atomic state publication |
| Removal and docs | No reachable legacy codecs/converter/magic dispatch, owning Runtime contracts updated, changed/all documentation and plan validation, and exact completion evidence |

## Definition of Done

- `DURF` header version 1 has exactly three current asset-file format branches:
  permanent DAST, DABK, and DBLK identities with independently owned codecs and
  versions.
- DAST v6 remains the sole `.dasset` contract; every tracked external authored
  closure uses DABK v2 and passes exact package-companion validation.
- Cook emits only DBLK v2, all in-scope generated payloads use their selected
  magic-free schema, and DDC/Cook identities prevent old bytes from loading as
  current.
- Asset classes and reflected payload slots are the sole domain codec dispatch
  authority. No generic bulk descriptor, container entry, registry, or byte
  heuristic owns payload type.
- DABK/DBLK readers validate storage completely before returning opaque bytes,
  and domain decoders validate detached candidates completely before any live
  object, CPU resource, GPU resource, catalog, or package publication.
- Authored conversion is exact and failure-atomic; generated data is rebuilt;
  Engine and Sandbox baselines and all selected package/DDC/Cook/runtime tests
  pass.
- Legacy readers/writers, mnemonic payload dispatch, temporary conversion code,
  and obsolete fixtures are removed, and lasting documentation owns the final
  contract.

## Deferred Follow-ups

- DURF adoption for CMNF, asset-catalog/reference caches, thumbnail stores,
  import records, mutation journals, shader or pipeline caches, and other
  independently discoverable Durin binary files requires separate inventory
  and value evidence.
- A future plugin system that must decode unknown payload domains without
  loading their owning asset module may introduce an explicit plugin-owned
  dispatch contract. It must not preemptively add `PayloadTypeId` to the closed
  built-in asset path.
- Compression expansion, streamable sub-payloads, sparse/range Cook access,
  remote storage, signing, encryption, and append-in-place publication remain
  separately triggered format changes.
- New asset families follow the asset-owned dispatch contract and do not
  require a new DURF branch merely because they define a new domain payload
  schema.

## Related Documentation

- [Durin Binary Envelope and DAST v6](../../../Roadmaps/Archive/2026-08/DurinBinaryEnvelopeAndDastV6.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../../../Runtime/Assets/Versioning.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)
- [Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/BinaryEnvelope.h`
- `Engine/Source/Runtime/Core/Private/Serialization/BinaryEnvelope.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageVersionPolicy.h`
- `Engine/Source/Runtime/AssetCore/Private/BulkContainerInfrastructure.h`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Developer/AssetBuildCore/Private/DerivedDataObjectStore.h`
- `Engine/Source/Developer/AssetBuildCore/Private/DerivedDataObjectStore.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TexturePayloadContainer.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmapDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/EnvironmentLighting/EnvironmentLighting.h`
- `Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLighting.cpp`
- `Engine/Tests/Native/CoreTests/Private/BinaryFormatTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkContainerInfrastructureTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkDataTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV6Tests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureDerivedDataTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureCookTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAnimationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapCookTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EnvironmentLightingTests.cpp`
