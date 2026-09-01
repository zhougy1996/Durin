# Canonical DAST v8 Writer Plan

Summary: Freeze DAST v8 package sections and implement the canonical construct-free CoreDObject save linker.

Last reviewed: 2026-08-31

Status: Archived
Completed: 2026-08-31

## Current Status

All stages are complete. CoreDObject owns canonical DAST v8 freeze and write,
including nine hashed sections, stable id remapping, native tagged values,
explicit inline/external BulkData placement, DURF finalization, typed failures,
and atomic dual-output publication. Exact fixtures cover every value kind and
freeze the main/bulk hashes, selected offsets, and canonical manifest. Focused
CoreDObject tests pass and AssetRegistry plus Engine compile without executing
an Engine runtime. A v8 reader, Engine live application, Registry cutover, and
corpus conversion remain later roadmap milestones.

## Goal

Make CoreDObject the sole owner of deterministic DAST v8 package emission.
Given a complete detached linker model and explicit bulk placement, the writer
freezes every table, emits one canonical DURF/DAST main image plus an optional
raw bulk segment, and publishes neither output on any failure.

## Scope

- CoreDObject-owned DAST identity/version constants, v8 section kinds, limits,
  writer input/output, and typed diagnostics.
- Canonical Registry, name, import, export, type, schema, value, bulk-directory,
  and inline-bulk sections.
- Explicit inline/external bulk placement with a raw external-segment extent
  and digest bound by the main package.
- Frozen deterministic ids independent of input table or Map insertion order.
- CoreDObject-only exact-byte, repeatability, malformed-input, and atomicity
  tests, plus compile validation for temporary AssetRegistry/Engine consumers.

## Non-Goals

- Reading or validating v8 bytes, applying exports to DObjects, publishing
  files, choosing asset policy, scanning Registry metadata, or converting v7.
- Preserving DAST v7 section ids, object-stream opcodes, retained unknown
  payloads, compatibility reports, or in-place rewrite behavior.
- Launching Engine, an editor/game binary, Cook, or an application-hosted test.

## Frozen v8 Contract

- DAST keeps format id `3c59d1a9-6ceb-4e4c-b059-452db0a5af56` and uses DURF
  header version 1 with format version 8 and no required feature bits.
- The format header is 32 bytes after the 64-byte common preamble. It carries
  package kind, zero flags, directory offset 96, section count 9, 48-byte entry
  size, and zero reserved data.
- Nine required, contiguous, individually XXH3-128-hashed sections appear in
  this order: Registry data, names, imports, exports, types, schemas, values,
  bulk directory, and inline bulk data. Registry, names, and imports are wholly
  header-resident; `HeaderBytes` ends after imports so scans need no export data.
- Registry data version 1 carries asset class, redirect destination, main
  export id, export count, hard package ids, soft package ids, searchable-name
  ids, and the optional raw bulk-segment extent and digest. Strings are name
  ids; zero is allowed only for explicitly nullable fields.
- Package indices encode null as zero, imports as negative one-based values,
  and exports as positive one-based values. Signed indices use canonical ZigZag
  VarInt; other ids and counts use shortest unsigned VarInt.
- Imports sort by package, object, class, and Outer identity. Exports retain
  Outer-before-inner topology and sort siblings bytewise by full logical path.
  Structural types sort recursively; schemas and fields sort by bytewise name.
  Duplicate logical identities fail.
- Values use a v8-native tag derived from `EValueKind`, never v7 opcodes or
  `EPropertyGenFlags`. Property blocks sort by declaring type and field. Struct
  fields sort by schema field id; Map entries sort by canonical key token and
  reject collisions. NaNs use one quiet pattern; value signed zero is retained.
- BulkData values encode a one-based bulk-directory id. Each entry carries owner
  export/property identity, logical size, content digest, alignment, storage
  kind, and a bounded offset/size in the inline section or raw segment. Payload
  starts are zero padded to their declared power-of-two alignment.
- External bulk bytes form one headerless raw segment. Registry binds its exact
  extent and whole-segment digest. No external entries means zero extent, zero
  digest, and an empty bulk output.
- Discovery and placement finish before emission. The writer emits into detached
  buffers, verifies the frozen manifest during the value pass, finalizes hashes
  last, and replaces both caller outputs only on complete success.

## Implementation Stages

### Stage 0: Freeze byte fixtures and writer boundary

- [x] Add exact layout constants, section/version tables, and byte-offset
  assertions for the preamble, format header, directory, and Registry record.
- [x] Extend detached BulkData values with explicit element size, alignment,
  and inline/external placement; reject ambiguous default policy.
- [x] Freeze logical fixtures covering every scalar/container/reference type,
  custom versions, hard/soft/searchable data, redirects, and bulk placements.
- [x] Record complete stable hashes plus selected offsets and the canonical
  table/id manifest for the fixtures.
- [x] Confirm `PackageLinkerContractTests` for execution and `AssetRegistry`
  plus `Engine` as compile-only compatibility targets.

#### Acceptance Gate

The complete write contract and fixture identities are frozen below Engine,
all placement policy is explicit input, and no reader is required for progress.

### Stage 1: Freeze canonical tables

- [x] Validate summary, names, versions, imports, exports, schemas, property
  tags, package indices, and Outer topology into detached writer state.
- [x] Discover every referenced name and type, then assign stable one-based ids.
- [x] Rebuild import/export mappings and remap every Outer and hard reference.
- [x] Reject duplicate identities, missing fields, descriptor cycles, invalid
  type shapes, unsupported values, and unrepresentable manifest facts.
- [x] Prove equivalent shuffled inputs freeze to one identical manifest.

#### Acceptance Gate

One immutable manifest owns every emitted id; equivalent inputs have identical
manifests and failures publish no partial state.

### Stage 2: Encode v8 tables and tagged values

- [x] Encode Registry, names, imports, exports, types, schemas, custom versions,
  and property records with canonical integers and bounded strings.
- [x] Encode every scalar, enum, intrinsic, Struct, fixed/dynamic array, Map,
  hard/soft reference, byte/blob, and BulkData value.
- [x] Sort Maps through canonical key tokens and reject token collisions,
  invalid key shapes, and discovery/emission drift.
- [x] Enforce section count/byte/depth limits with stable logical-path errors.
- [x] Cover every value kind with exact bytes and atomic failure tests.

#### Acceptance Gate

All non-bulk sections and bulk handles emit deterministically from the frozen
manifest without AssetRegistry or Engine serialization code.

### Stage 3: Place bulk payloads and assemble DURF/DAST

- [x] Assign deterministic inline/external ranges with checked alignment, zero
  padding, content digests, and whole-segment binding.
- [x] Build the directory and per-section hashes, then finalize the DURF header.
- [x] Reject overflow, invalid alignment, limits, late bulk discovery, and
  destination aliasing before caller publication.
- [x] Prove repeated saves and equivalent discovery orders emit identical main
  and bulk outputs.
- [x] Freeze redirect, no-bulk, inline, external, and mixed-bulk fixtures.

#### Acceptance Gate

The writer emits canonical complete main/bulk images with internally consistent
header, section, range, padding, and digest facts and atomic failure behavior.

### Stage 4: Qualify ownership and publish landed contracts

- [x] Remove newly unreachable writer helpers while retaining bounded v7
  migration support required by P4.
- [x] Prove CoreDObject v8 sources contain no AssetRegistry, Engine, v7 opcode,
  live DObject, or live FProperty dependency.
- [x] Run focused tests and compile AssetRegistry/Engine without executing them.
- [x] Publish landed writer, bulk, determinism, and ownership contracts.
- [x] Complete the plan, update P1, and pass documentation validation.

#### Acceptance Gate

CoreDObject is the only v8 writer owner; canonical fixtures pass, affected
modules compile, no Engine runtime ran, and P2 has frozen bytes to read.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Canonical ids | Shuffled inputs produce identical tables, values, and complete bytes. |
| Tagged values | Every structural kind has exact bytes; invalid shapes, ids, depth, and Map collisions fail atomically. |
| Registry boundary | Registry, names, and imports end at `HeaderBytes`; exports and values begin afterward. |
| Bulk integrity | Ranges, alignment, padding, payload hashes, segment digest, and empty rules match fixtures. |
| DURF integrity | Preamble, v8 header, directory, section hashes, header hash, and extents are exact. |
| Ownership | CoreDObject writer sources include no AssetRegistry/Engine header and link only Core. |
| Construct-free | Tests construct no DObject, invoke no serializer/PostLoad, publish no file, and execute no Engine binary. |

## Related Code and Documentation

- [CoreDObject package linker](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageLinker.h)
- [Core binary envelope](../../../../Engine/Source/Runtime/Core/Public/Serialization/BinaryEnvelope.h)
- Temporary v7 writer (retired by the single-package-IR cutover)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Core Object Package Linker roadmap](../../../Roadmaps/Archive/2026-08/CoreObjectPackageLinker.md)
- [Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Testing Workflow](../../../Agents/Testing.md)
