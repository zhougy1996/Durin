# DAST V4 Measurement and Wire Contract Plan

Summary: Measure DAST v3 recursively and freeze a bounded deterministic DAST v4 byte contract with golden primitives and a test-only size-feasibility codec.

Last reviewed: 2026-08-08

Status: Completed
Completed: 2026-08-08

## Current Status

- All stages and acceptance gates are complete. Recursive byte accounting
  conserves every byte of a synthetic nested fixture and all 17 tracked DAST v3
  packages without constructing objects. The frozen contract and its executable
  goldens cover the envelope, tables, values, defaults/provenance, custom
  versions, retained closures, ordering, bounds, and malformed-input behavior.
- The generic test-only codec produces a deterministic complete 10,869-byte
  Default Material: envelope/directory 79, Name 1,803, Type 62, Schema 107,
  Object 5, and Value 8,813. Its 105 names, 21 types, 6 schemas, 1 object,
  3 explicit overrides, zero unproven default omissions, depth 5, and XXH64
  `5955D6A8C777870C` are exact goldens. Modeled parse/allocation inputs fall from
  v3's 5,020/3,948 to 136/133 without compression or unowned bytes.
- The lasting qualified layout now resides in
  [Asset Packages](../Runtime/Assets/AssetPackages.md#frozen-dast-v4-wire-contract),
  and the [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
  records this milestone complete and Default-relative reflection ready to
  activate. AssetCore still reads and writes DAST v3 exclusively; no production
  v4 reader, writer, migration edge, registry policy, or authored-content
  rewrite is active.
- Final qualification began from baseline `0d4f6b56`. AssetPackageTests passes
  105/105, MaterialTests 78/78, StaticMeshTests 44/44, the asset baseline accepts
  all 17 current v3 packages, every activation SHA-256 remains unchanged, and
  the complete `all` build passes on `Win64-Debug-DurinEditor-Tests`.
- Activation baseline is `97a0d180`. The motivating
  `Engine/Content/Materials/DefaultMaterial.dasset` is 115,479 bytes at DAST v3
  with SHA-256
  `BFFA544C32BD8FFC00D1B9941EF7CD3CAFEDC3743DF7DF56F060620A42BEF229`.
  Its recorded same-content v2 baseline is 82,636 bytes, so the two exit gates
  are 20,659 bytes and 16,384 bytes; the 16-KiB gate is controlling and requires
  an 85.8-percent reduction from current v3.
- Current v3 stores raw fixed-width counts and lengths, repeats textual declaring
  types, field names, and recursive type signatures at each occurrence, and has
  no recursive byte-accounting fixture. The first stage must measure those bytes
  exactly before selecting the final v4 table and value encodings.

## Goal

Produce an evidence-backed, implementation-ready DAST v4 wire specification
without changing the production authored-package format. The completed plan
must provide:

- a bounded construct-free analyzer whose recursive categories conserve every
  byte of representative v3 packages;
- one exact public-header, section, table, schema, object, value, custom-version,
  default/forced-override, and unknown-retention contract;
- golden little-endian and canonical unsigned-LEB128 primitives plus malformed
  input cases; and
- a test-only reference codec proving that the current Default Material content
  fits both roadmap size gates without general-purpose compression.

The result must be precise enough for the next default-relative-reflection and
writer plans to consume without reopening byte-layout decisions.

## Scope

- Recursive DAST v3 accounting for the public header, dependencies, object
  records, field records, nested struct metadata, container framing, references,
  scalar/value bytes, and unclassified remainder.
- Stable measurement reports for the tracked corpus and a detailed Default
  Material report including section budgets, metadata cardinality, and repeated
  parse/allocation inputs.
- The exact DAST v4 public summary and body section directory for Name, Type,
  Schema, Object, and Value data, including custom-version placement.
- Explicit little-endian fixed-width primitives and canonical bounded unsigned
  LEB128 for counts, identifiers, lengths, offsets where selected, and version
  values where selected by the frozen contract.
- Canonical package-local identities, table order, deduplication, dependency
  order, object order, field order, Map-key order, section alignment, and
  complete-consumption rules.
- Logical encodings for scalar, bool, string, Name, Guid, enum, intrinsic math,
  struct, fixed array, Array, Map, hard reference, soft reference, raw bytes, and
  explicitly unsupported/custom states.
- Class-default and safe struct-default omission semantics, explicit
  forced-override provenance, and the rule for a loaded explicit value that
  equals the current default.
- GUID-keyed custom-version storage, discovery freeze, canonical order, bounds,
  unknown-version behavior, and exact-retention semantics.
- A retained descriptor-closure model that keeps unknown explicit payload bytes
  meaningful across canonical table reconstruction.
- Golden primitive fixtures, synthetic compatibility fixtures, and a test-only
  reference encoder/validator used only to prove the frozen contract and size
  budget.

## Non-Goals

- Adding a production DAST v4 reader or writer, changing `AssetVersion`, or
  allowing ordinary save/load/inspection/registry paths to accept v4.
- Implementing the next plan's production struct-default storage, recursive
  runtime default comparison, or forced-override state on loaded objects.
- Supporting a v3/v4 reader window, package migration, cache-version policy,
  registry rollout, bulk resave, or changing tracked `.dasset` bytes.
- General-purpose or block compression inside authored packages.
- Asset-specific Material serialization, cooked-only unversioned property
  layout, raw C++ memory serialization, async loading, memory mapping, hot
  reload, soft-reference redesign, or redirector redesign.
- Activating custom struct asset codecs without audit evidence that reflected
  fields plus repair cannot preserve durable authored state.

## Design Decisions and Invariants

### Execution and Ownership Boundary

- AssetCore test support owns the construct-free v3 accounting model, generic
  v4 contract model, primitive goldens, malformed fixtures, and synthetic
  compatibility cases. Production AssetCore gains no callable v4 load/save path
  in this plan.
- The Engine Material test layer owns the real Default Material feasibility case
  so AssetCore never depends upward on Engine or Material classes.
- The reference codec consumes the unified Archive discovery result and a
  test-owned immutable default oracle. It may model later runtime behavior but
  cannot publish default state, mutate class defaults, or become an alternate
  live-object serializer.
- Byte-only accounting and contract validation construct no asset object, invoke
  no serializer or callback, resolve no dependency, and perform no writable
  action. Only the Engine-layer feasibility fixture may load the known corpus to
  compare logical values against established defaults.

### Wire Boundary

- The package begins with the existing `DAST` magic and an explicit v4 version.
  The exact version representation, public-summary fields, section directory,
  and offset/length representation are frozen in Stage 0 and then covered by
  byte-for-byte goldens.
- The public summary remains independently readable with bounded work and
  without parsing or allocating body tables. It retains the information required
  by header-only registry and redirector validation: format version, asset class,
  entry kind, redirect destination, dependencies, and object count.
- Body sections have one canonical order. Every section has an exact bounded
  extent; overlap, overflow, duplicate section kinds, unknown required kinds,
  out-of-order entries, trailing bytes, and unconsumed bytes are invalid.
- All fixed-width multibyte values are explicitly little-endian. Counts,
  identifiers, and lengths use the frozen canonical bounded unsigned-LEB128
  representation; overlong, overflowing, or non-minimal encodings are invalid.
- Strings are valid UTF-8 with explicit byte lengths. The contract freezes
  whether normalization is forbidden or required; readers never silently
  normalize wire identities.
- Runtime sizes, offsets, hashes, pointers, registration order, allocator state,
  container capacity, C++ padding, and native endianness never enter the wire
  contract.

### Tables, Schemas, and Values

- Names, logical types, declaring schemas, fields, objects, and dependencies are
  package-local immutable tables with stable canonical identities. Discovery
  closes before emission; a late name, type, field, object, dependency, or
  custom version is an internal save failure.
- Type identities are structural and cycle-checked. Schema identities preserve
  qualified declaring type, stable field name, logical type, fixed-array
  dimension, and only the property flags that have authored meaning.
- Every logical opcode has one canonical payload. Length-delimited boundaries
  remain wherever skipping, bounded inspection, rollback, or exact unknown
  retention requires them.
- Maps are ordered by canonical encoded key bytes after duplicate logical keys
  are rejected. Ordering never depends on the runtime container iteration order.
- Intrinsic math values have explicit logical component layouts and never use
  raw GLM or C++ struct memory.

### Defaults, Versions, and Unknown Data

- An absent object field means the current immutable class-default value. An
  absent struct field means a default only when registered struct operations
  explicitly support deterministic construction and logical equality.
- An explicit override remains distinguishable from omission even when its value
  equals today's default. A load/resave cannot erase that intent merely because
  the current comparison is equal.
- A struct requiring an authored custom codec fails closed; this plan does not
  invent an implicit reflected fallback for durable opaque state.
- Custom versions are package-local GUID/value entries discovered before
  emission. Their exact ordering, numeric domain, bounds, unknown handling, and
  retention representation are frozen with the rest of the contract.
- Retaining an unknown explicit value retains its exact payload bytes and the
  complete descriptor closure needed to interpret every embedded table identity.
  Canonical resave cannot silently remap an opaque payload against a different
  table meaning; the contract must choose and prove either stable identities or
  a self-contained retained closure.

### Failure and Determinism

- Readers and validators fail before destination mutation on invalid UTF-8,
  invalid ids, recursive type cycles, duplicate or unordered entries, arithmetic
  overflow, impossible counts, excessive nesting, overlap, truncation, trailing
  bytes, or incomplete payload consumption.
- The reference encoder produces identical bytes, section sizes, table
  cardinalities, and digest across repeated runs and independent insertion
  orders.
- The feasibility result must meet both size gates with all section framing and
  descriptor-closure costs included. A projected or payload-only number does not
  pass.
- Production v3 byte behavior remains unchanged throughout this plan. Tests must
  prove that the normal writer still emits v3, normal readers reject v4, and no
  tracked package changes.

## Current Foundations and Gaps

### Foundations

- `DObject::Serialize(FArchive&)` is the only live complete-object entry, and
  authored discovery/emission already rejects manifest growth and object-graph
  mutation.
- `FArchiveLogicalTypeDescriptor` carries recursive logical type information for
  reflected and native fields; construct-free tools share the current v3 logical
  value grammar.
- `DClass` exposes immutable deterministic class defaults, and `DStruct` exposes
  explicit lifecycle, equality, reference, serialization, and repair
  capabilities.
- DAST v3 already has independent header reads, bounded body parsing, exact
  unknown field payload retention, compatibility inspection, atomic publication,
  deterministic writer tests, migration scaffolding, and 17 current tracked
  packages.
- The current capture path already separates discovery from payload emission and
  owns a logical captured package of dependencies, objects, fields, types, and
  raw value events.

### Gaps to Close

- `AssetVersion` is duplicated across package capture and package parsing, and
  current readers equate the latest writer with the only supported reader.
- `FByteWriter`/`FByteReader` currently copy native fixed-width values and use
  eight-byte string/count framing; they are v3 helpers rather than a portable v4
  primitive contract.
- No tool recursively attributes the metadata embedded inside v3 struct and
  container payloads, conserves total file bytes, or records metadata
  cardinality and parsing work.
- There is no package-local Name/Type/Schema model, section directory, custom
  version table, intrinsic layout catalog, forced-override record, or bounded
  retained descriptor closure.
- Existing compatibility tests retain v3 payload bytes but do not prove that
  opaque bytes remain meaningful when package-local table identifiers are
  rebuilt.
- Existing tests do not enforce a package size budget or distinguish envelope,
  tables, schema, object graph, override framing, and actual value bytes.

## Implementation Stages

### Stage 0: Measure v3 recursively and freeze the complete v4 contract

- [x] Record the activation baseline, current tracked-corpus manifest, Default
  Material size/hash, exact test profile, and all relevant v3 grammar owners.
- [x] Implement a bounded construct-free v3 accounting walker that attributes
  every top-level and recursively nested byte to a named category while reusing
  the existing logical type grammar rather than duplicating ad hoc type guesses.
- [x] Prove byte conservation on synthetic fixtures, all tracked packages, and
  Default Material; record recursive metadata/value totals, unique/repeated
  names, types, schemas, fields, objects, container elements, references, maximum
  nesting, and estimated parse/allocation counts.
- [x] Derive an explicit v4 byte budget for the public summary, each body section,
  retained descriptors, and values, with contingency below the 16-KiB controlling
  gate.
- [x] Freeze the exact public header, section directory, section order, ids,
  bounds, UTF-8 policy, alignment, little-endian primitives, unsigned-LEB128
  domains, Name/Type/Schema/Object/Value layouts, custom-version placement,
  canonical orderings, and complete-consumption rules.
- [x] Freeze every logical opcode and intrinsic layout, default omission and
  forced-override provenance, unknown descriptor closure, and malformed-input
  disposition.
- [x] Audit current authored structs and custom Archive fields against the
  proposed logical grammar. Either prove reflected/default-safe coverage or
  raise the roadmap's evidence-gated custom-codec milestone before proceeding.
- [x] Record a stage handoff with baseline commit, measured report, selected
  contract, working set, symbols, open questions, and validation. Update this
  plan before implementing Stage 1 if any measured result changes the proposed
  section model.

#### Acceptance Gate

- Recursive accounting assigns exactly the full byte length of every measured
  file with zero overlap, omission, or unexplained remainder and reports nested
  struct metadata separately from value data.
- Default Material has a reproducible v3 report and a concrete v4 section budget
  whose complete total is at most 16,384 bytes and at most 20,659 bytes.
- Every choice capable of changing golden bytes, table identity, compatibility,
  exact retention, defaults, bounds, or parser behavior is resolved in one
  internally consistent contract; no production reader or writer is added.
- The authored-struct audit either closes with existing reflected/repair
  semantics or activates the conditional custom-codec milestone with exact
  evidence. Later stages do not proceed through an unresolved opaque-state gap.

#### Stage 0 Evidence and Frozen Contract

The activation baseline is `97a0d180`; the active-plan baseline and Stage 0
starting point is `c2535eea`. Measurement and tests use the
`Win64-Debug-DurinEditor-Tests` Agent Build Profile and the focused command
`DevTool.bat test --target AssetPackageTests --filter
FPackageV3MeasurementTests.* --agent`. The v3 grammar owners are
`AssetPackageArchive.cpp` (`WritePackage`, `WriteField`, and `EncodeValue`),
`AssetSystem.cpp` (`ReadPackageHeader`, `ReadPackageFile`, and
`DecodeByteToolValue`), `AssetPackageValueCodec.h` (`FByteReader`,
`FByteWriter`, and type signatures), and `Archive.h`/
`Archive.cpp` (`FArchiveLogicalTypeDescriptor` and reflected field discovery).

##### Tracked v3 corpus

Every row below has zero overlap, zero omission, and zero unclassified bytes.
“Metadata” includes top-level field records and recursively repeated struct
headers/field records. “Values” includes container framing, references, scalar
bytes, and string/name framing and text. The remaining bytes are the public
envelope, dependency summary, and object records.

| Package | Bytes | Metadata | Values | Objects | Fields (top/nested) | Max depth |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `Engine/Content/Materials/DefaultMaterial.dasset` | 115,479 | 107,082 | 8,269 | 1 | 3 / 1,181 | 5 |
| `Engine/Content/Materials/ImportedSurface.dasset` | 115,479 | 107,082 | 8,269 | 1 | 3 / 1,181 | 5 |
| `Engine/Content/Models/Box.dasset` | 3,312 | 2,950 | 242 | 1 | 4 / 25 | 3 |
| `Engine/Content/Models/Sphere.dasset` | 3,318 | 2,950 | 245 | 1 | 4 / 25 | 3 |
| `Engine/Content/Renderer/DefaultStudioEnvironment.dasset` | 1,457 | 1,218 | 80 | 1 | 1 / 12 | 2 |
| `Sandbox/Content/Levels/NewLevel.dasset` | 9,926 | 8,308 | 747 | 9 | 40 / 65 | 3 |
| `Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter.dasset` | 37,897 | 35,039 | 2,329 | 1 | 5 / 377 | 5 |
| `Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter_alpha.dasset` | 44,997 | 41,615 | 2,774 | 1 | 5 / 449 | 5 |
| `Sandbox/Content/Models/VintageLighter/Meshes/vintage_lighter_1k.dasset` | 4,130 | 3,396 | 471 | 1 | 4 / 29 | 3 |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor.dasset` | 3,421 | 2,996 | 280 | 1 | 15 / 19 | 4 |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity.dasset` | 3,479 | 2,996 | 340 | 1 | 15 / 19 | 4 |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic.dasset` | 3,503 | 2,996 | 340 | 1 | 15 / 19 | 4 |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness.dasset` | 3,504 | 2,996 | 340 | 1 | 15 / 19 | 4 |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal.dasset` | 3,422 | 2,996 | 282 | 1 | 15 / 19 | 4 |
| `Sandbox/Content/Models/VintageLighter/vintage_lighter_1k_Import.dasset` | 14,583 | 11,531 | 2,880 | 1 | 12 / 100 | 4 |
| `Sandbox/Content/Textures/TEXCUBE_PureSky_512x512.dasset` | 6,214 | 5,719 | 353 | 1 | 8 / 51 | 4 |
| `Sandbox/Content/Textures/TEX_StoneHead.dasset` | 3,369 | 2,996 | 245 | 1 | 15 / 19 | 4 |

The tracked SHA-256 manifest at activation is:

| Package | SHA-256 |
| --- | --- |
| `Engine/Content/Materials/DefaultMaterial.dasset` | `BFFA544C32BD8FFC00D1B9941EF7CD3CAFEDC3743DF7DF56F060620A42BEF229` |
| `Engine/Content/Materials/ImportedSurface.dasset` | `72F5F51BCDF6B3A86E1F90F39339AFA4364B05CCCA88B2A8B18133F4F3567999` |
| `Engine/Content/Models/Box.dasset` | `DA278F907BB43A96C9ABFFAE284E3A30DEE9095B48884504E7CD05F1700038ED` |
| `Engine/Content/Models/Sphere.dasset` | `A97DAFE0F198DF857888532E33D7DAA5A765C071802F7F5C819DB4521A43045D` |
| `Engine/Content/Renderer/DefaultStudioEnvironment.dasset` | `3BC77392D732A44543097F6481319BA661F1ED7592B290DC458291A3F26A0100` |
| `Sandbox/Content/Levels/NewLevel.dasset` | `A58CE09F4C8598602B6E03499AE71E8C21D0DF7ADCD74FC077F2681B3E1195BF` |
| `Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter.dasset` | `2EC47BD171C990A5C8DAD8AB477969509DFD18BA5EE732982F8F389BC81259D0` |
| `Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter_alpha.dasset` | `A55A4BE92F3150BDD299F78612D5BF9FB082B6882AF994980AEEA9DCBA6ABF23` |
| `Sandbox/Content/Models/VintageLighter/Meshes/vintage_lighter_1k.dasset` | `C01C8E10178AC7367B6AA9C1CA1270C00BAC022B90EAAC8A214E25BC3F88C3FC` |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor.dasset` | `77B3EFFE835F43C36B7578A0FE1E6C2B893099FBE6AE0DDFFD9F179EB7F45E4D` |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity.dasset` | `D5AB9B23568ED373569DF6F072BFC56FC570E1DF0414897C0666DE57E7FE7B7B` |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic.dasset` | `EC5197D7AD69A77AB3B48E55FE2C83BCA377AB28BB7EABD35BF6D471A6AD1494` |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness.dasset` | `550F07D0B534B55020F4B4B28D28D803143E741164AEAF80781CB324A19E003E` |
| `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal.dasset` | `B9B8E5B61FC097113630C285CED90A3C41611F3CD6F6A266A35E0A12ECD0061D` |
| `Sandbox/Content/Models/VintageLighter/vintage_lighter_1k_Import.dasset` | `29E9A2E1E5F23D1DB88BDCBEF68D4296FAA1687027608572ED19FD803D471BA7` |
| `Sandbox/Content/Textures/TEXCUBE_PureSky_512x512.dasset` | `379367575E5F29D48B7158A70F5BB1CEE0E47AB12BFA5B53F08D7BF55305270D` |
| `Sandbox/Content/Textures/TEX_StoneHead.dasset` | `C69C0303172BB9B44169E89E72582F0468DD9DC31AC704B8C1C39F566A7391F1` |

Default Material contains 1 object, 3 object fields, 1,181 recursively nested
fields, 56 container elements, and 56 references. Its 3,780 metadata-string
occurrences collapse to 54 unique strings, and 1,184 type-signature occurrences
collapse to 20 unique signatures; the same 1,184 field occurrences collapse to
29 unique declaring-type/name/type schema identities. The bounded walk reaches
depth 5 and models 5,020 parse operations and 3,948 allocation inputs. Its exact
category totals are: 8-byte magic/version envelope, 33-byte public summary, 8-byte dependency
count, 79-byte object record, 298-byte top-level field records, 106,784-byte
nested struct records, 8-byte container framing, 56-byte references, 4,268-byte
scalars, and 3,937-byte string/name values.

##### Primitive and envelope contract

- A package starts with bytes `44 41 53 54`, then `04 00 00 00` (`uint32`
  little-endian version 4), a little-endian `uint32` public-summary byte length,
  and `uint8(5)` section count. The summary is followed by exactly five
  9-byte directory entries: `uint8 kind`, little-endian absolute `uint32 offset`,
  and little-endian `uint32 length`.
- Directory kinds and order are Name `0x01`, Type `0x02`, Schema `0x03`, Object
  `0x04`, and Value `0x05`. The first section begins immediately after the
  directory; every next offset equals the preceding offset plus length; the last
  extent equals file size. There is no alignment padding. Duplicate, missing,
  unknown, out-of-order, overlapping, overflowing, gapped, or trailing extents
  are invalid. The summary is bounded to 65,535 bytes and the complete package
  to 256 MiB.
- The public-summary payload is, in order: UTF-8 asset-class string, `uint8`
  entry kind (`0` asset, `1` redirector), UTF-8 redirect destination (empty only
  for an asset), VarUInt dependency count, canonical bytewise-sorted unique
  dependency strings, and VarUInt object count. It is completely consumed.
  Header-only validation reads this payload and the fixed directory but never
  parses or allocates body tables.
- `uint16`, `uint32`, `uint64`, IEEE-754 binary32/binary64, and GUID components
  are little-endian independent of host layout. VarUInt is unsigned LEB128 over
  `uint64`, at most 10 bytes, minimal, and rejected on overflow, unused high bits,
  or an overlong representation. Signed integer values use ZigZag followed by
  VarUInt. Counts and ids apply their narrower semantic bounds before allocation.
- A wire string is VarUInt byte length followed by valid shortest-form UTF-8.
  Surrogates, invalid sequences, embedded NUL, and lengths over 1 MiB are invalid.
  Unicode normalization is forbidden: writers preserve code points and identity
  comparison/order uses the exact UTF-8 bytes. Empty strings are allowed only in
  fields explicitly identified by this contract.

##### Tables, identities, and canonical order

- All table ids are one-based VarUInt; zero means absent only where explicitly
  permitted. Discovery closes before any bytes are emitted. Any late name, type,
  schema, object, dependency, or custom version is an internal save failure.
- Name entries are nonempty UTF-8 strings deduplicated and sorted by unsigned
  bytewise lexical order. The Name section is `count` followed by wire strings.
- Type entries are deduplicated by their complete structural descriptor bytes,
  sorted lexicographically by that self-contained descriptor, and assigned ids
  in sorted order. The Type section is `count`, then length-delimited records.
  The sorting/deduplication key is recursively `opcode`, directly encoded
  qualified-name UTF-8 where applicable, scalar parameters, and child keys; it
  never contains a package-local id. Emitted records replace names and children
  with their already frozen package-local ids.
  Record opcodes are Bool `01`, I8 `02`, I16 `03`, I32 `04`, I64 `05`, U8 `06`,
  U16 `07`, U32 `08`, U64 `09`, F32 `0A`, F64 `0B`, String `0C`, Name `0D`, Guid
  `0E`, Enum `0F`, Intrinsic `10`, Struct `11`, FixedArray `12`, Array `13`, Map
  `14`, HardRef `15`, SoftRef `16`, and Bytes `17`. Enum stores qualified-name id
  and integer storage opcode; Intrinsic stores its layout id; Struct stores its
  qualified-name id; FixedArray stores element-type id and nonzero dimension;
  Array stores element-type id; Map stores key- and value-type ids; references
  store expected-class name id (zero means `DObject`). Type graphs are validated
  after resolution and cycles are invalid.
- The Schema section starts with VarUInt custom-version count and entries sorted
  by numeric GUID tuple `(A,B,C,D)`, each four little-endian `uint32` components
  plus an unsigned `uint32`-domain VarUInt value. It then stores schema count and
  length-delimited schemas. Schemas sort by qualified-type UTF-8 bytes. Each is
  qualified-name id, field count, then fields sorted by field-name bytes, type
  descriptor bytes, and authored flags. A field is field-name id, type id, and
  authored-flags VarUInt. The only v4 authored-flags value is zero; unknown bits
  are invalid. Field ids are one-based positions within the canonical schema.
- The Object section is object count followed by length-delimited records
  `(outer-object-id, class-name-id, object-name-id)`. The package root is id 1
  with outer id zero. Remaining objects sort by canonical outer path, class-name
  bytes, then object-name bytes; duplicate sibling identities are invalid.
- Every table, record, and section is completely consumed. Limits are 1,048,575
  names/types/schemas/objects, 65,535 fields per schema, 4,096 dependencies, 256
  custom versions, 1,048,575 container elements, and nesting depth 64, further
  constrained by the package-size bound.

##### Value contract

The Value section is object count followed, in object-id order, by one
length-delimited block per object. A block is override count followed by records
sorted by `(schema id, field id)`. A known record is schema id, field id,
provenance (`00` explicit or `01` forced), value byte length, and the value. A
duplicate field or noncanonical order is invalid. Values have no repeated opcode;
the schema type determines exactly one payload:

- Bool is one byte `00` or `01`. Unsigned integers are VarUInt; signed integers
  are ZigZag VarUInt. F32/F64 use little-endian IEEE bits, preserve signed zero
  and infinities, and canonicalize every NaN to quiet `0x7FC00000` or
  `0x7FF8000000000000`.
- String is a wire string; Name is a Name id; Guid is `(A,B,C,D)` as four
  little-endian `uint32`; Enum follows its declared signed/unsigned integer
  storage. Bytes is VarUInt length plus exact bytes.
- Intrinsic layouts are: `01` FVector2 `(x,y)` F64; `02` FVector3 `(x,y,z)` F64;
  `03` FVector4 `(x,y,z,w)` F64; `04` FQuat `(w,x,y,z)` F64; `05` FTransform
  `(rotation FQuat, translation FVector3, scale FVector3)`; and `06` FLinearColor
  `(r,g,b,a)` F32. Raw GLM/C++ memory is never used.
- Struct is canonical changed-field count followed by `(field id, provenance,
  value length, value)` records under the named struct schema. FixedArray emits
  exactly its dimension elements without a count. Array emits count then elements.
  Map emits count then key/value pairs after duplicate logical keys are rejected
  and entries are sorted by the complete canonical encoded key bytes. Map keys
  are limited to Bool, integer, String, Name, Guid, Enum, and fixed-size Intrinsic.
- HardRef is tag `00` null, `01` plus internal object id, or `02` plus public
  dependency id. SoftRef is tag `00` null or `01` plus a Name id containing the
  canonical soft-object path. Unknown tags and zero/out-of-range ids are invalid.
- A class field omitted from its object block means the current immutable class
  default. A struct field may be omitted only when registered operations provide
  deterministic construction and logical equality. An unavailable operation or
  authored custom serializer without a proven v4 codec fails closed. A loaded
  explicit field remains provenance `00` even when equal to today's default;
  `01` records a serializer-forced override and is never removed by equality.

##### Versions, unknown retention, and failure behavior

Custom-version discovery is frozen with the other tables. Duplicate GUIDs,
unsupported known values, out-of-range values, and discovery/emission mismatch
fail before publication. Unknown GUID/value pairs are retained exactly and
re-emitted in canonical GUID order; a live load may proceed only when no known
codec declares that GUID required for interpretation.

Unknown explicit values use provenance `02`. Their value body is retained-closure
length and bytes, then payload length and exact payload bytes. The closure is a
self-contained mini Name/Type/Schema table using the same canonical encodings,
followed by its root schema and field ids. Its ids never refer to or get remapped
through package tables. Both closure bytes and payload bytes are copied exactly
on canonical resave; the closure must parse completely and resolve the root
descriptor before retention is accepted. Thus known-table reordering cannot
change opaque meaning.

Validation uses checked arithmetic and temporary immutable models. Invalid UTF-8,
nonminimal VarUInt, bounds, invalid ids, descriptor cycles, unordered/duplicate
records, overlap/gaps, truncation, unsupported opcodes/flags, impossible counts,
excess depth, trailing bytes, or incomplete length-delimited consumption fail
before destination mutation or output publication. There is no optional section
or opcode in v4; extensions requiring new kinds use a later format version.

##### Default Material v4 budget

| Owner | Budget bytes |
| --- | ---: |
| Envelope, public summary, directory | 256 |
| Name section | 1,536 |
| Type section | 512 |
| Schema section including custom versions | 2,048 |
| Object section | 64 |
| Value data | 10,240 |
| Retained descriptor closures inside Value | 512 |
| Contingency | 1,216 |
| **Complete package** | **16,384** |

The budget is also below the 20,659-byte 25-percent-v2 gate. The measured v3
logical data is only 8,269 bytes before default omission, leaving 2,483 bytes
inside the combined value/retention allocation and 1,216 bytes of package-level
contingency for the reference codec. Stage 3 must still prove actual complete
bytes; payload projection alone does not pass.

##### Authored-struct audit and handoff

The production tree has 21 generated reflected `DSTRUCT` declarations plus the
six registered intrinsic math structs above. No production trait sets
`bHasCompleteAuthoredFields = false` or `bWithSerializer = true`. Two import
record structs use `PostDeserialize` only to rebuild validated cached asset paths
from reflected durable strings; they retain complete authored fields. FVector3
specializes deterministic default construction but remains fully reflected.
Consequently all current durable state is representable by the frozen grammar
and repair semantics, and the evidence-gated custom-codec milestone remains
inactive.

Stage 1's working set is the three measurement support files, the
`AssetPackageTests` target declaration, and new test-only v4 primitive/envelope
support beside them. Key symbols are `MeasureDastV3`, `FV3PackageMeasurement`,
and `FPackageV3MeasurementTests`. Production `AssetVersion`,
`BuildAuthoredPackageBytes`, `ReadPackageHeader`, and `ReadPackageFile` remain
untouched. There are no open byte-layout questions. Focused validation passed
all three measurement tests on the profile above; full qualification remains a
Stage 4 gate.

### Stage 1: Implement golden primitives and the bounded v4 envelope model

- [x] Implement test-only little-endian fixed-width and canonical unsigned-LEB128
  writers/readers with overflow-safe bounds, minimality checks, valid UTF-8, and
  exact remaining-byte accounting.
- [x] Implement the frozen public summary and section directory model with a
  header-only validation path that never parses or allocates body tables.
- [x] Add exact golden byte vectors for every primitive boundary, header field,
  entry kind, empty/nonempty table case, and section-directory form.
- [x] Add mutation fixtures for truncation, overlong VarUInt, integer overflow,
  invalid UTF-8, invalid section kind, duplicate/out-of-order sections, overlap,
  extent overflow, unknown required sections, trailing bytes, and unconsumed
  data.
- [x] Prove repeated encode/decode determinism and successful round-trip only for
  the test contract model; keep production `ReadPackageHeader`,
  `ReadPackageFile`, and `BuildAuthoredPackageBytes` on v3.
- [x] Record the stage handoff and exact golden-fixture ownership.

#### Acceptance Gate

- Primitive and envelope goldens specify every byte and reject all noncanonical
  alternatives without relying on host endianness or C++ object layout.
- Header-only validation obtains the complete registry/redirect summary within
  frozen bounds and does not touch body table decoders.
- Section arithmetic is overflow-safe, exact, ordered, non-overlapping, and
  complete; production package APIs remain v3-only and their exact-byte tests
  remain unchanged.

#### Stage 1 Evidence and Handoff

Stage 1 began from `94d98f8b`. The test-owned
`PackageV4WireContract.h`/`.cpp` pair now owns explicit little-endian integer and
IEEE-bit encodings, canonical unsigned LEB128, ZigZag signed integers, bounded
wire strings with shortest-form UTF-8 validation, exact remaining-byte
accounting, public-summary encoding, fixed five-entry directory emission, and
immutable header validation. `DecodeHeader` parses and allocates only the bounded
registry/redirect summary; section bodies remain opaque spans whose contents are
never interpreted.

`PackageV4WireContractTests.cpp` is the sole owner of Stage 1 golden bytes. Its
nine tests pin fixed-width and floating encodings, every unsigned-LEB128 and
signed-ZigZag boundary, empty and nonempty section extents, all five section
kinds, complete header bytes, deterministic re-encoding, and a redirector with
sorted dependencies. Mutation fixtures reject truncation, nonminimal and
overflowing VarUInts, invalid/overlong/surrogate/NUL UTF-8, unconsumed summary
bytes, zero and unknown section kinds, duplicate/out-of-order kinds, overlap,
extent overflow, and trailing bytes. A separate opaque-body fixture proves
header-only validation does not enter table decoders.

The Stage 2 working set is these three test-only v4 files plus the existing
`AssetPackageTests` target declaration; the Stage 0 measurement files remain
available as evidence but need no initial modification. Key symbols are
`FWireWriter`, `FWireReader`, `EncodePublicSummary`, `EncodeEnvelope`, and
`DecodeHeader`. There are no open envelope or primitive-layout questions.
Focused validation passed all nine `FPackageV4WireContractTests.*`; the complete
`AssetPackageTests` target also passed all 93 tests. Documentation validation is
also clean across all active/completed/archived plans. Production `AssetVersion`,
`BuildAuthoredPackageBytes`,
`ReadPackageHeader`, and `ReadPackageFile` remain untouched and v3-only.

### Stage 2: Implement canonical tables, schemas, values, and retention fixtures

- [x] Implement the test-only Name, Type, Schema, custom-version, Object, and
  Value models with frozen canonical sorting, deduplication, id assignment,
  discovery freeze, and late-discovery failure.
- [x] Implement reference encodings for every frozen logical opcode and intrinsic
  layout, including canonical Map keys, nested containers, internal/external
  references, enums, and length-delimited skip boundaries.
- [x] Add synthetic default-relative fixtures for omitted defaults, changed
  defaults, explicit values equal to the current default, forced overrides,
  nested struct defaults, unavailable struct operations, and custom serializer
  failure.
- [x] Add exact-retention fixtures that preserve unknown payload bytes together
  with their complete descriptor closure across known-table reordering and
  canonical rebuild.
- [x] Add custom-version fixtures for ordering, duplicates, unknown GUIDs,
  unsupported values, discovery/emission mismatch, exact retention, and bounds.
- [x] Add deterministic insertion-order permutations and malformed table/type/
  schema/id/cycle/container fixtures with exact failure categories.
- [x] Record the stage handoff, including any contract correction before the
  Default Material feasibility run.

#### Acceptance Gate

- Every logical value kind and intrinsic layout has exact golden or round-trip
  coverage, and independent insertion orders produce identical bytes and ids.
- Default omission never loses explicit override intent; unsupported struct
  semantics fail closed with no implicit raw-memory or reflected fallback.
- Unknown explicit values retain byte-for-byte payloads and a demonstrably valid
  descriptor closure after canonical rebuild; no opaque table id changes
  meaning.
- Custom-version and table failures are bounded, deterministic, and occur before
  any destination mutation or output publication.

#### Stage 2 Evidence and Handoff

Stage 2 began from `c76cbbdd`. The new test-owned
`PackageV4ReferenceModel.h`/`.cpp` pair owns canonical discovery and freezing for
Name, Type, Schema, custom-version, Object, and Value sections. Structural type
keys are self-contained and recursive; emitted type records use only frozen
one-based ids. Decoding rebuilds a temporary model, resolves type graphs, rejects
cycles and invalid ids, re-freezes it, and requires byte-identical canonical
re-emission before publishing the result. A discovery registry rejects additions
after freeze.

The comprehensive permutation fixture contains 14 names, 29 deduplicated
structural types covering all opcodes `01` through `17` plus all six intrinsic
layouts, 2 schemas, 2 custom versions, and 2 objects. Reversing name, type,
schema, field, version, and object discovery order produces identical section
bytes and ids. The value model covers canonical NaNs, signed zero, every integer
family, strings, names, GUIDs, signed enums, all intrinsic layouts, structs,
fixed/nested arrays, byte arrays, canonical Maps, and null/internal/external hard
and soft references. Complete Value sections sort supplied objects by frozen id
and use exactly consumed length-delimited blocks.

Default-relative fixtures prove omission against the current immutable default,
changed-default emission, preservation of loaded-explicit provenance `00`,
forced provenance `01`, nested struct omission, and fail-closed behavior for
unavailable struct operations or unproven custom serializers. Provenance `02`
fixtures preserve both opaque payload and a fully parsed self-contained
Name/Type/Schema closure byte-for-byte across known-table reordering and
canonical rebuild. Custom-version fixtures cover numeric GUID ordering,
duplicates, unknown retention, unsupported known values, required unknown
versions, discovery/emission mismatch, `uint32` overflow, and the 256-entry
bound. Malformed fixtures cover table order/trailing data, opcode/flags/ids,
object outers and duplicate siblings, descriptor cycles, invalid Map keys,
container bounds/order, provenance, unconsumed records, reference ids, and depth.

`PackageV4ReferenceModelTests.cpp` owns the nine Stage 2 reference tests; Stage 1
golden ownership remains unchanged. No wire-contract correction was required.
The initial Stage 3 working set is the three reference-model files, the Stage 0
measurement header, and the `AssetPackageTests` target declaration; direct
Archive/default-oracle dependencies may be added only when the real Default
Material adapter requires them. Key symbols are `FreezeTables`,
`EncodeTableSections`, `DecodeTableSections`, `EncodeValue`,
`EncodeValueSection`, `EncodeOverrideBlock`, and `EncodeRetainedClosure`.
Focused validation passes all nine `FPackageV4ReferenceModelTests.*`; complete
`AssetPackageTests` also passes all 102 tests. Documentation validation is the
clean across all active/completed/archived plans. Production package APIs remain
untouched and v3-only.

### Stage 3: Prove Default Material feasibility and parsing-cost reduction

- [x] Connect the test-only reference encoder to unified Archive discovery and a
  test-owned immutable default oracle without adding a production v4 save path
  or production default-relative state.
- [x] Encode the real current Default Material logical content, including the
  complete public summary, all sections, table/schema framing, override
  provenance, and any retained descriptor costs.
- [x] Record exact section bytes, table cardinalities, override counts, omitted
  default counts, maximum nesting, digest, and modeled parse/allocation counts;
  compare them with the conserved v3 report.
- [x] Prove identical bytes, digest, size report, and cardinalities across repeated
  runs and perturbed discovery/insertion order where the contract permits.
- [x] Add a hard test budget for both 16,384 bytes and 20,659 bytes, with a
  diagnostic section breakdown on failure; verify no compression library or
  compressed block enters the fixture.
- [x] Exercise a representative non-Material synthetic corpus so the size win is
  not achieved through asset-specific serialization.
- [x] Record the stage handoff and the evidence needed to activate the
  default-relative-reflection child plan.

#### Acceptance Gate

- Complete test-reference v4 bytes for current Default Material are no larger
  than 16,384 bytes and no larger than 20,659 bytes, with neither compression nor
  omitted envelope/table/retention costs.
- The v4 report shows fewer per-occurrence metadata parses/materializations than
  v3 and attributes every output byte to a frozen section and logical owner.
- The fixture is generic, deterministic, uses the unified Archive contract, and
  introduces no Material-specific production codec or production v4 entry point.

#### Stage 3 Evidence and Handoff

Stage 3 began from `d03b618a`. The test-owned
`PackageV4Feasibility.h`/`.cpp` pair adds `AdaptArchiveLogicalType` and
`AddArchiveDiscoveredField`, mapping unified `FArchiveLogicalTypeDescriptor` and
`FArchiveFieldDescriptor` discovery directly into the Stage 2 reference
vocabulary. `BuildFeasibilityPackageFromV3` recursively reads the current
tracked logical content without object construction, applies a deterministic
immutable authored-presence oracle, freezes all tables, emits the five canonical
uncompressed sections, and builds the complete Stage 1 envelope. This remains a
test-only upper-bound feasibility path; it adds no production v4 entry point,
default state, Material branch, reader, or writer.

For `Engine/Content/Materials/DefaultMaterial.dasset`, the complete result is
10,869 bytes, 5,515 bytes below the 16,384-byte controlling gate and 9,790
bytes below the 20,659-byte v2-relative gate. Exact ownership is envelope and
directory 79; Name 1,803; Type 62; Schema/custom-version 107; Object 5; Value
8,813; retained unknown-descriptor closure 0 because the current package has no
unknown fields. The frozen cardinalities are 105 names, 21 structural types, 6
schemas, and 1 object. Three top-level overrides are emitted with known authored
provenance `00`; zero fields are omitted because v3 authored presence alone does
not prove equality with the current class default. This conservative oracle
preserves every logical value and gives an upper bound that a proven default
snapshot may only reduce. Maximum nesting remains 5. XXH64 is
`5955D6A8C777870C`. Envelope plus section sizes
conserve every byte, and decoding the real table sections followed by Value
validation consumes the generated package exactly.

The modeled v4 work is 136 parse operations and 133 allocation inputs versus
the conserved v3 report's 5,020 and 3,948. Repeated builds and reversed name,
type, schema, field, and object discovery produce identical bytes, report,
cardinalities, size, and digest. A separate non-Material corpus uses unified
Archive scalar, string, struct-array, map, fixed-array, hard/soft-reference, and
byte descriptors; a repeated nested-struct authored package also shrinks through
the same generic path. No compression dependency, flag, or block exists in the
fixture or frozen five-section envelope.

`PackageV4FeasibilityTests.cpp` owns the three Stage 3 tests. Focused validation
passes all three `FPackageV4FeasibilityTests.*`; complete `AssetPackageTests`
passes all 105 tests. The initial Stage 4 working set is this plan, the lasting
Runtime Asset package contract, the Compact Asset Serialization roadmap, and the
Stage 0 through 3 package test fixtures. Key symbols are
`BuildFeasibilityPackageFromV3`, `AdaptArchiveLogicalType`,
`AddArchiveDiscoveredField`, `DecodeTableSections`, and `ValidateValueSection`.
Stage 4 must still run the Engine Material suites, production v3/reject-v4 and
tracked-corpus hash gates, documentation validation, and the full `all` build;
no open wire or feasibility question remains.

### Stage 4: Qualify the contract and hand off to default-relative reflection

- [x] Run the focused AssetCore package and Engine Material suites, changed-
  document validation, and the required full `all` build through the documented
  DurinDevTool workflow.
- [x] Prove normal saving still emits exact deterministic DAST v3, normal
  production readers reject v4, `DevTool asset baseline` accepts the complete
  tracked corpus, and all tracked `.dasset` hashes remain unchanged.
- [x] Move the lasting v4 byte contract, bounds, compatibility identities,
  default/forced-override semantics, custom-version model, and retention closure
  into the owning Runtime Asset documentation without claiming production v4
  support.
- [x] Update the Compact Asset Serialization roadmap: complete this milestone,
  link its evidence, and mark Default-relative reflection ready to activate only
  if every exit gate passed.
- [x] Complete this plan's status/checklists and record the final handoff with
  baseline commit, working set, key symbols and decisions, open questions,
  focused/full validation, byte budgets, corpus hashes, and next-plan boundary.

#### Acceptance Gate

- Focused suites, documentation validation, and the full build pass from one
  coherent baseline; no tracked authored package changes.
- Lasting documentation and golden fixtures agree on every byte-affecting rule,
  and the roadmap records evidence for both size and parsing-cost outcomes.
- Production remains DAST v3-only. The next child plan can implement
  default-relative reflection without reopening wire-format, custom-version,
  override-provenance, or unknown-retention decisions.

#### Stage 4 Evidence and Final Handoff

Stage 4 began from `0d4f6b56`. Its modified working set is this plan,
[Asset Packages](../Runtime/Assets/AssetPackages.md), and the
[Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md);
the Stage 0 through 3 package fixtures remain the executable evidence. Key
symbols are `MeasureDastV3`, `BuildFeasibilityPackageFromV3`,
`AdaptArchiveLogicalType`, `AddArchiveDiscoveredField`, `DecodeTableSections`,
and `ValidateValueSection`. Production `AssetVersion`,
`BuildAuthoredPackageBytes`, `ReadPackageHeader`, and `ReadPackageFile` remain
untouched and v3-only.

The lasting Runtime contract now owns every byte-affecting layout, bound,
compatibility identity, default/forced/loaded-explicit provenance rule,
custom-version rule, and retained descriptor-closure rule qualified here. The
roadmap records this milestone completed and Default-relative reflection ready
to activate. There are no open wire, feasibility, custom-codec, or qualification
questions. The next child may add deterministic production struct-default
storage, recursive logical default comparison, forced-override state, and
no-delta policy; it must not add a v4 writer/reader or reopen the frozen wire
contract.

Qualification on `Win64-Debug-DurinEditor-Tests` passes `AssetPackageTests`
105/105, `MaterialTests` 78/78, and `StaticMeshTests` 44/44. The production tests
`AuthoredArchiveFreezesNativeFieldsReferencesAndFailures`,
`WriterEmitsVersionThreeRedirectSummary`, and
`HeaderReaderRejectsMalformedAndUnboundedDeclarations` prove deterministic v3
emission and production rejection of v4. `DevTool asset baseline` accepts all
17 tracked packages as current DAST v3; every SHA-256 equals the Stage 0
activation manifest and Git reports no authored-package diff. Changed-document
validation, all-plan validation, and the complete `all` build pass from the same
baseline.

The exact feasibility result remains 10,869 bytes, 5,515 below the 16,384-byte
controlling budget and 9,790 below the 20,659-byte same-content-v2-relative
budget. Section bytes remain 79/1,803/62/107/5/8,813 for envelope and directory,
Name, Type, Schema/custom versions, Object, and Value respectively; XXH64 remains
`5955D6A8C777870C`. Modeled parse/allocation inputs remain 136/133 versus v3's
5,020/3,948, with no compression and no unowned bytes.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| V3 accounting | Exact recursive byte conservation for synthetic fixtures, all tracked packages, and Default Material; nested metadata/value separation and stable cardinalities |
| Primitive bytes | Golden little-endian fixed-width and canonical unsigned-LEB128 boundaries; invalid UTF-8, overlong, overflow, truncation, and trailing-byte rejection |
| Public summary | Header-only class, entry kind, redirect, dependency, version, and object-count reads without body-table parsing or allocation |
| Sections and bounds | Canonical order, exact extents, no overlap/overflow/duplicates, required-kind policy, bounded counts/depth/string sizes, and complete consumption |
| Metadata tables | Stable deduplication and ids for Names, Types, Schemas, Fields, Objects, dependencies, and custom versions across insertion-order permutations |
| Value grammar | Scalars, bool, strings, Names, Guids, enums, intrinsics, structs, fixed arrays, Arrays, Maps, raw bytes, hard/soft and internal/external references |
| Defaults | Class and safe struct omission, changed defaults, nested deltas, forced overrides, explicit-equals-current-default provenance, and unsupported-operation failure |
| Compatibility | Unknown and removed fields, mismatches, exact payload plus descriptor-closure retention, canonical rebuild, and data-loss protection semantics |
| Versions | GUID canonical order, bounds, unknown/unsupported versions, discovery freeze, exact retention, and separation of latest writer from supported readers |
| Determinism | Exact bytes, section sizes, table cardinalities, and digest across repeated runs and independent discovery/insertion orders |
| Size and cost | Complete Default Material bytes within 16 KiB and 25 percent of v2; section diagnostics and reduced repeated metadata parsing/materialization |
| Boundary | Production writer remains exact v3, production readers reject v4, tracked corpus baseline/hashes unchanged, no migration or compressed block |
| Qualification | Focused AssetPackage and Material suites, plan/document validation, full `all` build, and compact final handoff |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Definition of Done

- Recursive v3 accounting conserves every byte and establishes a reproducible
  Default Material metadata/value and parsing-cost baseline.
- The complete DAST v4 header, section, table, schema, object, value,
  custom-version, default/forced-override, and unknown-retention contract is
  frozen in lasting documentation and executable goldens.
- A generic test-only reference codec produces deterministic complete Default
  Material bytes within both roadmap size gates without compression.
- Malformed, noncanonical, excessive, cyclic, overlapping, truncated, and
  retention-invalid inputs fail with bounded deterministic outcomes.
- Production authored-package APIs and the tracked corpus remain DAST v3-only
  and byte-stable; focused tests and the full build pass.
- The roadmap has evidence to complete this milestone and activate only the
  Default-relative reflection child plan next.

## Deferred Follow-ups

- Production deterministic struct-default storage, recursive logical default
  comparison, and forced-override state.
- Production v4 discovery and writer integration using the frozen contract.
- Production v4 reader, construct-free inspection, compatibility parity, and
  exact unknown retention.
- Mixed v3/v4 policy, registry/cache behavior, explicit atomic migration, corpus
  rollout, and removal of the temporary v3 edge.
- Custom struct asset codecs unless the Stage 0 audit activates their
  evidence-gated roadmap milestone.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
- [Class Default Object Lifecycle Plan](ClassDefaultObjectLifecycle.md)
- [Unified Archive Serialization Plan](Archive/2026-08/UnifiedArchiveSerialization.md)
- [Reflected Struct Operations Plan](Archive/2026-08/ReflectedStructOperations.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetPackageArchive.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageArchive.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageValueCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialSchemaAndEditingTests.cpp`
- `Engine/Content/Materials/DefaultMaterial.dasset`
