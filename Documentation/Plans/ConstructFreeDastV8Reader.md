# Construct-Free DAST v8 Reader Plan

Summary: Decode the frozen DAST v8 byte contract into validated CoreDObject linker tables without constructing live objects.

Last reviewed: 2026-08-31

Status: Completed
Completed: 2026-08-31

## Current Status

All stages are complete. CoreDObject now validates the bounded DURF/DAST v8
front matter into a package-level Registry projection and decodes complete
main/raw-bulk inputs into detached linker tables. Exact P1 fixtures round-trip
byte-identically, adversarial envelope, directory, section, value, topology,
and bulk mutations fail atomically, and compile-only AssetRegistry/Engine gates
pass without executing Engine. AssetRegistry cutover, corpus conversion, live
DObject construction, Engine load, and v7 retirement remain later milestones.

## Goal

Make CoreDObject the sole owner of bounded DAST v8 interpretation. Given exact
main-package bytes and an optional raw external-bulk segment, the reader
validates the complete envelope, directory, tables, topology, tagged values,
and bulk bindings before atomically publishing a detached `FLinkerTables`.

## Scope

- CoreDObject-owned v8 registry/header projection and complete package reader
  APIs with typed, stable diagnostics and caller limits.
- DURF, format-header, directory, section-digest, extent, and header-boundary
  validation against the P1 constants.
- Bounded name, import, export, structural-type, schema, custom-version,
  property, recursive value, bulk-directory, and inline/external payload decode.
- Cross-table identity, Outer topology, hard-reference, schema/field/type,
  canonical ordering, Map-key, padding, range, and digest validation.
- Exact write-read-write round trips and malformed mutations entirely below
  AssetRegistry and Engine, plus compile-only compatibility targets.

## Non-Goals

- Constructing `DPackage`, `DObject`, reflected properties, defaults, or GC
  state; calling serializers, PostLoad, Engine load policy, or publication.
- Changing the v8 bytes frozen by P1, accepting noncanonical equivalent bytes,
  reading v7, converting the corpus, or retaining unknown wire values.
- Persisting AssetRegistry state or changing ordinary scan behavior; P3 owns
  that cutover after this reader lands.
- Launching Engine, an editor/game binary, Cook, or an application-hosted test.

## Frozen Reader Contract

- `ReadPackageV8Registry(...)` accepts exactly the declared front header plus
  independently known physical main and bulk extents. It validates DURF,
  format header, all nine directory descriptors and hashes wholly contained in
  the front header, then publishes package kind, asset class, redirect target,
  main export/count, hard/soft/searchable package names, and bulk binding.
- `ReadPackageV8(...)` requires the complete main image and exact external raw
  segment. It validates every section digest and consumes every section byte;
  missing, duplicate, unknown, reordered, gapped, overlapping, trailing, or
  overflowing descriptors fail before table allocation.
- Counts, strings, records, depths, byte extents, alignments, indices, and
  allocations use explicit v8 limits. VarInts must be shortest and UTF-8 must
  be valid. All outputs remain unchanged on any failure.
- Names, types, schemas/fields, imports, exports, properties, Struct fields,
  Map entries, custom versions, and metadata id lists must already be in the
  canonical P1 order. Duplicate identities and canonical-key collisions fail.
- Type records may reference only earlier structural children when required by
  their canonical identity reconstruction. Schema and value references must
  match exact frozen ids; package indices and Outer graphs must be in range and
  acyclic, with every export Outer preceding its child.
- Every native value tag must match its declared type. Numeric widths, enum
  storage, intrinsic component counts, fixed-array counts, Struct identities,
  provenance, and container limits are checked before publication. NaNs must
  already use the one canonical quiet representation.
- Each BulkData handle is used exactly once and agrees with its directory owner
  export/schema/field/path. Ranges are bounded, non-overlapping, aligned,
  preceded only by zero padding, and match their payload digest. Registry binds
  the exact external extent and whole-segment digest; no external entries means
  zero extent, zero digest, and an empty supplied segment.
- Successful canonical bytes round-trip through the detached model to the same
  main and bulk bytes. Reader APIs depend only on Core and CoreDObject.

## Implementation Stages

### Stage 0: Freeze reader boundaries and mutation fixtures

- [x] Add public registry projection, read limits, typed failures, and atomic
  registry/complete-reader API declarations beside the P1 writer contract.
- [x] Reuse the P1 exact main, redirect, no-bulk, mixed-bulk, and all-value-kind
  fixtures as reader truth rather than creating a second byte description.
- [x] Build deterministic mutation helpers for preamble, directory, section,
  table, value, topology, and bulk corruption without live objects.
- [x] Confirm the focused CoreDObject test target for execution and
  AssetRegistry plus Engine as compile-only consumers.

#### Acceptance Gate

Reader inputs, limits, outputs, diagnostics, and frozen byte evidence are
explicit below Engine, and no production route changes are required.

### Stage 1: Validate envelope, directory, and Registry projection

- [x] Validate the DURF preamble through Core's registry and exact v8 limits.
- [x] Decode the fixed format header and nine descriptors with checked
  arithmetic, exact ordering/contiguity, per-section hashes, and header cutoff.
- [x] Decode canonical names, imports, and Registry metadata from the bounded
  front header and validate all ids, lists, counts, and external binding facts.
- [x] Publish the registry projection only after complete header validation.
- [x] Cover truncated, corrupt, reordered, duplicate, gapped, oversized,
  noncanonical, and wrong-version front matter with typed atomic failures.

#### Acceptance Gate

Header-only consumers can trust package-level metadata and exact byte ranges
without reading exports or constructing a linker model.

### Stage 2: Decode canonical tables and topology

- [x] Decode exports, structural types, custom versions, and schemas with
  bounded records, exact consumption, and stable one-based ids.
- [x] Reconstruct recursive types and reject invalid shapes, invalid/cyclic
  references, duplicate identities, and noncanonical ordering.
- [x] Validate import/export package indices, Outer-before-inner topology,
  resolved logical-path uniqueness, main export, and export count.
- [x] Prove every writer fixture reconstructs the expected canonical manifest.
- [x] Cover malformed counts, records, strings, ids, schemas, and topology.

#### Acceptance Gate

All tables and package topology are detached, canonical, and cross-validated
before any value or bulk payload is published.

### Stage 3: Decode tagged values and bind BulkData

- [x] Decode every scalar, enum, intrinsic, Struct, fixed/dynamic array, Map,
  reference, byte/blob, and BulkData tag against its exact frozen type.
- [x] Validate property/schema/field/type identity, nested provenance, value
  depth/count/width, canonical NaNs, Map order/tokens, and reference ranges.
- [x] Decode bulk-directory ownership and validate handles, placement,
  alignment, padding, non-overlap, payload digests, and external binding.
- [x] Publish one complete `FLinkerTables` only after all sections and supplied
  bytes are consumed and validated.
- [x] Cover every value kind plus late malformed value and bulk failures while
  proving outputs remain unchanged.

#### Acceptance Gate

Canonical v8 main/bulk bytes produce one complete format-neutral linker model;
all malformed value and payload routes fail closed and atomically.

### Stage 4: Qualify round trips and publish landed contracts

- [x] Prove write-read-write byte identity for canonical, shuffled-source,
  redirect, no-bulk, inline, external, mixed, and all-value-kind fixtures.
- [x] Prove the reader sources contain no AssetRegistry, Engine, v7 opcode,
  live DObject, serializer, PostLoad, or live FProperty dependency.
- [x] Run focused reader/writer/linker tests and compile AssetRegistry/Engine
  without executing an Engine binary.
- [x] Publish landed reader, bounds, topology, value, bulk, and atomicity
  contracts in owning Runtime documentation.
- [x] Complete the plan, update P2, and pass documentation validation.

#### Acceptance Gate

CoreDObject is the sole v8 reader owner, exact round trips and adversarial
fixtures pass below Engine, compile-only consumers remain buildable, and P3 can
consume a trusted header projection without parsing exports.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Atomic decode | Registry and complete outputs retain sentinels for every failure class. |
| Envelope/directory | Exact layout, extent, ordering, cutoff, hash, and trailing-byte mutations fail. |
| Canonical tables | Names, types, versions, schemas, imports, exports, fields, and properties reject reordering and duplicates. |
| Topology/references | Invalid main/Outer/hard ids, cycles, forward Outers, and duplicate resolved paths fail. |
| Tagged values | Every `EValueKind`, provenance, width, depth, shape, tag, Map token, and NaN rule is covered. |
| Bulk integrity | Handles, owners, ranges, alignment, padding, hashes, extent, empty rules, and overlap are exact. |
| Round trip | Every canonical P1 fixture writes, reads, and writes to byte-identical main/bulk outputs. |
| Ownership | Reader sources link only Core/CoreDObject and execute no Engine-hosted target. |

## Related Code and Documentation

- [CoreDObject package format](../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [CoreDObject package linker](../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageLinker.h)
- [Core binary envelope](../../Engine/Source/Runtime/Core/Public/Serialization/BinaryEnvelope.h)
- [Canonical DAST v8 writer plan](CanonicalDastV8Writer.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Core Object Package Linker roadmap](../Roadmaps/CoreObjectPackageLinker.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)
