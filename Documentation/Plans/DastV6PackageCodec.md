# DAST v6 Package Codec Plan

Summary: Implement the detached DAST v6 package codec with complete AssetCore capability parity while preserving the v5 ordinary writer.

Last reviewed: 2026-08-25

Status: Completed
Completed: 2026-08-25

## Current Status

This is the active M2 child of the
[Durin Binary Envelope and DAST v6 roadmap](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md).
M1 has frozen and implemented `DURF` v1, the permanent DAST identity, immutable
registry, diagnostics, and fail-closed AssetCore dispatch. DAST v5 remains the
only registered reader and ordinary writer; no tracked package or fixture has
changed.

All five stages are complete. AssetCore now owns a detached, capability-complete
DAST v6 codec with the selected 32-byte format header, eight canonical required
sections, 48-byte directory entries, bounded header-only inspection, complete
validation, exact v5 logical adaptation, no footer, and fail-closed unknown-data
mutation. The supported-reader policy and ordinary writer remain v5 and no
tracked binary changed.

The regenerated Stage 0 inventory found 27 tracked packages. Observed maxima
were 10,964 file bytes, 503 v5 header bytes, 445 summary bytes, six Imports, 19
Exports, section sizes of 1,780 Name, 94 Type, 260 Schema, 77 Export/Object, and
8,808 Value bytes, plus one payload entry. Selected limits are 16 MiB front
header, 1 GiB file/section, 65,536 Imports, 1,048,576 Exports, and 65,536
payload entries, all with more than 8x corpus headroom.

The independent parser and production codec agree on a deterministic 600-byte
nontrivial golden with XXH3-128
`1262496184075ce9227c5b2864562f71`; exact repeated conversion and v6-to-v5
reconstruction agree. Robustness evidence covers all 416 DAST header/directory
bytes, count/index failures, gaps, trailing bytes, section/envelope hashes, and
unknown required/skippable behavior. Debug measurements over 1,000 iterations
were 552 front-header bytes, 4.41 microseconds header-only, and 36.09
microseconds full validation; thresholds are 500 and 2,000 microseconds.

Final registered validation passed `CoreUtilityTests` 91/91,
`AssetPackageTests` 121/121, `AssetPackageTrailerTests` 8/8,
`AssetBulkContainerTests` 11/11, and `AssetCookTests` 13/13. Capability evidence
includes construct-free header/inspection, references, compatibility, live
load, write, relocation, reference rewrite, redirector, external DABK
descriptor equality, failure atomicity, and unknown no-loss rejection.

## Goal

Provide one detached, fully capable DAST v6 codec under `DURF` v1 that proves
the complete package semantics and AssetCore operations needed for cutover,
without changing the ordinary writer, supported-reader policy, or tracked
authored baseline.

## Scope

- Implement the fixed 32-byte DAST v6 format header and canonical 48-byte
  section directory with exact little-endian encoding and section hashes.
- Implement required Public Summary, Import, Name, Type, Schema, Export, Value,
  and Payload Directory sections with checked 64-bit extents and no footer.
- Preserve or exactly translate the proven v5 Name, Type, Schema, Export/Object,
  Value, dependency/Import, and external-payload logical model.
- Provide detached header inspection, full validation, reference extraction,
  compatibility probing, live loading, writing, reference rewriting,
  relocation, redirector creation, Cook construction, and companion-compatible
  package bytes through one complete codec.
- Add production/independent wire agreement, corruption/topology coverage,
  failure-atomic capability tests, and measured header size/cost evidence.

## Non-Goals

- Selecting DAST v6 as the ordinary writer or supported repository reader.
- Rewriting `.dasset`, `.dasset.hex`, `.dabulk`, DDC, or cooked artifacts.
- Adding a persistent v5-to-v6 converter or keeping a runtime migration graph.
- Changing DABK, DBLK, reflected payload schemas, builder versions, package
  path identity, or external companion transaction ownership.
- Accepting unknown skippable sections in an operation that cannot preserve
  them byte-exactly and canonically.

## Design Decisions and Invariants

- DAST v6 uses the M1 permanent DAST `FormatId`, `FormatVersion` 6, and required
  feature mask zero under the fixed 64-byte `DURF` v1 preamble.
- The 32-byte DAST header contains package kind, package flags, absolute
  directory offset, section count, 48-byte entry size, and zero reserved word.
- Required section kinds are Public Summary, Import, Name, Type, Schema,
  Export, Value, and Payload Directory in that canonical order. Each entry
  stores kind, required/skippable flags, absolute offset, exact size,
  XXH3-128 section hash, and zero reserved word.
- Public Summary and Import bytes are inside `HeaderBytes`; the complete
  directory is also inside `HeaderBytes`. Initial writer extents are contiguous
  with no gaps or padding from the preamble through physical EOF.
- `HeaderBytes` ends after Import. Name, Type, Schema, Export, Value, and Payload
  Directory are validated by directory extent/hash before interpretation.
- Limits are explicit: 16 MiB front header, 1 GiB file and individual section,
  65,536 Imports, 1,048,576 Exports, existing 1 MiB strings, existing object
  stream table/value/depth limits, and 65,536 payload entries. Stage 0 records
  corpus maxima and confirms at least 8x headroom for observed counts/bytes.
- The v5 dependency order becomes one-based Import order. Existing external
  hard-reference dependency IDs are interpreted as Import indexes. Existing
  object IDs become one-based Export indexes, `Outer` remains zero or an Export
  index, and the sole public main Export is selected by `MainExportIndex`.
  Soft paths remain paths.
- The proven v5 Name, Type, Schema, and Value section bytes may be reused
  exactly. The v5 Object section becomes Export after topology validation. The
  v5 trailer entry model becomes Payload Directory; DTRL/DTRF bytes are never
  embedded in v6.
- A detached adapter may reconstruct a canonical internal v5 logical package
  solely to reuse the already-qualified semantic engine. That representation
  is never published, never accepted as v6 wire authority, and must round-trip
  through exact v6 validation before output publication.
- Unknown required sections fail. Unknown skippable sections are extent- and
  hash-validated but any save or mutation rejects unless their bytes and
  canonical placement can be preserved exactly.
- The v6 codec exists in the table for test-only/detached selection with full
  capabilities but is not added to `SupportedAssetPackageReaderVersions` and
  does not replace `OrdinaryAssetPackageWriterVersion` during M2.

## Current Foundations and Gaps

M1 supplies exact common-envelope framing, a permanent identity, bounded
registry dispatch, success-atomic APIs, independent goldens, and mutation
coverage. DAST v5 supplies deterministic logical tables/values, complete codec
capabilities, construct-free inspection, transactional live load, mutation,
Cook construction, trailer-directory validation, and companion transactions.

The missing layer is the DAST v6 format-owned header, front directory and
summary/import model, no-footer payload directory, exact topology validation,
and a detached codec proving every AssetCore capability before cutover.

## Implementation Stages

### Stage 0: Freeze topology, limits, and logical mapping

- [x] Measure tracked v5 package/header/dependency/object/payload maxima and
  record the selected v6 limits with documented headroom.
- [x] Freeze exact DAST header, directory entry, section kind/flag, Public
  Summary, Import, Export, and Payload Directory field tables in test constants.
- [x] Define the byte-exact reuse/conversion mapping from v5 logical sections,
  dependency/object indexes, package kinds, redirectors, and trailer entries.
- [x] Define required/skippable section handling, canonical order/contiguity,
  no-loss mutation policy, diagnostics, and success-only output behavior.
- [x] Capture M1/v5 focused test baselines and representative header size/cost.

#### Acceptance Gate

- Every persisted field, count, index, limit, and failure category has one
  authority and the detached implementation has no unresolved mapping choice.

### Stage 1: Implement the detached v6 wire codec

- [x] Add focused DAST v6 wire types, encoder/builder, bounded prefix/header
  inspection, complete parser, and section-hash validation inside AssetCore.
- [x] Emit exact common and format headers, canonical directory entries,
  contiguous sections, `HeaderBytes`, `FileBytes`, and zeroed-field header hash.
- [x] Implement Public Summary, Import, Export, and Payload Directory encoding
  and decoding with one-based index/topology validation.
- [x] Reject missing/duplicate/out-of-order sections, gaps, overlap, trailing
  bytes, overflow, nonzero reserved fields, bad hashes, excessive counts, and
  unsupported required/skippable data.
- [x] Add an independent test-only encoder/parser and exact minimal/nontrivial
  golden bytes without calling production v6 helpers.

#### Acceptance Gate

- Production and independent bytes agree; valid detached v6 parses
  construct-free and every malformed topology fails before semantic decode.

### Stage 2: Complete AssetCore capability parity

- [x] Add the detached v6 codec with complete read/write/mutate capability
  pointers while leaving supported-reader and ordinary-writer policy at v5.
- [x] Implement exact v5-logical-to-v6 construction and v6-to-logical adapter
  for Name, Type, Schema, Export/Object, Value, Imports, and payload entries.
- [x] Prove header inspection, validation, inspection, references,
  compatibility, live load, deterministic write, relocation, rewrite, and
  redirector parity.
- [x] Prove Cook package construction and external DABK descriptor equality
  without changing companion ownership or publication transactions.
- [x] Preserve failure atomicity and reject mutation/save when accepted unknown
  skippable data cannot be retained byte-exactly.

#### Acceptance Gate

- Every codec capability passes success and injected-failure coverage, v6 has
  no footer, and ordinary production activity still emits exact v5 bytes.

### Stage 3: Establish robustness and cost evidence

- [x] Cover all format-header and directory-entry fields with deterministic
  mutation, bounded pathological counts, index failures, and stable diagnostics.
- [x] Prove repeated/reverse-discovery deterministic bytes and independent
  decode equality for ordinary, redirector, referenced, and external-bulk cases.
- [x] Measure v6 front-header bytes and header-only/full validation cost against
  the recorded v5/M1 samples and set evidence-based regression thresholds.
- [x] Run the smallest registry-discovered package, Cook, trailer, bulk, and
  Core targets required by the changed capability surface.

#### Acceptance Gate

- Robustness, semantic parity, failure atomicity, and accepted size/cost
  evidence satisfy the roadmap M2 exit gate.

### Stage 4: Qualify and document the detached codec

- [x] Update Asset Packages and Versioning with the implemented detached v6
  contract and explicit unchanged baseline policy.
- [x] Update this plan and roadmap with exact tests, goldens, mutations,
  measured limits/cost, and M2 completion evidence.
- [x] Run changed-document, all-plan, and all-roadmap validation.
- [x] Verify no tracked package, fixture, companion, DDC, or cooked artifact
  changed and commit the isolated M2 implementation.
- [x] Create the M3 baseline-cutover plan only after every M2 gate passes.

#### Acceptance Gate

- Lasting v6 rules are authoritative, all required validation passes, the v5
  baseline is byte-stable, and M3 can freeze and convert the complete corpus.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Wire | Exact headers/directories/sections, independent goldens, LE GUID/hash words, contiguous extents, no footer |
| Topology | Required unique order, Public Summary equality, bounded Import/Export indexes, no gaps/overlap/trailing bytes |
| Semantics | Exact retained table/value bytes or proven conversion, hard Import/Export union, soft paths, redirector equality |
| Capabilities | Header, validate, inspect, references, compatibility, load, write, rewrite, relocate, redirector, Cook, external bulk |
| Transactions | No construction/publication before validation, rollback, destination success atomicity, unknown no-loss rejection |
| Robustness/cost | Deterministic mutations, pathological bounds, stable diagnostics, front-header/full-parse size and timing |
| Repository | Focused registered targets, document validators, exact v5 ordinary prefix, zero tracked binary changes |

## Definition of Done

- All stages and acceptance gates pass with evidence in Current Status.
- One detached DAST v6 codec implements every capability and exact wire rule.
- DAST v5 remains the only supported reader and ordinary writer during M2.
- No tracked authored or generated binary changes.
- The roadmap marks M2 complete and links an active M3 cutover plan.

## Deferred Follow-ups

- M3 owns the frozen manifest, temporary exact converter, policy switch,
  tracked corpus/fixture conversion, baseline qualification, and removal of v5
  plus converter code.
- DABK/DBLK/payload-format adoption remains outside this roadmap.

## Related Documentation

- [Durin Binary Envelope and DAST v6](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md)
- [Durin Binary Envelope Foundation](DurinBinaryEnvelopeFoundation.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/BinaryEnvelope.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageObjectStreamReader.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageTrailer.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageObjectStreamWireContractTests.cpp`
