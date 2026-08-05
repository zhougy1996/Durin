# Compact Asset Serialization Plan

Summary: Replace repeated DAST property metadata with package-local name, type, and schema tables plus default-relative value encoding while preserving authored-asset compatibility inspection.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- DAST v2 is the only package version accepted by the current reader and
  emitted by the current writer.
- The v2 public header is already suitable for bounded registry inspection: it
  stores magic, version, main-asset class, package dependencies, and object
  count before any object payload.
- Every object field repeats its declaring qualified class, property name,
  property kind, recursive textual type signature, and payload size. Every
  nested `DStruct` value repeats the same information for all of its fields.
- `Engine/Content/Materials/DefaultMaterial.dasset` is the motivating corpus.
  Its 2026-08-05 v2 baseline is 82,636 bytes. A byte-level inspection found
  48,170 bytes in printable string runs, of which 45,722 bytes are repeated
  occurrences, and 29,022 zero bytes. The most frequent qualified struct name,
  `Durin::FMaterialParameterDefinition`, occurs 520 times.
- General-purpose compression proves that the data is redundant—Deflate
  reduces the baseline to about 2.9 KiB and Brotli's smallest-size setting to
  about 2.2 KiB—but it does not remove repeated parsing, string allocation, or
  reflected-schema reconstruction after decompression.
- This plan selects structural compaction as the authored-package solution.
  No implementation stage has started.

## Goal

Introduce a deterministic DAST v3 authored-package encoding that:

- stores reflection names and recursive wire types once per package;
- stores each class or struct field schema once per package;
- encodes object and struct instances as default-relative, length-delimited
  field overrides;
- encodes intrinsic math values without reflected field-name strings;
- keeps the public package header independently readable by the registry;
- preserves unknown-field retention, structure compatibility reporting, and
  explicit data-loss consent;
- retains a bounded DAST v2 reader and rewrites a loaded v2 package to v3 only
  on an authorized save;
- reduces the canonical Default Material package to at most 25 percent of its
  same-content v2 baseline and no more than 16 KiB; and
- avoids repeated materialization of equal metadata strings during full load or
  compatibility inspection.

## Scope

- A DAST v3 public header and section directory.
- Package-local name, type, and schema tables.
- Canonical variable-width identifiers and sizes.
- Default objects for reflected `DClass` values and deterministic default
  construction for serializable `DStruct` values.
- Type-aware property equivalence used for serialization delta decisions.
- Compact value records for every property kind supported by DAST v2.
- Stable intrinsic wire codecs for CoreDObject math value types.
- DAST v2/v3 header reading, complete loading, inspection, compatibility
  probing, registry caching, and save migration.
- Deterministic tests, malformed-input tests, compatibility fixtures, package
  size accounting, content resave, and lasting format documentation.

## Non-Goals

- General-purpose Deflate, Brotli, Zstandard, or other block compression in the
  authored `.dasset` envelope.
- UE-style unversioned property serialization for cooked packages. A future
  cooked format may omit schema metadata only when the cooker and runtime share
  a frozen reflection layout.
- Pooling every ordinary `std::string` value. V3 pools metadata names and
  reflected `FName` values; ordinary user strings remain inline unless a later
  measured change adds a separate repeated-string representation.
- Changing DMSH, TXPL, DBLK, DDC keys, source provenance, or bulk-data
  publication.
- Asset-specific material serialization. Default Material is a representative
  benchmark, not a special-case file format.
- Editor UI, automatic bulk resave on discovery, silent compatibility repair,
  or writable actions in Asset Compatibility Audit.
- Async package loading, memory mapping, soft references, redirects, or hot
  reload.

## Design Decisions and Invariants

### UE Reference Boundary

The selected design borrows four separable UE concepts without copying its
package implementation:

1. A package summary acts as a table of contents and leaves registry-relevant
   metadata readable without export payloads.
2. A package-local name map replaces repeated serialized `FName` strings with
   compact local references.
3. Authored assets remain field-tagged for schema evolution, unknown-field
   handling, and conversion diagnostics.
4. Object values serialize relative to a class default object or archetype-like
   baseline.

UE's cooked `SAVE_Unversioned_Properties` tradeoff is deliberately excluded
from authored DAST v3 because it assumes writer and reader share the same
property definitions and removes the metadata required by Durin's compatibility
audit.

Reference API documentation:

- [UE `FPackageFileSummary`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPackageFileSummary)
- [UE `FLinkerSave`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FLinkerSave)
- [UE `FPropertyTag`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPropertyTag)
- [UE `UObject::SerializeScriptProperties`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject/SerializeScriptProperties)
- [UE `ESaveFlags`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/ESaveFlags)

### V3 Envelope and Public Header

All multibyte fixed-width values use explicit little-endian encoding. Counts,
table-local identifiers, payload sizes, and ordinary string sizes use canonical
unsigned LEB128 (`VarUInt`): writers emit the shortest representation and
readers reject overflow and overlong encodings.

The conceptual v3 layout is:

```text
DAST v3 fixed prefix
  magic
  package format version
  flags
  public header byte count

public header
  main asset qualified class UTF-8 string
  sorted dependency UTF-8 strings
  object count
  section directory entries

NameTable
TypeTable
SchemaTable
ObjectTable
ValueData
```

Each section directory entry records a known section kind, absolute file
offset, encoded byte size, and logical entry count. Sections are ordered as
shown, non-overlapping, and exactly cover the bytes after the public header.
The complete reader rejects missing, duplicate, overlapping, out-of-order, or
out-of-bounds sections and trailing data.

The main asset class and dependency strings remain inline in the public header
instead of depending on the body Name Table. Their count is small, and this
keeps `ReadAssetPackageHeader` independent of all body allocations. Registry
header reads remain bounded by explicit string, dependency, directory, and
header-size limits.

Package paths continue to derive from mounted filenames. V3 does not serialize
the owning package path into the body.

### Name Table

The Name Table contains each of the following UTF-8 values at most once:

- qualified class, struct, and enum names used by body schemas;
- declaring-type and reflected field names;
- object names; and
- serialized `FName` values.

Entries are sorted by unsigned UTF-8 byte order before IDs are assigned. ID zero
is reserved for `None`; stored entries begin at ID one. A name record contains
only a bounded VarUInt byte length followed by exact UTF-8 bytes. Duplicate,
empty, invalid, or non-canonical ordering is corrupt input.

Readers intern each table entry into `FName` at most once and retain compact IDs
through schema and value parsing. They do not construct a `std::string` for
every field occurrence. Ordinary `std::string` property values remain bounded
inline UTF-8 payloads and are not automatically added to the Name Table.

### Type Table

The Type Table replaces recursive textual signatures such as
`Array<Struct<Durin::FVector3>>` with deduplicated binary type nodes. A node has
a stable v3 wire opcode followed only by opcode-specific operands:

- fixed scalar opcodes for bool, signed and unsigned integers, float32, and
  float64;
- versioned logical opcodes for UTF-8 string, Name ID, and Guid128;
- enum with qualified enum Name ID and fixed underlying scalar opcode;
- object reference with required qualified class Name ID and object-pointer
  wrapper semantics;
- struct with a Schema ID;
- array with an element Type ID; and
- map with key and value Type IDs.

Type nodes are deduplicated and deterministically sorted by their canonical
recursive descriptor before IDs are assigned. Runtime `ElementSize`, C++ type
names, property pointers, reflection registration order, and process-local hash
values never enter the wire contract.

The v3 reader compares binary type descriptors recursively with current
reflection metadata. A matching field name with a different descriptor remains
a `TypeMismatch`; bytes are never reinterpreted.

### Schema Table

The Schema Table contains one entry for every serialized `DClass` or `DStruct`.
Each entry records:

- schema kind: class or struct;
- qualified type Name ID;
- a deterministically sorted list of serializable fields; and
- for each field, declaring-type Name ID, field Name ID, Type ID, fixed array
  dimension, and no-delta policy.

Schema field ordering uses declaring qualified name followed by field name,
both in unsigned UTF-8 byte order. It never depends on memory offsets or
reflection registration order. Instance records refer to schema-local field
ordinals, but the Schema Table retains the stable names and types required to
map those ordinals to a different runtime layout.

Transient and serialization-filtered properties do not enter a schema. A
schema containing two fields with the same declaring type and field name is
invalid.

### Default Objects and Delta Semantics

Durin will adopt UE-like default-relative object serialization:

- Each constructible `DClass` owns one lazily created default object.
- A default object is rooted for the class lifetime, marked transient and
  default-only, has no authored package identity, is excluded from asset object
  discovery, is never saved, and never receives `PostLoad()`.
- Default creation invokes the ordinary class constructor exactly once after
  the class reflection layout is complete.
- Serializable `DStruct` values must expose deterministic initialize, destroy,
  and copy lifecycle operations. A struct without the required lifecycle fails
  v3 saving rather than falling back to uninitialized bytes.
- A new type-aware reflected-value equivalence helper compares logical values
  recursively. It never compares struct padding or container backing storage.
  Floating-point values compare their canonical wire bits so NaN payloads and
  signed zero round-trip deterministically.
- A top-level object field equal to its class default is absent from that
  object's override list.
- A struct value is initialized from its current struct default and stores only
  fields that differ from that default. This rule also applies to struct values
  inside arrays and maps.
- Arrays and maps are first compared as complete logical properties against the
  owning object or struct default. A differing container serializes its full
  logical entry set, while nested struct elements still use sparse struct
  overrides.
- Map entries are ordered by canonical encoded key bytes before writing. Equal
  canonical keys are rejected as an invalid reflected map state.
- A no-delta property policy forces a field override even when it equals the
  current default. The exact property metadata spelling is selected in Stage 0
  and becomes part of the lasting reflection contract.

No default payload is stored in each asset package. This intentionally selects
UE-like inheritance semantics: when a constructor, class default object, or
struct default changes, an old asset that omitted the field observes the new
default after loading. A semantic change that must preserve old behavior needs
a format/custom-version migration or a forced explicit override before the
default changes.

Default values containing non-null object references are forced explicit in v3
until Durin has a separate class-default dependency contract. This prevents an
omitted default reference from disappearing from package dependency metadata.

### Compact Instance and Value Records

An object record contains only:

```text
ObjectId: VarUInt
OuterObjectId: VarUInt, zero for the main object
ClassSchemaId: VarUInt
ObjectNameId: VarUInt
OverrideCount: VarUInt
Repeated overrides:
  FieldOrdinal: VarUInt
  PayloadSize: VarUInt
  Payload bytes
```

Overrides are written in ascending field ordinal order. Duplicate or descending
ordinals are corrupt. Payload sizes remain present even when a current reader
knows the field's fixed size, because bounded skipping and unknown-field
retention are authored-format invariants.

A reflected struct value does not repeat its type or schema ID; the containing
Type ID already supplies it. Its value payload is the same ordered,
length-delimited override sequence used by an object. Arrays and maps encode a
VarUInt count followed by values whose types are inherited from the Type Table.

Internal object references retain their Object ID. Cross-package object
references encode a one-based index into the sorted public dependency list
instead of repeating the dependency path in Value Data. Reference kind remains
explicit and bounded.

### Intrinsic Math Wire Codecs

AssetCore owns stable v3 intrinsic opcodes for the CoreDObject math types used
as common values:

- `FVector2` as two float64 components;
- `FVector3` as three float64 components;
- `FVector4` as four float64 components;
- `FQuat` as four float64 components;
- `FTransform` as intrinsic quaternion, translation, and scale values; and
- `FLinearColor` using its declared logical scalar widths.

Intrinsic values encode their components explicitly and never copy C++ object
memory. Their instances contain no type, struct, or component-name strings.
The Type Table records only the intrinsic opcode. AssetCore validates that the
registered reflected type matches the expected logical component contract
before using an intrinsic codec; mismatch fails saving or reports an
incompatible package rather than silently falling back.

Other structs use the ordinary Schema Table representation. Asset-specific
modules do not register ad hoc codecs in v3.

### Authored Compatibility and Inspection

V3 remains a tagged authored format even though tags move from each value into
the package Schema Table:

- The complete loader maps file schemas to current reflection by qualified
  declaring type and field name.
- Unknown or removed file fields with explicit overrides retain their exact
  payload bytes and file Type descriptor in `FAssetLoadReport`.
- A file field with no override carries no instance data to retain; its value
  was the default under the file's selected delta semantics.
- Unknown classes, invalid Outers, bad IDs, incompatible type descriptors,
  malformed references, and invalid section or payload bounds preserve their
  current terminal classifications.
- Compatibility inspection reads and validates Name, Type, and Schema tables
  once, then streams Object and Value records. It seeks over length-delimited
  payloads without constructing objects, loading dependencies, or copying
  payloads.
- Compatibility report schemas continue to expose stable string identities;
  package-local numeric IDs never escape as user-facing identities.
- Explicit data-loss consent and ordinary-save rejection remain unchanged.

### Version and Migration Policy

- `LatestAssetPackageVersion` becomes 3 while the supported reader set contains
  versions 2 and 3.
- Header readers, registry entries, and compatibility reports record the actual
  file version. Code must not equate `supported` with `latest`.
- The asset-registry cache schema is invalidated once for the new version model
  and thereafter accepts fingerprints for every supported package version.
- DAST v2 remains fully readable and auditable. The v2 parser keeps its current
  bounds and compatibility behavior.
- The writer emits only canonical v3. Loading or scanning a v2 package does not
  dirty or rewrite it. The next authorized package save writes v3 atomically.
- Frozen v2 compatibility fixtures remain v2. Authored repository content is
  resaved to v3 only after dual-reader and end-to-end validation passes.
- No in-place byte patching or bulk startup migration is permitted.

### Determinism and Failure Policy

- Dependency, name, type, schema, field, map-key, object, and override ordering
  is explicitly deterministic.
- Writers use a discovery pass followed by a frozen-table emission pass. A
  value or dependency discovered after table freeze is an internal save error.
- Every count, byte length, ID, recursion depth, section, and cumulative
  allocation has a format-specific upper bound checked before allocation or
  pointer arithmetic.
- Readers reject non-canonical VarUInt values, invalid UTF-8, duplicate table
  entries, invalid table order, recursive Type cycles, invalid Schema/Type
  references, integer overflow, and any unconsumed bytes.
- Save failures leave the original package and registry entry unchanged under
  the existing atomic bundle publication contract.
- A v3 package never falls back to v2 interpretation after a v3 parse failure.

## Current Foundations and Gaps

### Foundations

- `AssetSystem.cpp` already separates public-header reading, package byte
  construction, complete reading, save publication, registry updates, and
  inspection entry points.
- `FPackageFile`, `FObjectRecord`, and `FFieldRecord` provide an existing
  intermediate representation that can be replaced with discovery and frozen
  v3 table models without changing public asset-manager ownership.
- Reflection exposes qualified class/struct/enum names, property kinds,
  recursive container metadata, object-reference helpers, and value lifecycle
  callbacks.
- `FPropertyValueSnapshot` already provides detached property capture and
  logical object-reference retention that can inform the new equivalence
  helper.
- Asset Compatibility Audit already freezes reflection identities and performs
  bounded off-thread descriptor inspection.
- Atomic package bundle saving, registry fingerprints, DAST wire fixtures, and
  broad PackageTests coverage already exist.

### Gaps

- `DClass` has a default object name but does not own a class default object.
- There is no general, side-effect-audited default-object construction contract.
- There is no public recursive reflected-property equivalence operation.
- DStruct serialization repeats its schema for every value.
- Type compatibility uses formatted strings instead of canonical binary
  descriptors.
- Readers allocate and compare repeated metadata strings for every field.
- `AssetVersion` currently acts as both latest writer version and only accepted
  reader version, including inside registry-cache validation.
- Existing tests do not assert package metadata cardinality, per-section byte
  accounting, or a package-size budget.

## Implementation Stages

### Stage 0: Freeze the V3 Wire and Default Contracts

- [ ] Add focused v2 byte-accounting coverage for Default Material and a small
  synthetic package containing every supported property kind.
- [ ] Record total bytes split into public header, repeated metadata, value
  payloads, and zero/default bytes; keep the measurement generated by tests or
  a test helper rather than a hand-maintained binary parser.
- [ ] Assign and document every v3 wire opcode, section kind, flag, upper bound,
  VarUInt rule, intrinsic codec, and canonical ordering rule.
- [ ] Audit all constructible asset classes for constructor side effects,
  required Outer assumptions, object creation, and external default references.
- [ ] Audit every serializable DStruct for initialize/destroy/copy lifecycle
  availability and deterministic default construction.
- [ ] Select the exact default-object flags, ownership, name, creation phase,
  shutdown behavior, and no-delta property metadata spelling.
- [ ] Define logical equality for every supported property kind, including
  NaN, signed zero, enum width, object references, arrays, maps, and nested
  structs.
- [ ] Define the v2/v3 registry-cache compatibility model independently of the
  latest writer version.
- [ ] Update this plan before implementation if the constructor or lifecycle
  audits invalidate the selected class-default-object design; do not introduce
  a hidden alternate baseline.

#### Acceptance Gate

- The byte contract can be implemented without relying on host ABI, reflection
  registration order, runtime hashes, or an unresolved default-value source.
- Default Material has a reproducible v2 baseline and every planned size gate
  names the same logical corpus.
- There are no open decisions about CDO lifecycle, intrinsic opcode ownership,
  canonical ordering, supported versions, or malformed-input limits.

### Stage 1: Add Default-Relative Reflection Foundations

- [ ] Add one rooted, transient default object per constructible `DClass` under
  the Stage 0 lifecycle contract.
- [ ] Exclude default objects from asset packages, object-path publication,
  ordinary object enumeration, garbage collection, duplication roots, and
  serialization.
- [ ] Enforce deterministic value lifecycle operations for serializable
  DStructs and diagnose unsupported structs at reflection or save boundaries.
- [ ] Add a recursive reflected-property equivalence API with explicit behavior
  for all supported property kinds.
- [ ] Make property snapshots and the equivalence API share canonical logical
  value rules where practical instead of creating two competing encodings.
- [ ] Add a no-delta property policy and force non-null default object
  references explicit.
- [ ] Add CoreDObject tests for one-time construction, inheritance, rooting,
  shutdown, constructor defaults, struct lifecycle, logical equality, and
  exclusion from ordinary object graphs.

#### Acceptance Gate

- Every asset class used by native tests can obtain a deterministic default
  object without publishing an asset, invoking `PostLoad()`, or leaking a
  package-visible object.
- Logical equality distinguishes every wire-significant value and ignores no
  serialized state.
- CoreDObject lifecycle and GC tests pass without AssetCore depending on test
  ordering.

### Stage 2: Implement the Deterministic V3 Writer

- [ ] Split latest writer version from supported reader versions.
- [ ] Add bounded fixed-width and canonical VarUInt writer primitives.
- [ ] Replace v3 textual-signature construction with canonical Type descriptors
  while keeping the v2 writer model available only to test helpers needed for
  migration comparison.
- [ ] Discover dependencies, names, types, schemas, objects, and values before
  writing bytes; freeze and deterministically assign all local IDs.
- [ ] Emit the public header and exact section directory before body sections.
- [ ] Emit sorted Name, Type, and Schema tables with duplicate and late-
  discovery assertions.
- [ ] Emit default-relative object and struct override records with a payload
  size for every explicit field.
- [ ] Encode cross-package references through sorted dependency indices and
  internal references through object IDs.
- [ ] Implement explicit intrinsic math codecs and validate their reflected
  logical layouts.
- [ ] Canonicalize map entries by encoded key bytes.
- [ ] Add deterministic golden-byte fixtures, repeated-save equality tests,
  table-cardinality tests, and section byte accounting.

#### Acceptance Gate

- Repeated saves of the same object graph produce identical v3 bytes across
  clean process runs.
- The synthetic all-property package contains no textual recursive type
  signatures and no per-instance reflected field names.
- Every `FVector3` instance emits only its explicit component values and
  surrounding value framing; no vector type or component-name string is
  repeated in Value Data.
- Default Material meets the 25-percent and 16-KiB size gates before any
  general-purpose compression.

### Stage 3: Implement V3 Loading, Inspection, and Compatibility

- [ ] Parse and validate the v3 public header without reading or allocating any
  body section.
- [ ] Parse Name, Type, and Schema tables with bounded allocations and intern
  every Name entry at most once.
- [ ] Construct all object skeletons before resolving internal and external
  references, preserving circular dependency behavior.
- [ ] Initialize objects and structs from current defaults, then apply ordered
  explicit overrides.
- [ ] Map schema fields to current reflection by declaring type and field name
  and validate their binary Type descriptors recursively.
- [ ] Preserve unknown explicit overrides and their file descriptors in
  `FAssetLoadReport`; reject ordinary saves under the existing compatibility-
  risk policy.
- [ ] Extend `InspectAssetPackage`, field helper reads, the frozen reflection
  catalog, the worker compatibility probe, DurinAssetAudit, and editor audit
  presentation to understand v3 without constructing objects.
- [ ] Add malformed fixtures for every section, table, VarUInt, ID, ordering,
  recursion, payload, reference, UTF-8, and default-policy boundary.
- [ ] Add allocation/cardinality assertions proving repeated metadata does not
  recreate one string per field occurrence.

#### Acceptance Gate

- V3 round-trips every supported scalar, logical, enum, object, struct, array,
  map, and nested combination without relying on source-object lifetime.
- Header-only reads consume exactly the public header and no body section.
- Unknown, removed, mismatched, corrupt, and unsupported v3 inputs produce the
  same stable compatibility categories as equivalent v2 inputs.
- The compatibility worker remains read-only, bounded, cancelable, and free of
  DObject construction and dependency loading.

### Stage 4: Integrate Dual-Version Migration and Registry Behavior

- [ ] Dispatch DAST v2 and v3 to separate bounded readers after the common
  magic/version prefix.
- [ ] Retain frozen v2 success, compatibility, and corrupt-input fixtures.
- [ ] Make the registry, registry cache, thumbnails, package fingerprints, cook
  inputs, and audit records carry actual supported file versions rather than
  requiring the latest writer version.
- [ ] Invalidate the old registry cache schema once and prove mixed v2/v3
  content mounts rebuild and then warm-reuse correctly.
- [ ] Prove that load and scan never rewrite or dirty v2 packages.
- [ ] Prove that an authorized save of a cleanly loaded v2 package atomically
  publishes canonical v3 and preserves logical content and dependencies.
- [ ] Exercise move, delete, bundle save, rollback, dependency closure, unload,
  cook publication, and thumbnail-key behavior with mixed versions.
- [ ] Keep data-loss consent behavior identical for v2 and v3 compatibility
  reports.

#### Acceptance Gate

- A mixed content tree can scan, audit, load, edit, save, move, delete, cook,
  unload, restart, and warm-load without a forced bulk migration.
- Saving one v2 package changes only that package and the derived registry
  snapshot; unrelated authored files remain untouched.
- Interrupted or failed v3 publication restores the exact previous package and
  registry state.

### Stage 5: Qualify, Document, and Roll Out Authored V3 Content

- [ ] Run the AssetCore and CoreDObject native suites covering serialization,
  package lifecycle, registry, compatibility, DDC integration, and cooked
  package publication.
- [ ] Run Engine material, static-mesh, texture, level, thumbnail, workspace,
  and editor document tests that load authored packages.
- [ ] Record v2/v3 bytes and load/inspection timing for Default Material, the
  synthetic all-property fixture, the largest checked-in level, and every
  checked-in `.dasset` class.
- [ ] Confirm that v3 reduces Default Material parsing metadata and allocations,
  not only disk bytes.
- [ ] Resave authored repository packages to v3 after the dual-reader suite
  passes; retain only intentionally frozen v2 fixtures.
- [ ] Update `AssetPackages.md`, `Versioning.md`, asset data lifecycle guidance,
  and compatibility fixture documentation with the implemented lasting
  contract.
- [ ] Complete a successful full `all` build under the documented Agent Build
  Profile and run the verified editor executable against the resaved Engine and
  Sandbox content.
- [ ] Confirm the default material loads, creates its render proxy, renders in
  the editor, and survives a save/restart cycle from the verified build.

#### Acceptance Gate

- All required native, integration, content, and runtime validation passes from
  one coherent baseline.
- Default Material remains at or below 25 percent of its same-content v2 size
  and 16 KiB, with no general-purpose compression.
- Full editor startup and default-material rendering succeed against v3 Engine
  content, and the verified editor executable is recorded in the stage handoff.
- Lasting documentation describes the implemented format; the plan contains no
  unresolved contract that belongs in runtime documentation.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Default lifecycle | One CDO per constructible class, deterministic struct defaults, GC/root ownership, no package publication, no `PostLoad()` |
| Primitive wire values | Golden little-endian and canonical VarUInt fixtures for every scalar and logical opcode |
| Name/Type/Schema tables | Stable ordering, deduplication, recursive descriptors, invalid-ID and duplicate rejection |
| Delta serialization | Equal-to-default omission, no-delta override, changed-default inheritance semantics, non-null default reference forcing |
| Containers | Empty/non-empty/default arrays and maps, nested structs, deterministic map keys, recursion and count bounds |
| Object graphs | Main/inner objects, Outers, internal references, external dependencies, circular dependencies, unload guards |
| Compatibility | Unknown/removed fields, type mismatch, unknown class, retained payload, explicit data-loss consent for v2 and v3 |
| Header and registry | Exact header byte count, header-only I/O, mixed-version scan, cold rebuild, warm fingerprint reuse |
| Migration | V2 load without mutation, authorized v2-to-v3 save, atomic failure rollback, deterministic resave |
| Size | Default Material at most 25 percent of v2 and no more than 16 KiB; section-level byte report retained by tests |
| Parsing cost | Name entries interned once; no per-occurrence metadata string construction in full load or audit |
| Integration | Levels, materials, meshes, textures, thumbnails, editor documents, cook publication, move/delete/bundle operations |
| End to end | Successful full `all` build and verified editor startup/render/save/restart with resaved v3 content |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md); this plan does not
duplicate operational commands or profile paths.

## Definition of Done

- DAST v3 is the only authored writer format and DAST v2/v3 are both bounded,
  explicitly supported reader formats.
- Public registry metadata remains independently readable without body tables
  or value payloads.
- Reflected names, recursive types, and field schemas occur once per package,
  not once per object or struct instance.
- Default-relative object and struct encoding is backed by an explicit,
  documented default lifecycle and logical equality contract.
- Intrinsic math values contain no repeated type or component-name strings.
- Unknown-field retention, compatibility audit, and data-loss protection remain
  effective for v3.
- Default Material passes the size, parsing, round-trip, render, and restart
  gates without block compression.
- Mixed-version migration is non-destructive and occurs only on authorized
  save.
- Required tests and the full build pass, authored repository content is
  resaved, and lasting runtime documentation owns the final contract.

## Deferred Follow-ups

- Cooked-only unversioned property serialization tied to a frozen reflection
  schema fingerprint.
- Optional package or object-block compression after structural compaction,
  selected from measured I/O and CPU data rather than authored-file size alone.
- Frequency-based pooling for repeated ordinary `std::string` values.
- Memory-mapped Name/Schema tables and lazy object payload reads.
- Stable reflected field GUIDs for rename-aware compatibility without an
  explicit upgrader.
- Class-default dependencies that allow non-null object-reference defaults to
  remain implicit while preserving package dependency closure.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Data/Compatibility/`
- `Engine/Content/Materials/DefaultMaterial.dasset`
