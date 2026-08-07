# DAST V4 Measurement and Wire Contract Plan

Summary: Measure DAST v3 recursively and freeze a bounded deterministic DAST v4 byte contract with golden primitives and a test-only size-feasibility codec.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

- The [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
  entry gate is satisfied. Reflected struct operations, unified Archive
  serialization, and class-default-object lifecycle are complete; all 38
  production reflected classes have an explicit disposition and all 25 eligible
  classes own deterministic immutable defaults.
- This plan activates only measurement and byte-contract work. AssetCore still
  reads and writes DAST v3 exclusively; no production v4 reader, writer,
  migration edge, registry policy, or authored-content rewrite is active.
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

- [ ] Record the activation baseline, current tracked-corpus manifest, Default
  Material size/hash, exact test profile, and all relevant v3 grammar owners.
- [ ] Implement a bounded construct-free v3 accounting walker that attributes
  every top-level and recursively nested byte to a named category while reusing
  the existing logical type grammar rather than duplicating ad hoc type guesses.
- [ ] Prove byte conservation on synthetic fixtures, all tracked packages, and
  Default Material; record recursive metadata/value totals, unique/repeated
  names, types, schemas, fields, objects, container elements, references, maximum
  nesting, and estimated parse/allocation counts.
- [ ] Derive an explicit v4 byte budget for the public summary, each body section,
  retained descriptors, and values, with contingency below the 16-KiB controlling
  gate.
- [ ] Freeze the exact public header, section directory, section order, ids,
  bounds, UTF-8 policy, alignment, little-endian primitives, unsigned-LEB128
  domains, Name/Type/Schema/Object/Value layouts, custom-version placement,
  canonical orderings, and complete-consumption rules.
- [ ] Freeze every logical opcode and intrinsic layout, default omission and
  forced-override provenance, unknown descriptor closure, and malformed-input
  disposition.
- [ ] Audit current authored structs and custom Archive fields against the
  proposed logical grammar. Either prove reflected/default-safe coverage or
  raise the roadmap's evidence-gated custom-codec milestone before proceeding.
- [ ] Record a stage handoff with baseline commit, measured report, selected
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

### Stage 1: Implement golden primitives and the bounded v4 envelope model

- [ ] Implement test-only little-endian fixed-width and canonical unsigned-LEB128
  writers/readers with overflow-safe bounds, minimality checks, valid UTF-8, and
  exact remaining-byte accounting.
- [ ] Implement the frozen public summary and section directory model with a
  header-only validation path that never parses or allocates body tables.
- [ ] Add exact golden byte vectors for every primitive boundary, header field,
  entry kind, empty/nonempty table case, and section-directory form.
- [ ] Add mutation fixtures for truncation, overlong VarUInt, integer overflow,
  invalid UTF-8, invalid section kind, duplicate/out-of-order sections, overlap,
  extent overflow, unknown required sections, trailing bytes, and unconsumed
  data.
- [ ] Prove repeated encode/decode determinism and successful round-trip only for
  the test contract model; keep production `ReadPackageHeader`,
  `ReadPackageFile`, and `BuildAuthoredPackageBytes` on v3.
- [ ] Record the stage handoff and exact golden-fixture ownership.

#### Acceptance Gate

- Primitive and envelope goldens specify every byte and reject all noncanonical
  alternatives without relying on host endianness or C++ object layout.
- Header-only validation obtains the complete registry/redirect summary within
  frozen bounds and does not touch body table decoders.
- Section arithmetic is overflow-safe, exact, ordered, non-overlapping, and
  complete; production package APIs remain v3-only and their exact-byte tests
  remain unchanged.

### Stage 2: Implement canonical tables, schemas, values, and retention fixtures

- [ ] Implement the test-only Name, Type, Schema, custom-version, Object, and
  Value models with frozen canonical sorting, deduplication, id assignment,
  discovery freeze, and late-discovery failure.
- [ ] Implement reference encodings for every frozen logical opcode and intrinsic
  layout, including canonical Map keys, nested containers, internal/external
  references, enums, and length-delimited skip boundaries.
- [ ] Add synthetic default-relative fixtures for omitted defaults, changed
  defaults, explicit values equal to the current default, forced overrides,
  nested struct defaults, unavailable struct operations, and custom serializer
  failure.
- [ ] Add exact-retention fixtures that preserve unknown payload bytes together
  with their complete descriptor closure across known-table reordering and
  canonical rebuild.
- [ ] Add custom-version fixtures for ordering, duplicates, unknown GUIDs,
  unsupported values, discovery/emission mismatch, exact retention, and bounds.
- [ ] Add deterministic insertion-order permutations and malformed table/type/
  schema/id/cycle/container fixtures with exact failure categories.
- [ ] Record the stage handoff, including any contract correction before the
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

### Stage 3: Prove Default Material feasibility and parsing-cost reduction

- [ ] Connect the test-only reference encoder to unified Archive discovery and a
  test-owned immutable default oracle without adding a production v4 save path
  or production default-relative state.
- [ ] Encode the real current Default Material logical content, including the
  complete public summary, all sections, table/schema framing, override
  provenance, and any retained descriptor costs.
- [ ] Record exact section bytes, table cardinalities, override counts, omitted
  default counts, maximum nesting, digest, and modeled parse/allocation counts;
  compare them with the conserved v3 report.
- [ ] Prove identical bytes, digest, size report, and cardinalities across repeated
  runs and perturbed discovery/insertion order where the contract permits.
- [ ] Add a hard test budget for both 16,384 bytes and 20,659 bytes, with a
  diagnostic section breakdown on failure; verify no compression library or
  compressed block enters the fixture.
- [ ] Exercise a representative non-Material synthetic corpus so the size win is
  not achieved through asset-specific serialization.
- [ ] Record the stage handoff and the evidence needed to activate the
  default-relative-reflection child plan.

#### Acceptance Gate

- Complete test-reference v4 bytes for current Default Material are no larger
  than 16,384 bytes and no larger than 20,659 bytes, with neither compression nor
  omitted envelope/table/retention costs.
- The v4 report shows fewer per-occurrence metadata parses/materializations than
  v3 and attributes every output byte to a frozen section and logical owner.
- The fixture is generic, deterministic, uses the unified Archive contract, and
  introduces no Material-specific production codec or production v4 entry point.

### Stage 4: Qualify the contract and hand off to default-relative reflection

- [ ] Run the focused AssetCore package and Engine Material suites, changed-
  document validation, and the required full `all` build through the documented
  DurinDevTool workflow.
- [ ] Prove normal saving still emits exact deterministic DAST v3, normal
  production readers reject v4, `DevTool asset baseline` accepts the complete
  tracked corpus, and all tracked `.dasset` hashes remain unchanged.
- [ ] Move the lasting v4 byte contract, bounds, compatibility identities,
  default/forced-override semantics, custom-version model, and retention closure
  into the owning Runtime Asset documentation without claiming production v4
  support.
- [ ] Update the Compact Asset Serialization roadmap: complete this milestone,
  link its evidence, and mark Default-relative reflection ready to activate only
  if every exit gate passed.
- [ ] Complete this plan's status/checklists and record the final handoff with
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

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
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
