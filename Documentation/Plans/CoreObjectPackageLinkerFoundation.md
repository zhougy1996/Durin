# Core Object Package Linker Foundation Plan

Summary: Establish the CoreDObject linker model and canonical property semantics that will replace duplicated v7 readers without running the Engine runtime.

Last reviewed: 2026-08-31

Status: Active
Completed:

## Current Status

The aggressive linker replacement is selected and no implementation has
started. Stage 0 is the first open work. This plan is P0 of the
[Core Object Package Linker roadmap](../Roadmaps/CoreObjectPackageLinker.md)
and deliberately stops before defining or emitting DAST v8 bytes.

## Goal

Create one package-format-neutral linker vocabulary in CoreDObject, make
canonical reflected type and Map-key semantics reusable without Engine or
AssetRegistry ownership, adapt fully decoded DAST v7 packages into that model
for migration proof, and remove the first duplicated helpers. The completed
foundation must be useful to the later v8 writer/reader plans without becoming
a permanent wrapper around `FDecodedPackage`.

## Scope

- CoreDObject package index, recursive serialized type name, property tag,
  import/export descriptor, and linker-table value types.
- Checked one-based import/export/table access and deterministic logical
  identity independent of object memory addresses.
- CoreDObject canonical map-key token primitives shared by live reflected
  properties and decoded package values.
- An AssetRegistry-private v7 adapter that translates a validated
  `FDecodedPackage` into the CoreDObject linker model without constructing
  DObjects.
- Focused CoreDObject and AssetRegistry native tests, compile validation, and
  removal of dead or replaced helper implementations.

## Non-Goals

- Defining DAST v8 section bytes, offsets, compression, or bulk layout.
- Constructing or publishing a DPackage, invoking `PostLoad`, launching an
  editor/game, running Cook, or using an application-hosted test.
- Migrating the public Registry reference model in this plan.
- Porting compatibility audits, deprecated-route evidence, retained unknown
  resave, relocation, deletion, or redirector fix-up.
- Preserving source compatibility for private reader helpers.

## Selected Decisions

- CoreDObject owns the format-neutral model under its public `DObject`
  serialization surface and depends only on Core. It does not include
  AssetRegistry or Engine headers.
- The model uses a discriminated `FPackageIndex` for null/import/export
  identity. Raw signed or one-based indices do not escape table decoding
  adapters.
- Recursive serialized type identity is represented structurally rather than
  as `EPropertyGenFlags` plus separately formatted strings. Fixed arrays,
  dynamic arrays, maps, enums, structs, hard references, and soft references
  retain explicit parameters.
- A property tag carries field identity, structural type identity, provenance
  needed by the new linker, and a bounded payload view or owned payload. It does
  not point to `FProperty`, DObject memory, or AssetRegistry records.
- `FDecodedPackage` remains v7-specific and AssetRegistry-owned. The adapter
  translates it into the new model; CoreDObject does not acquire DAST opcodes or
  v7 reader diagnostics.
- Canonical map-key byte construction has one low-level CoreDObject writer.
  Live `FProperty` values and v7 decoded values provide typed inputs to it;
  neither reimplements integer ordering, float normalization, enum storage,
  strings, names, GUIDs, or struct-field framing.
- Unsupported v7 retained-unknown or custom payload conversion fails with an
  explicit adapter diagnostic. This plan does not silently discard it or add a
  permanent compatibility representation to the new model.
- Validation is construct-free. Engine may be compiled after private helper
  replacement, but no Engine-linked test binary or runtime is executed as an
  acceptance requirement.

## Implementation Stages

### Stage 0: Freeze the cut line and baseline

- [ ] Inventory every current definition and consumer of `FDecodedPackage`,
  type/schema table lookup, wire-to-property-kind mapping, canonical Map-key
  construction, nested reference routes, and package reference rewrite.
- [ ] Classify each consumer as foundation migration, later roadmap work, or
  deliberate retirement; record any exception to the roadmap preservation and
  retirement policy before changing code.
- [ ] Freeze a compact v7 fixture corpus covering scalar widths, enums,
  intrinsic math structs, nested structs, fixed/dynamic arrays, maps, internal
  and external hard references, soft references, malformed ids, and bounded
  decode failures.
- [ ] Record exact canonical token bytes and decoded logical identities for the
  fixture corpus, including signed boundaries, float positive/negative zero,
  representative NaN bit patterns, names, GUIDs, enum storage widths, and
  intrinsic layouts.
- [ ] Confirm the focused CoreDObject and AssetRegistry test targets and the
  compile-only Engine validation target through the repository test/build
  discovery workflow; do not select an application-hosted target.

#### Acceptance Gate

- Every duplicate helper and downstream consumer has one recorded disposition;
  the fixture corpus detects semantic drift; and validation can proceed without
  launching or loading through Engine.

### Stage 1: Introduce the CoreDObject linker vocabulary

- [ ] Add `FPackageIndex` with checked null/import/export construction and
  accessors; cover invalid, boundary, and round-trip cases without exposing raw
  table arithmetic to callers.
- [ ] Add structural serialized type values for scalar, enum, intrinsic,
  struct, fixed array, array, map, hard-reference, soft-reference, byte, and
  bulk-data kinds, including deterministic equality and ordering.
- [ ] Add pointer-free package import, export, property-tag, and linker-table
  records with explicit ownership and payload lifetime contracts.
- [ ] Provide checked table lookup and path reconstruction through the linker
  model; invalid indices return typed failure rather than assertions or partial
  output.
- [ ] Keep the public surface independent of DAST versions,
  `FAssetReferenceEdge`, Engine asset types, reflection catalogs, and live
  DObject pointers.

#### Acceptance Gate

- CoreDObject exposes one bounded, pointer-free linker model that represents
  the frozen fixture identities, depends only on Core, and passes focused unit
  coverage for type structure, table indexing, lookup failure, equality, and
  deterministic ordering.

### Stage 2: Centralize canonical type and Map-key semantics

- [ ] Extract one CoreDObject canonical token writer for type tags, sortable
  integers, normalized floating-point values, enum storage, strings, names,
  GUIDs, and nested struct field framing.
- [ ] Reimplement the existing live reflected-property
  `BuildCanonicalMapKeyToken` entry through the common writer without changing
  its accepted key set or token bytes.
- [ ] Add a decoded-value adapter that maps validated v7 type/value records into
  the same writer and returns an atomic output plus typed diagnostic on failure.
- [ ] Replace duplicated `BuildLedgerMapKeyToken` implementations and remove
  empty-package type-kind tricks, partial-output failure, and Engine-specific
  diagnostics from construct-free code.
- [ ] Express linker property compatibility through structural type identity;
  retain an adapter to `EPropertyGenFlags` only at legacy call sites scheduled
  for later removal.

#### Acceptance Gate

- Live reflected values and decoded fixture values produce byte-identical
  canonical tokens from one implementation; no AssetRegistry or Engine source
  contains a second sortable integer/float or intrinsic Map-key encoder; and
  failure leaves output unchanged with a stable diagnostic.

### Stage 3: Adapt validated v7 packages into linker tables

- [ ] Implement an AssetRegistry-private, construct-free adapter from
  `FDecodedPackage` to the CoreDObject linker model using checked table access
  and structural property tags.
- [ ] Translate object identity and Outer topology into exports, internal hard
  references into export indices, external hard references into imports, and
  soft references into explicit soft package identity.
- [ ] Preserve supported scalar, struct, array, map, byte, bulk, custom-version,
  and authored provenance facts required by later offline conversion.
- [ ] Reject invalid topology, ambiguous class/type identity, unsupported
  retained unknown data, and custom payloads that cannot be represented without
  constructing objects; publish no partial linker model.
- [ ] Prove deterministic adapter output across repeated runs and equivalent
  writer-discovery orderings.

#### Acceptance Gate

- Every supported frozen v7 fixture translates into one deterministic linker
  model with matching object graph, property type/value identity, and hard/soft
  package dependencies; unsupported fixtures fail atomically and identify the
  logical location; no DObject is constructed.

### Stage 4: Remove foundation-era duplication and qualify the seam

- [ ] Replace remaining shared type/schema lookup and property-kind consumers
  that are within this plan's scope with checked linker/model access; leave
  later Registry redesign consumers explicitly recorded for P3.
- [ ] Delete the unused Engine `ExtractValueReferences` copy and any helper that
  becomes unreachable after canonical-token migration.
- [ ] Add CoreDObject/AssetRegistry-only contract coverage proving that package
  dependencies and structural types can be obtained from the linker model
  without live reflection or Engine callbacks.
- [ ] Compile affected Engine consumers against temporary adapters without
  executing Engine-linked tests, editor/game binaries, Cook, or live package
  load.
- [ ] Update lasting CoreDObject serialization and module-ownership
  documentation only for contracts actually landed by this plan, then update
  roadmap status and pass documentation lifecycle validation.

#### Acceptance Gate

- CoreDObject owns the new linker vocabulary and canonical token mechanics;
  AssetRegistry owns only the temporary v7 translation; duplicate reader
  helpers selected by this plan are absent; affected modules build; focused
  CoreDObject and AssetRegistry tests pass; and no Engine runtime was launched.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Dependency direction | CoreDObject public/private sources include no AssetRegistry or Engine header and its module descriptor gains no such dependency. |
| Package indices | Null/import/export boundaries, invalid raw values, checked lookup, Outer path reconstruction, and deterministic identity pass focused tests. |
| Structural types | All frozen scalar/container/reference shapes compare and order deterministically without formatted-string authority. |
| Canonical tokens | Live and decoded values match exact frozen bytes for supported keys; invalid keys and layouts leave output unchanged. |
| v7 adaptation | Supported fixtures preserve graph, type/value, provenance, custom-version, and dependency facts; unsupported data fails atomically. |
| Construct-free boundary | Tests observe no DObject construction, reflection callback, PostLoad, package publication, Engine runtime, or Cook execution. |
| Duplication retirement | Repository searches find no second token encoder and no unused Engine nested-reference extraction copy after Stage 4. |

## Related Code and Documentation

- [CoreDObject module](../../Engine/Source/Runtime/CoreDObject)
- [AssetRegistry object-stream API](../../Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/ObjectStream.h)
- [AssetRegistry object-stream reader](../../Engine/Source/Runtime/AssetRegistry/Private/AssetObjectStreamReader.cpp)
- [Engine package object-stream reader](../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageObjectStreamReader.cpp)
- [Asset reference records](../../Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/References.h)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Core Object Package Linker roadmap](../Roadmaps/CoreObjectPackageLinker.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)
