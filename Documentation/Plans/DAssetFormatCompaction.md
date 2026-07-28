# DAsset Format Compaction Plan

Summary: Introduce a compact DAST v3 schema-symbol representation while preserving bounded header discovery, legacy package loading, and structure-upgrade payload semantics.

Last reviewed: 2026-07-28

Status: Active
Completed:

## Current Status

DAST v2 stores each schema string inline with a fixed-width `uint64` byte
length. Every object record repeats its qualified class name, and every field
record repeats its declaring qualified class, property name, and textual type
signature. Reflected struct payloads recursively repeat the same metadata.
The package header remains small and independently readable, but complete
package loading allocates and compares many duplicate strings.

A repository sample of 15 authored and test `.dasset` files totals 82,312
bytes and contains 1,245 occurrences of `Durin::`. Interning only the
top-level package metadata into a per-package table is estimated to save 7,944
bytes, or 9.7% of that sample, before accounting for repeated metadata inside
struct payloads. The estimated saving ranges from 4.5% for current static-mesh
packages to 20.2% for `Sandbox/Content/Levels/NewLevel.dasset`.

Fast whole-file compression reduced the sampled Sandbox packages by 71% to
87%, demonstrating high redundancy but not by itself selecting whole-body
compression. Current packages are small, registry discovery reads only their
headers, and a body codec would introduce separate dependency, allocation,
corruption, and resource-boundary concerns.

## Goal

Define and implement a deterministic DAST v3 representation that removes
repeated schema strings from object and nested-struct records, reduces package
size and parse-time string work, preserves all current compatibility and
inspection behavior, and provides evidence for whether an additional body
compression layer is warranted.

## Scope

- The authored and cooked `.dasset` DAST envelope owned by `AssetCore`.
- A deterministic package-local table for serialized schema symbols.
- Compact references to object classes, declaring types, property names, and
  type signatures in object and reflected-struct records.
- Bounded parsing, malformed-input rejection, and allocation limits for the
  symbol table and every symbol reference.
- DAST v2 read compatibility and DAST v3 writing.
- Package inspection, structure-upgrade reports, legacy-field payloads, asset
  registry metadata, committed asset fixtures, and authored content migration.
- Size, correctness, and load-cost measurements for representative packages.

## Non-Goals

- Shortening or changing reflected qualified names such as `Durin::AActor`.
- Assigning process-global or build-generated numeric IDs to reflected types
  or properties.
- Interning arbitrary authored string values, source paths, object-reference
  paths, or other user data by default.
- Changing reflection identity, property matching, object IDs, dependency
  semantics, DDC payload formats, DBLK, DMSH, or TXPL.
- Making `.dasset` text-mergeable or changing its Git/LFS policy.
- Requiring every old package to be rewritten before it can be loaded.
- Selecting a whole-body compression codec without measurement against the
  compact v3 representation.

## Design Decisions and Invariants

### Version and migration

- DAST v3 is a package-format change independent of the engine release
  version.
- New saves emit v3. The reader accepts both v2 and v3 so checked-in content,
  user projects, compatibility fixtures, and cooked packages remain loadable
  during migration.
- Loading v2 does not mark a package dirty solely because of its envelope
  version. An ordinary explicit save rewrites it as v3.
- Existing assets are migrated in one bounded content update after the v3
  reader and writer pass compatibility validation. Historical compatibility
  fixtures remain v2 when their purpose requires it.

### Header and discovery

- The header remains self-contained and uncompressed. It continues to expose
  magic, version, main asset class, dependencies, and object count without
  reading or allocating the object body.
- Header-only reads remain bounded by declared header data rather than package
  body size. The format records enough body sizing information to reject
  truncation and impossible declarations before full decoding.
- Registry snapshot compatibility continues to include the active DAST writer
  version. A stale snapshot is rebuilt non-fatally.

### Schema symbol table

- V3 owns one package-local schema-symbol table shared by object records and
  nested reflected-struct records.
- The table contains exact UTF-8 strings for object class names, declaring
  class or struct names, property names, and type signatures. Qualified names
  remain unchanged and diagnostic output remains readable after decoding.
- Symbols are deduplicated and ordered deterministically by bytewise value.
  Serialized output for equivalent package state must not depend on hash-table
  iteration order, allocation addresses, or reflection registration order.
- Records refer to symbols with bounded compact unsigned indices. Index width
  and encoding are fixed by the v3 format and validated before lookup.
- Empty symbols are represented explicitly when valid; no reserved index may
  silently change an empty reflected identity into a missing value.
- User-authored string and `FName` property payloads keep their existing
  self-contained value encoding unless later measurements justify a distinct
  value-string table.

### Type signatures

- V3 interns the existing canonical textual type signatures rather than
  replacing them with hashes or a new structural type bytecode.
- This keeps compatibility comparison and diagnostics exact while separating
  string deduplication from a future type-signature redesign.
- Hash-only identifiers are not accepted because collision handling and
  unavailable newer-schema diagnostics require the original identity.

### Legacy and unknown payloads

- `FAssetLegacyField`, `FAssetPackageField`, registered structure upgraders,
  and lightweight package inspection continue to expose decoded declaring
  names, field names, type signatures, and usable payloads without requiring
  callers to understand v2 or v3 wire details.
- V3 legacy and inspected fields retain their original payload bytes. They do
  not expand symbol references into a second canonical byte encoding during
  package loading.
- A shared immutable payload context owns the package format version and decoded
  schema-symbol table required to interpret those original bytes. Load reports,
  package inspections, and their retained fields share ownership of that
  context; no field borrows a symbol table from a temporary reader.
- `FAssetMigrationContext`, `FAssetPackageField::TryReadStruct`, and related
  AssetCore helpers resolve v2 inline metadata or v3 symbol references through
  the retained context. Registered upgraders must use these version-aware APIs
  for structured payloads rather than parsing package wire bytes directly.
- Copying or moving a load report, inspection, compatibility issue, or retained
  field preserves access to the same immutable context. Destroying the package
  reader or unloading the package does not invalidate retained payloads.
- Scalar payloads remain directly readable where their byte representation is
  version-independent. Structured and container payloads require the retained
  context even when a particular instance happens not to reference a symbol.
- Save rejection for unknown newer fields and explicit data-loss consent keep
  their current behavior.

### Failure and resource bounds

- Duplicate table entries, out-of-range symbol indices, invalid UTF-8 policy
  violations, oversized counts or strings, integer overflow, truncation, and
  trailing bytes fail with `CorruptFile`.
- Unsupported future format versions fail with `UnsupportedVersion`.
- Limits cover table entry count, individual symbol bytes, aggregate table
  bytes, object and field counts, payload sizes, and decompressed sizes if a
  later stage selects body compression.
- A corrupt v3 package is never partially published into the asset registry or
  loaded package cache.

### Whole-body compression

- String compaction is implemented and measured before selecting whole-body
  compression.
- Any later compression layer leaves the header readable without
  decompression, stores explicit codec and stored/uncompressed sizes, applies
  an expansion-ratio bound, and uses compression only when it produces a
  defined minimum byte saving.
- Compression must be deterministic for identical input and must not weaken
  complete-file validation or compatibility payload retention.

## Current Foundations and Gaps

| Area | Current foundation | Gap |
| --- | --- | --- |
| Package envelope | DAST v2 magic/version and bounded header reader | Reader accepts only the current exact version; no migration path |
| Object metadata | Field-tagged class, property, type, and payload records | Schema strings and 8-byte string lengths repeat inline |
| Struct metadata | Recursive field tables preserve compatibility | Nested payloads repeat qualified names and signatures |
| Compatibility | Unknown fields retain metadata and raw payload; risky saves require consent | Load reports must share immutable v3 decoding context with retained fields |
| Inspection | Header and complete field inspection avoid object construction | Inspection snapshots must own the same version-aware payload context |
| Registry | Cached format version and bounded header-read diagnostics | Snapshot compatibility and tests must advance with v3 |
| Validation | Round-trip, malformed header, unknown field, upgrader, and large-payload tests | No symbol-table determinism, bounds, corruption, or v2-to-v3 tests |
| Measurements | Initial sample shows 4.5% to 20.2% top-level savings | Nested-metadata saving and parse-time effects are not yet measured |

## Implementation Stages

### Stage 0: Freeze The V3 Wire Contract

Dependencies: none.

- [ ] Write the byte-level v3 header, symbol-table, object-record, field-record,
  and nested-struct layouts into the owning runtime documentation.
- [ ] Select the compact index representation and define its canonical encoding,
  maximum value, and invalid encodings.
- [x] Retain original legacy and inspected payload bytes with a shared immutable
  context containing their package format version and schema-symbol table.
- [ ] Define v2/v3 dispatch, v2 resave behavior, registry snapshot
  invalidation, and content-fixture migration policy.
- [ ] Record explicit maximum counts, aggregate byte limits, overflow checks,
  and error classifications.
- [ ] Add fixed byte fixtures or golden vectors that make accidental wire
  changes visible.

#### Acceptance Gate

- The v3 byte contract has no unresolved ownership, compatibility, ordering, or
  failure-policy decisions.
- A reviewer can derive header-only read boundaries and every symbol lookup
  from the documented layout.
- Golden vectors cover an empty optional symbol, repeated symbols, nested
  structs, and at least one external reference.

### Stage 1: Add Dual-Version Decoding

Dependencies: Stage 0.

- [ ] Separate shared logical package records from v2 and v3 wire readers.
- [ ] Keep the bounded file-header reader version-aware without consuming the
  body.
- [ ] Decode and validate the v3 schema-symbol table before resolving record
  indices.
- [ ] Decode nested struct metadata through the package symbol context while
  preserving self-contained inspection and migration behavior.
- [ ] Accept valid v2 packages without changing their represented object state.
- [ ] Reject malformed tables, duplicate entries, invalid indices, oversized
  declarations, truncation, and trailing data.

#### Acceptance Gate

- Existing v2 authored assets and compatibility fixtures load and inspect with
  unchanged logical results.
- Golden v3 packages load, inspect, and report compatibility issues correctly.
- Header reads for either supported version remain bounded independently of
  object payload size.
- Focused corruption tests cover every new count, index, and size boundary.

### Stage 2: Emit Deterministic V3 Packages

Dependencies: Stage 1.

- [ ] Gather schema symbols from complete logical object and nested-struct
  records before writing the body.
- [ ] Sort and deduplicate symbols using the frozen bytewise ordering.
- [ ] Emit v3 indices for object and field metadata while leaving authored
  value strings self-contained.
- [ ] Make serialization fail rather than emit an invalid or out-of-range
  symbol reference.
- [ ] Update reported format versions and registry snapshot compatibility.
- [ ] Verify repeated serialization of identical state produces identical
  bytes.

#### Acceptance Gate

- New saves and cooked `.dasset` publication emit valid v3 packages.
- V3 save-load-save round trips are byte-deterministic.
- Logical round trips cover scalar, string, name, enum, GUID, struct, array,
  map, internal reference, external reference, and nested combinations.
- Unknown-field reporting, registered upgrading, save refusal, and explicit
  data-loss saves retain their existing semantics.

### Stage 3: Measure And Decide Body Compression

Dependencies: Stage 2.

- [ ] Measure v2 and v3 byte size across representative level, material,
  static-mesh, Texture2D, TextureCube, cooked, and synthetic large packages.
- [ ] Separate header, schema-table, record metadata, authored payload, and
  container overhead in the report.
- [ ] Measure complete parse time, peak temporary memory, and allocation count
  in repeated warm-cache runs appropriate to the platform.
- [ ] Compare uncompressed v3 with available deterministic fast codecs,
  including codec overhead and expansion limits.
- [ ] Select one of: no body compression, thresholded body compression, or a
  separately planned compression stage. Record the quantitative threshold and
  rationale.

#### Acceptance Gate

- V3 demonstrates a positive aggregate size reduction on the representative
  corpus and does not regress any selected critical load metric beyond the
  recorded tolerance.
- The compression decision is evidence-backed and does not remain implicit.
- If compression is selected, its wire contract, dependency ownership,
  corruption behavior, and resource limits are added before implementation.

### Stage 4: Migrate Content And Finalize Contracts

Dependencies: Stages 2 and 3, plus any selected compression implementation.

- [ ] Resave current Engine and Sandbox authored packages through the v3 writer.
- [ ] Preserve deliberately old compatibility fixtures and label their expected
  version in tests or fixture documentation.
- [ ] Run the focused AssetCore package, registry, inspection, cooking, and
  structure-upgrade tests through the repository BuildTool.
- [ ] Run affected Engine asset round-trip and cook tests.
- [ ] Run a full `all` build if the change affects the user-visible editor or
  editor-authored content workflow.
- [ ] Update the owning asset-package documentation with the implemented
  contract and measured outcome.
- [ ] Validate the plan corpus, record completion evidence, and mark the plan
  complete.

#### Acceptance Gate

- Current authored content loads, resaves, unloads, and reloads as v3.
- Registry cold scans, warm snapshot reuse, package inspection, dependency
  loading, cooking, and compatibility reports pass.
- No ordinary current package requires v2-only writing.
- Long-lived v3 behavior and limits live in the owning runtime documentation.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Byte contract | Golden v2/v3 fixtures and deterministic byte comparisons |
| Header discovery | Large-body bounded header-read test for both versions |
| Symbol table | Ordering, deduplication, empty value, invalid index, duplicate entry, and aggregate-limit tests |
| Value round trip | Scalar, string, name, enum, GUID, struct, array, map, and nested-container package tests |
| References | Internal object IDs, external package dependencies, circular dependency coverage |
| Compatibility | Unknown field, incompatible type, registered upgrader, retained nested payload, save refusal, and data-loss consent tests |
| Tooling | `InspectAssetPackage`, registry cold/warm scans, move/delete contributors, and cooked publication tests |
| Determinism | Repeated save and equivalent-state construction produce identical v3 bytes |
| Size | Per-asset and aggregate v2/v3 corpus report with metadata/payload breakdown |
| Performance | Warm-cache parse time, peak temporary memory, and allocation comparison |
| Build and plans | Repository DurinDevTool validation and `.\DevTool.bat plan validate --scope all` |

Build and test execution must follow [Build And Run](../Development/Build/BuildAndRun.md)
and [Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- DAST v3 stores repeated schema identities once per package and uses validated
  compact references throughout object and nested-struct metadata.
- New saves emit deterministic v3 packages while supported v2 packages remain
  readable.
- Header-only registry discovery remains bounded and does not decode the body.
- Inspection, unknown-field retention, registered upgrading, and explicit
  data-loss protection preserve their logical behavior.
- Representative content demonstrates a recorded positive size reduction with
  acceptable parse-time and memory results.
- The whole-body compression decision is recorded with measurements.
- Current authored packages are migrated, required validation passes, and the
  implemented contract is documented in the runtime asset domain.

## Deferred Follow-ups

- Replace textual type signatures with a structural type bytecode only if
  profiling shows that the interned representation remains material.
- Add a separate authored-value string table only if real packages contain
  enough repeated user values to offset its indexing and lifetime costs.
- Introduce process-global reflection IDs only with a stable cross-build schema
  and redirect/versioning design.
- Add chunked or seekable body compression only when packages need partial
  object loading.
- Revisit Git LFS or asset-oriented version control only if package payloads or
  binary-conflict frequency outgrow the current policy.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Data/AssetStructureUpgrade/`
