# Durin Binary Envelope Foundation Plan

Summary: Freeze and implement the reusable DURF v1 preamble, explicit format registry, independent golden parser, and bounded validation required before DAST v6.

Last reviewed: 2026-08-25

Status: Active
Completed:

## Current Status

This is the active M1 child of the
[Durin Binary Envelope and DAST v6 roadmap](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md).
DAST v6 has been selected, but DAST v5 remains the only supported reader and
ordinary writer. No current format, tracked package, compatibility fixture, or
companion has changed.

Stage 0 is ready. It freezes the exact Core/AssetCore ownership boundary,
permanent DAST identity, two-phase header API, wire representation, and
caller-owned bounds before production code changes. The initial repository
inventory contains 30 tracked `.dasset` packages, two `.dabulk` companions,
and seven `.dasset.hex` fixtures; later cutover work must regenerate rather
than trust those counts.

## Goal

Provide a reusable, format-neutral `DURF` header-version-1 foundation that can
identify and validate bounded Durin binary front matter before codec dispatch,
then adapt AssetCore's package dispatch to understand the permanent DAST
identity without changing any DAST v5 bytes or selecting a DAST v6 writer.

## Scope

- Freeze the exact 64-byte `DURF` v1 little-endian preamble, GUID/hash wire
  order, required-feature behavior, physical/header extent checks, and
  zeroed-field XXH3-128 header hashing.
- Add Core-owned, format-neutral prefix parsing, complete front-header
  validation, header finalization, structured diagnostics, and caller-owned
  size limits.
- Add an explicit immutable format-descriptor registry/value view that rejects
  invalid and duplicate identities without global mutable state or static
  registration order.
- Allocate and freeze one permanent DAST `FormatId` and canonical debug name.
- Adapt AssetCore's codec key and preamble model so legacy DAST v5 maps to the
  same logical DAST identity and a future `DURF` DAST version can fail closed at
  the existing dispatch seam.
- Add an independent test-only encoder/parser, exact golden files or byte
  arrays, deterministic mutation/fuzz coverage, and measured bounded cost.
- Record the landed generic envelope contract in Core documentation and the
  still-legacy DAST boundary in asset documentation.

## Non-Goals

- Implementing the DAST v6 format-owned header, section directory, Public
  Summary, Import, Export, Payload Directory, or object-value mapping.
- Registering a readable or writable DAST v6 codec, changing the ordinary v5
  writer, or changing the supported authored-package baseline.
- Rewriting a `.dasset`, `.dasset.hex`, `.dabulk`, DDC object, cooked output,
  registry cache, or source asset.
- Converting DABK, DBLK, or any asset-family payload to `DURF`.
- Adding a process-global mutable registry, dynamic plugin load/unload
  lifecycle, manifest generator, authenticity, signing, encryption, or tail
  recovery.
- Selecting DAST-specific section/count limits; M2 owns those after measuring
  the real corpus. This plan provides the caller-owned limit mechanism.

## Design Decisions and Invariants

- The common magic is the literal byte sequence `DURF`; it identifies a Durin
  binary envelope and never an asset class, payload type, or schema.
- Header version 1 is exactly 64 bytes. Its fields and offsets are those
  selected in the investigation: magic at 0, `HeaderVersion` at 4,
  `PreambleBytes` at 6, `FormatId` at 8, `FormatVersion` at 24,
  `RequiredFeatures` at 28, `HeaderBytes` at 32, `FileBytes` at 40, and
  `HeaderHash` at 48.
- Fixed-width integers are little-endian. `FormatId` encodes `FGuid::A`, `B`,
  `C`, and `D` as four consecutive little-endian `uint32` values. `HeaderHash`
  encodes `FXxHash128::HashLow` then `HashHigh` as little-endian `uint64`
  values. Tests freeze this representation rather than relying on native
  struct layout.
- The DAST identity is one randomly generated, nonzero GUID constant checked
  into AssetCore with the permanent canonical debug name
  `Durin.BinaryFormat.DAST`. The name is diagnostic metadata and renaming code
  never reallocates the ID.
- Prefix parsing and complete header validation are separate operations. Prefix
  parsing consumes exactly the common preamble plus the independently known
  physical file size, performs no format-specific allocation, and returns the
  declared `HeaderBytes`. Complete validation receives the bounded contiguous
  front matter, resolves an explicit registry, validates version/features and
  the header hash, and returns immutable spans/values.
- Core does not read DAST sections or own codecs. A descriptor supplies a
  nonzero ID, nonempty unique debug name, supported format-version interval or
  set, supported required-feature mask, and caller-owned header/file limits.
  Registry creation rejects duplicate IDs or names and invalid descriptors.
- There is no default unlimited policy. Checked arithmetic precedes narrowing,
  slicing, hashing, or allocation; caller limits apply before a declared extent
  is trusted.
- `HeaderHash` covers `[0, HeaderBytes)` with bytes 48 through 63 treated as
  zero. Hash validation never authenticates content and never replaces a
  format's complete payload/section validation.
- Exact physical size is authoritative: `FileBytes` must equal the observed
  file size. Truncation and trailing bytes fail before codec dispatch.
- Output arguments and destination buffers change only on success. Diagnostics
  distinguish truncation, magic/header-version/preamble-size mismatch, invalid
  or unknown format identity, unsupported format version/features, invalid
  limits/extents, file-size mismatch, and header-hash failure.
- The AssetCore codec key becomes `(FormatId, FormatVersion)`. Reading legacy
  `DAST` v5 synthesizes the permanent DAST identity for dispatch while its bytes
  remain unchanged. A `DURF` DAST version with no codec is unsupported and
  cannot fall back to v5 parsing.
- No global constructor registers formats. Each owner exposes immutable
  descriptors and explicitly composes the registry used by its tool or host;
  result must not depend on descriptor discovery order.

## Current Foundations and Gaps

Core already provides canonical little-endian fixed-width primitives,
non-owning bounded regions, `FGuid`, and XXH3-128. AssetCore already has a
private statically composed codec table, validates complete capabilities, reads
the v5 preamble once, and fails before format-specific parsing when no reader
exists. The package test corpus already covers deterministic v5 output,
construct-free header reads, unsupported v4, transaction rollback, and exact
object-stream golden bytes.

The missing foundation is a shared envelope contract. `FBinaryFormatHeader`
still combines per-family magic, schema, format version, and endian marker;
AssetCore's package preamble carries only a v5 integer version; neither layer
has a 128-bit format namespace, total front-header/file extents, common header
hash, feature mask, explicit registry validation, or an independent `DURF`
oracle. Existing package tests cannot prove future DAST dispatch without first
changing the current format.

## Implementation Stages

### Stage 0: Freeze the envelope contract and DAST identity

Dependencies: the roadmap's DAST v6 selection and the characterized DAST v5
baseline.

- [ ] Record the exact 64-byte field table, GUID/hash encoding, hash-zeroing
  rule, two-phase read contract, diagnostic categories, and success-only output
  semantics in test/reference-model constants before production encoding.
- [ ] Generate the DAST GUID once with `FGuid::NewGuid()`, commit it as an
  explicit constant with `Durin.BinaryFormat.DAST`, and add a golden assertion
  that prevents accidental reallocation.
- [ ] Define the immutable descriptor/registry contract, including duplicate
  ID/name rejection, version/feature support, caller-owned size limits, and
  descriptor-order independence.
- [ ] Define the AssetCore coexistence rule that maps legacy `DAST` v5 bytes to
  the same logical DAST identity while leaving the supported-reader and
  ordinary-writer policy unchanged.
- [ ] Capture the pre-change `CoreUtilityTests`, `AssetPackageTests`, tracked
  package/fixture counts, ordinary v5 prefix, and representative header-read
  cost used for later comparison.

#### Acceptance Gate

- Every preamble byte and failure category has one authority, the DAST identity
  is permanent and nonzero, no DAST semantic field is assigned to Core, and
  the production API can be implemented without unresolved ownership or wire-
  order choices.

### Stage 1: Implement the Core envelope primitive

Dependencies: Stage 0 acceptance gate.

- [ ] Add focused `BinaryEnvelope` public/private source beside Core's existing
  binary-format utilities; do not grow `FBinaryFormatHeader` into a competing
  interpretation of `DURF`.
- [ ] Implement success-atomic prefix parsing with checked physical/header/file
  extents and caller limits, returning the exact required front-header size
  without reading format-owned data.
- [ ] Implement descriptor and immutable registry validation, lookup by
  `FormatId`, version/required-feature rejection, and deterministic diagnostics
  independent of descriptor order.
- [ ] Implement complete front-header validation and header finalization with
  exact XXH3-128 zeroed-field hashing and no native-layout serialization.
- [ ] Keep the API span/value-owned, allocation-bounded, thread-safe after
  construction, and independent of AssetCore, DObject, filesystem paths, and
  publication services.

#### Acceptance Gate

- Core can encode and validate a bounded synthetic `DURF` file from only an
  explicit descriptor registry and physical-size fact; every invalid input
  leaves outputs unchanged and no DAST symbol or semantic type appears in the
  Core implementation.

### Stage 2: Establish independent wire and robustness evidence

Dependencies: Stage 1 acceptance gate.

- [ ] Add a test-only reference encoder/parser that does not call the
  production reader, writer, finalizer, registry lookup, or shared field-offset
  helpers.
- [ ] Freeze exact minimal and nontrivial golden bytes, including a nonzero
  GUID, multi-byte version/features/extents, and header-owned bytes beyond the
  64-byte preamble.
- [ ] Prove production/reference agreement across repeated construction and
  forward/reverse descriptor order.
- [ ] Cover truncated prefixes/front matter, bad magic, header/preamble
  versions, zero/unknown/duplicate identities, duplicate names, unsupported
  format versions and feature bits, invalid limits, extent overflow,
  header/file mismatch, trailing data, reserved/hash byte mutations, and bad
  hashes.
- [ ] Add a deterministic mutation/fuzz loop over the 64-byte preamble and
  bounded front matter. Require termination, no out-of-bounds access or
  allocation beyond policy, stable classification, and no output publication
  on failure.
- [ ] Record encoded size plus header-only parse/hash cost for representative
  small and maximum-policy headers; set regression thresholds only from stable
  measurements rather than intuition.

#### Acceptance Gate

- Exact goldens, the independent oracle, mutation/fuzz coverage, and bounded
  cost evidence agree with the selected contract, and changing any persisted
  field order or hash rule breaks a focused test.

### Stage 3: Add the dormant AssetCore dispatch seam

Dependencies: Stages 1 and 2 acceptance gates.

- [ ] Declare the permanent DAST descriptor in AssetCore and change private
  codec identity from a bare wire version to `(FormatId, FormatVersion)` while
  preserving the existing complete-capability checks.
- [ ] Map a valid legacy `DAST` v5 preamble to the DAST descriptor and v5 codec
  without changing the bytes passed to header, validation, inspection, load,
  write, or mutation operations.
- [ ] Recognize `DURF` through the new Core reader, resolve only explicitly
  registered package descriptors, and reject an unimplemented DAST v6,
  unknown format, unsupported feature, or invalid header before a codec call.
- [ ] Prove duplicate codec keys/names, identity mismatches, and incomplete
  capabilities fail policy validation independent of table order.
- [ ] Retain DAST v5 as the sole supported reader and ordinary writer; assert
  that ordinary saves, redirectors, relocation, Cook package bytes, and
  construct-free inspection still use the exact legacy prefix and behavior.

#### Acceptance Gate

- AssetCore can distinguish legacy DAST v5, valid-but-unimplemented `DURF`
  DAST, unknown `DURF`, and corrupt input before semantic parsing; all existing
  v5 capability and transaction tests remain byte-stable.

### Stage 4: Qualify and document the foundation

Dependencies: Stage 3 acceptance gate.

- [ ] Run the registered `CoreUtilityTests`, `AssetPackageTests`, and the
  smallest additional registry-discovered targets required by changed source.
- [ ] Run changed-document, all-plan, and all-roadmap validation after updating
  this plan and roadmap status/evidence.
- [ ] Update the Core serialization contract with the landed neutral-envelope
  API and update Asset Packages/Versioning only to record the dormant dispatch
  seam and unchanged v5 baseline.
- [ ] Verify Git reports no modified `.dasset`, `.dasset.hex`, `.dabulk`, or
  generated/cooked binary artifact.
- [ ] Record exact test selections, golden/reference evidence, mutation count,
  header limits, and parse-cost measurements in `Current Status`; close only
  evidence-backed checks.

#### Acceptance Gate

- All focused validation passes, lasting generic rules are authoritative
  outside the plan, current assets and v5 bytes are untouched, and the M2 DAST
  v6 codec plan can begin without revisiting common-envelope identity or wire
  decisions.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Wire encoding | Exact 64-byte and extended-header goldens; explicit LE GUID/hash order; production/reference encode and decode agreement |
| Prefix bounds | Truncation, physical-size mismatch, `HeaderBytes`/`FileBytes` overflow and limit failures before allocation or format dispatch |
| Integrity | Hash field treated as zero, any protected-byte mutation rejected, hash bytes encoded low then high, and failed finalization preserves destination |
| Registry | Invalid descriptor, zero/duplicate ID, empty/duplicate name, unsupported version/features, unknown format, reversed registration, and immutable concurrent reads |
| AssetCore coexistence | Legacy v5 maps to DAST identity; unimplemented v6 fails closed; codec capability validation remains complete; ordinary writes stay byte-exact v5 |
| Robustness and cost | Deterministic bounded mutation/fuzz corpus, stable diagnostics, no excessive allocation, and recorded header-only time/bytes versus the v5 characterization |
| Repository | `CoreUtilityTests`, `AssetPackageTests`, applicable focused targets, documentation validators, and zero tracked binary changes |

## Definition of Done

- Every stage acceptance gate passes and evidence is recorded in Current
  Status with the exact validation selections and measurements.
- Core exposes one implemented `DURF` v1 contract with an independent golden
  oracle and explicit immutable registry; no DAST semantics leak into it.
- AssetCore owns one permanent DAST identity and can fail closed on `DURF`
  dispatch while DAST v5 remains the only supported reader and ordinary writer.
- No authored package, fixture, companion, DDC object, or cooked output is
  rewritten.
- Lasting envelope and unchanged package-version boundaries are documented in
  their owning domains, and the roadmap marks M1 complete with M2's entry gate
  satisfied.

## Deferred Follow-ups

- The proposed **DAST v6 Package Codec** plan owns the 32-byte DAST header,
  48-byte section directory, Public Summary, Import/Export mapping, Payload
  Directory, format-specific bounds, and complete codec capabilities.
- The proposed **DAST v6 Baseline Cutover** plan owns the exact temporary
  converter, frozen corpus manifest, tracked binary conversion, policy switch,
  project baseline validation, and removal of v5 plus the converter.
- DABK, DBLK, payload type identities, and standalone plugin formats require
  separate triggers after DAST v6; they are not latent tasks in this plan.

## Related Documentation

- [Durin Binary Envelope and DAST v6 Roadmap](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md)
- [Durin Binary Envelope Evolution](../Investigations/DurinBinaryEnvelopeEvolution.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/BinaryFormat.h`
- `Engine/Source/Runtime/Core/Private/Serialization/BinaryFormat.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Guid.h`
- `Engine/Source/Runtime/Core/Public/Hash/XxHash.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageVersionPolicy.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Tests/Native/CoreTests/Private/BinaryFormatTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageObjectStreamWireContractTests.cpp`
