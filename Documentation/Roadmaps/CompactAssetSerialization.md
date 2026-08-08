# Compact Asset Serialization Roadmap

Summary: Coordinate the reflection foundations, deterministic DAST v4 format, multi-version migration, and authored-content rollout needed to compact asset packages without weakening compatibility guarantees.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

- The bounded
  [DAST V4 Measurement and Wire Contract Plan](../Plans/DASTV4MeasurementAndWireContract.md)
  completed on 2026-08-08. Its recursive accounting conserves every byte of all
  17 tracked v3 packages, and its generic test-only reference codec produces a
  complete deterministic 10,869-byte Default Material with XXH64
  `5955D6A8C777870C`. That is 5,515 bytes below the 16-KiB gate and 9,790 bytes
  below the same-content-v2-relative gate; modeled parse/allocation inputs fall
  from 5,020/3,948 to 136/133 without compression. The lasting
  [frozen DAST v4 wire contract](../Runtime/Assets/AssetPackages.md#frozen-dast-v4-wire-contract)
  now owns the qualified layout and semantics. AssetCore still reads and writes
  only DAST v3, all 17 activation hashes remain unchanged, and the repository
  baseline rejects every other package format or incompatible schema.
- The bounded
  [DAST V4 Default-Relative Reflection Plan](../Plans/DASTV4DefaultRelativeReflection.md)
  activated on 2026-08-08. It owns production struct-default storage, tri-state
  recursive logical identity, class/default-subobject baselines, a wire-neutral
  logical delta plan, known-field override provenance, and no-delta policy. It
  consumes the frozen v4 bytes without reopening wire, custom-version, or
  retained-closure decisions.
- The completed [Asset Redirectors Refactor Plan](../Plans/Archive/2026-08/AssetRedirectors.md)
  owns DAST v3's bounded registry-entry kind and redirect-destination header
  summary. Compact serialization therefore starts at v4 and uses a temporary
  exact v3-to-v4 migration edge; it does not absorb redirector implementation.
- The previous monolithic implementation plan has been converted into this
  roadmap. Later production work remains deferred and can be activated only as
  bounded child plans when the preceding exit and entry gates are satisfied.
- `Engine/Content/Materials/DefaultMaterial.dasset` remains the motivating
  corpus. Its 2026-08-05 same-content v2 baseline is 82,636 bytes and its current
  v3 package is 115,479 bytes, 39.7 percent larger than that baseline. The
  16-KiB gate therefore requires an 85.8-percent reduction from current v3.
  Recursive measurement attributes 106,784 bytes to nested struct
  metadata/framing and 8,269 bytes to logical value/container/reference data;
  this evidence selected and qualified the frozen package-local table encoding.
- General-purpose compression proves that the bytes are redundant, but it does
  not remove repeated parsing, allocation, or schema reconstruction.
- The [Reflected Struct Operations](../Plans/Archive/2026-08/ReflectedStructOperations.md)
  prerequisite is complete. Struct lifecycle, logical equality, reference
  collection, runtime Archive customization, post-deserialize repair, and
  authored fail-closed semantics are now declarative. This satisfies a
  foundation gate but does not define DAST v4 bytes.
- The [Unified Archive Serialization Plan](../Plans/Archive/2026-08/UnifiedArchiveSerialization.md)
  prerequisite is complete. All live complete-object state transfer now uses
  `DObject::Serialize(FArchive&)`; purpose-specific Archives share one semantic
  reflected-value layer, and DAST v3 package Archives share their logical value
  grammar with construct-free inspection and rewrite tooling. The migration
  preserved the v3 wire contract and did not activate a v4 reader, writer, or
  migration.

## Outcome

Deliver a deterministic authored-package format that stores reflection metadata
once per package, encodes object and struct values relative to declared defaults,
preserves bounded compatibility inspection and unknown-field retention, and
migrates DAST v3 content only through explicitly authorized tooling.

The canonical Default Material package must become no larger than 25 percent of
its same-content v2 baseline and no larger than 16 KiB without relying on block
compression.

## Scope

- Reflection and default-value foundations required by sparse struct and object
  encoding.
- A versioned DAST v4 public header and deterministic Name, Type, Schema,
  Object, and Value sections.
- Compact canonical scalar, container, reference, enum, and intrinsic math
  encodings.
- Bounded DAST v3/v4 loading during the migration window, inspection,
  compatibility reporting, registry caching, and explicit migration.
- Determinism, malformed-input, size, parsing-cost, integration, and authored-
  content rollout validation.

## Non-Goals

- Starting DAST v4 implementation before an explicit child plan is activated.
- General-purpose compression inside the authored `.dasset` envelope.
- Cooked-only unversioned property serialization tied to a frozen runtime
  reflection layout.
- Asset-specific material serialization, async loading, memory mapping, soft
  references, redirects, or hot reload.
- Silent bulk migration, automatic compatibility repair, or writable actions in
  compatibility inspection tools.

## Program Decisions and Invariants

### Authored Format Boundary

- DAST v4 remains a field-tagged authored format. Package-local tables replace
  repeated metadata, but stable declaring-type, field, and logical-type
  identities remain available for compatibility inspection.
- Live-object v4 discovery, save, and load use the unified
  `DObject::Serialize(FArchive&)` contract established by the completed prerequisite.
  Byte-only inspection, compatibility, reference, and rewrite tools consume the
  same logical field codecs without constructing objects or invoking callbacks.
- The package summary remains independently readable without allocating or
  parsing body tables.
- All fixed-width multibyte values use explicit little-endian encoding. Counts,
  identifiers, and lengths use canonical bounded unsigned LEB128.
- Runtime sizes, offsets, hashes, property pointers, registration order, and C++
  padding never enter the wire contract.
- Every value record remains length-delimited where skipping or exact unknown-
  field retention requires it.

### Default and Struct Semantics

- Object overrides are relative to one deterministic default object owned by
  each constructible reflected class.
- Struct overrides are relative to a deterministic default value only when the
  registered struct operations explicitly support the required initialization,
  destruction, and logical comparison contracts.
- `DSTRUCT()` by itself must not claim default construction, copying,
  destruction, equality, reference collection, or custom serialization support.
- A struct requiring an authored custom codec cannot silently fall back to
  reflected-field encoding. Saving fails closed until a codec with dependency,
  inspection, version, and migration semantics is registered.
- Default changes intentionally affect fields omitted from older default-
  relative packages. Preserving an old semantic default requires an explicit
  override or a version migration.

### Determinism and Compatibility

- Dependencies, names, types, schemas, fields, objects, overrides, and map keys
  have explicit canonical orderings independent of process state.
- DAST v3 remains bounded and readable only during the reviewed v4 migration
  window. Scanning and loading never rewrite or dirty a package; once the
  tracked corpus is v4, the v3 reader and completed migration edge are removed.
- Unknown explicit overrides retain exact payload bytes and file-side type
  descriptors. Explicit data-loss consent remains required before overwriting
  incompatible authored content.
- Writers use discovery followed by frozen-table emission; late discovery is an
  internal save error.
- Readers reject overlong encodings, invalid UTF-8, duplicate or unordered
  entries, invalid IDs, recursive type cycles, overflow, overlap, trailing
  bytes, and unconsumed payload data.

## Current Foundations and Gaps

### Foundations

- DAST v3 separates bounded public-header reads from complete package loading
  and atomic publication without introducing compact body sections.
- Existing reflection metadata exposes qualified class, struct, enum, and field
  identities plus recursive property kinds.
- Compatibility inspection, data-loss consent, registry fingerprints, frozen
  fixtures, and package tests provide a baseline that v4 must preserve.
- `DStruct` exposes immutable operation capabilities, managed aligned value
  storage, recursive logical equality, and transactional Archive/authored
  loading. `FPropertyValueSnapshot` uses the same equality and roots reflected
  plus declared hidden references.
- `DClass` exposes immutable class defaults with stable eligibility diagnostics,
  construction-purpose identity, logical parity across the production class
  inventory, live-only query filtering, and derived-first GC/module release.

### Gaps

- Struct schemas and textual type signatures repeat at each occurrence.
- Package-version handling conflates the latest writer with the supported reader
  set. The v3 value is also duplicated across package saving, loading,
  compatibility, and migration code.
- DAST v3 has no package-local GUID-keyed custom-version table. The v4 wire
  contract must still choose its version-table representation, canonical order,
  bounds, unknown-version behavior, and exact-retention policy.
- Exact unknown-payload retention is not yet reconciled with package-local table
  identifiers. The v4 contract must define a retained descriptor closure or a
  stable/remappable identity rule so canonical table rebuilding cannot invalidate
  opaque retained bytes.
- Default-relative resave does not yet have a provenance rule for an explicit
  override whose value equals the current default. The v4 contract must define
  how a forced override remains distinguishable from an omitted default.
- Existing tests do not enforce package metadata cardinality, section byte
  accounting, or an authored-package size budget.

## Milestone Map

| Milestone | Kind | Entry gate | Deliverable and exit gate | State |
| --- | --- | --- | --- | --- |
| Reflected struct operations | External prerequisite | None | Declarative lifecycle, equality, reference, and serialization semantics are fail-closed; every current authored struct is audited and the full build passes | Completed 2026-08-05 |
| Unified Archive serialization | Required prerequisite plan | Reflected struct operations complete | One live `Serialize` entry, purpose-specific Archives, exact DAST v3 adapters, and shared construct-free field codecs pass focused, full-build, and editor qualification | Completed 2026-08-07 |
| Class default object lifecycle | Required prerequisite plan | Struct-operations and unified-Archive prerequisites complete; program explicitly scheduled | Every eligible concrete reflected class has one immutable deterministic default object; template construction is free of runtime publication, GC/shutdown ownership is explicit, and constructor/default parity passes full qualification | Completed 2026-08-08 |
| [V4 measurement and wire contract](../Plans/DASTV4MeasurementAndWireContract.md) | Required child plan | Class-default-object lifecycle exit gate passed | Recursive v3 accounting, a frozen bounded v4 byte contract, golden primitives, and a test-only feasibility fixture demonstrate the size target without a production reader or writer | Completed 2026-08-08 |
| [Default-relative reflection](../Plans/DASTV4DefaultRelativeReflection.md) | Required child plan | V4 default/override semantics frozen and measurement/wire-contract exit gate passed | Class defaults and safe struct defaults drive recursive logical equivalence, forced-override provenance, and no-delta policy under focused lifecycle tests | Active 2026-08-08 |
| Deterministic v4 writer | Required child plan | Default-relative reflection exit gate passed | Discovery freezes every referenced table entry and version; canonical emission is byte-deterministic and meets both Default Material size gates | Proposed, deferred |
| V4 reader and compatibility | Required child plan | Writer fixtures and frozen schema model available | Bounded v4 loading and construct-free inspection preserve unknown descriptor closures and pass malformed-input, rollback, and compatibility parity suites | Proposed, deferred |
| Mixed-version migration | Required child plan | V3/v4 readers and v4 writer stable | Latest-writer and supported-reader policy is separated; registry, cache, and explicit atomic v3-to-v4 migration pass mixed-corpus and rollback validation | Proposed, deferred |
| Qualification and rollout | Required child plan | Mixed-version migration exit gate passed | Full validation and editor load/render/save/restart pass before tracked authored content is explicitly resaved and the temporary v3 edge is retired | Proposed, deferred |
| Custom struct asset codecs | Conditional child plan | A current or future struct audit proves reflected fields plus repair cannot represent durable authored state | Versioned codecs provide dependency discovery, inspection, exact retention, and migration semantics, or the milestone is explicitly dispositioned by audit evidence | Evidence-gated; not currently required |

## Child Plan Boundaries

### Reflected Struct Operations

Owns `DStruct` capability declaration, generated registration, lifecycle call
contracts, logical equality hooks, custom runtime Archive dispatch, post-load
repair, hidden reference collection, and authored-serialization fail-closed
policy. It does not define DAST v4 bytes or class default objects.

### Unified Archive Serialization

Owns the single live-object `DObject::Serialize(FArchive&)` contract, Archive
purpose and capability state, structured fields, semantic value operations,
serialized-reference discovery, runtime object-graph v2, DAST v3 live-object
adapters, and the shared field codec used by construct-free tooling. It must
preserve exact DAST v3 bytes and does not define v4 tables, default-relative
encoding, custom-version wire storage, mixed-version migration, or content
rollout.

The completed migration also fixes constraints for later child plans: authored
writers use discovery followed by frozen emission; native durable state requires
stable named logical fields; package tools cannot depend on object construction
or callbacks; unknown payloads must remain exact; and package Archives cannot
persist GUID-keyed custom versions until v4 defines their table. Existing DAST
v3 inspection bounds remain measured operational inputs rather than a proposed
v4 encoding: reference extraction accepts at most four container levels,
100,000 occurrences per package, 1,000,000 per snapshot, 1 MiB paths and Map-key
tokens, and 4 KiB display paths.

### Class Default Object Lifecycle

The completed
[Class Default Object Lifecycle Plan](../Plans/ClassDefaultObjectLifecycle.md)
established one immutable default object per constructible reflected class, explicit
template construction purpose and flags, base-before-derived creation after
reflection finalization, constructor/default parity, runtime-side-effect
separation, global object-query filtering, GC retention, and deterministic
shutdown before owning modules unload. It does not define DAST v4 bytes, struct
default-relative encoding, override provenance, or package migration.

### [V4 Measurement and Wire Contract](../Plans/DASTV4MeasurementAndWireContract.md)

Completed recursive byte-accounting fixtures, section kinds, opcodes, bounds,
canonical ordering, intrinsic logical layouts, default/forced-override wire
semantics, custom-version storage, retained unknown descriptor closure, and the
exact compatibility model. The generic test-only reference codec qualifies the
complete Default Material size and parsing-cost gates. Production reader and
writer activation remains outside this completed milestone.

### [Default-Relative Reflection](../Plans/DASTV4DefaultRelativeReflection.md)

Consumes immutable class defaults and owns deterministic struct default storage,
tri-state recursive logical identity, class/default-subobject baseline pairing,
a wire-neutral delta plan, forced/loaded-explicit override provenance, and
no-delta policy. It consumes the completed struct-operations contract rather
than adding alternate lifecycle callbacks or comparison logic inside AssetCore.

### Writer, Reader, Migration, and Rollout Plans

Each later plan owns only its named production slice and focused validation.
The writer does not alter registry behavior; the reader does not resave content;
the migration plan does not bulk-resave; and repository content changes occur
only in the final rollout plan after multi-version validation passes.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Struct semantics | Every serialized struct has explicit usable capabilities or a deterministic unsupported diagnostic |
| Default lifecycle | One deterministic default object per constructible class and safe struct default storage |
| Wire primitives | Golden little-endian and canonical VarUInt fixtures for every logical opcode |
| Metadata tables | Stable ordering, deduplication, recursive descriptors, and invalid-reference rejection |
| Delta encoding | Default omission, forced override, changed-default semantics, and nested struct coverage |
| Containers and graphs | Canonical maps, arrays, Outers, cycles, and internal/external references |
| Compatibility | Unknown and removed fields, mismatches, exact payload retention, and data-loss consent for v2, v3, and v4 |
| Registry and migration | Header-only reads, mixed versions, cache reuse, authorized atomic migration, and rollback |
| Size and parsing cost | Default Material within both size gates and no per-occurrence metadata materialization |
| End to end | Required native suites, full build, editor load/render/save/restart, and deterministic resave |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Risks and Control Gates

- **Constructor side effects:** the completed default-object audit and production
  parity sweep cover deterministic construction, owned inners, process state,
  I/O, registration, publication, and external references. A future reflected
  class must receive an explicit disposition before it can enter v4 measurement.
- **Opaque custom state:** a custom Archive serializer is not automatically an
  authored-package codec. The conditional codec plan is mandatory if reflected
  fields plus post-load repair cannot preserve durable state.
- **Compatibility regression:** v4 cannot become the writer default until
  equivalent v3/v4 compatibility categories and exact unknown payload retention
  are proven during the migration window.
- **Opaque identifiers:** unknown v4 payload bytes cannot be retained exactly if
  they embed table ids whose meaning changes during canonical resave. The wire
  contract must close this before any production writer work begins.
- **Override intent loss:** automatically omitting a loaded explicit value that
  equals today's default can change its behavior after a future default change.
  Forced-override provenance must be part of the frozen contract rather than a
  writer-only heuristic.
- **Nominal compression only:** rollout is blocked unless measurements show
  reduced metadata parsing and allocation as well as reduced bytes.
- **Migration blast radius:** scanning and loading remain read-only; only an
  explicitly authorized save may migrate one package.

## Completion Criteria

- Every required milestone has completed its exit gate, and the conditional
  custom-codec milestone is either completed or dispositioned by audit evidence.
- DAST v4 is the only authored reader and writer after the tracked corpus is
  migrated and the temporary v3 compatibility path is retired.
- Default Material satisfies both size gates without block compression.
- Compatibility inspection, unknown-field retention, and data-loss protection
  remain effective across both versions during migration and on the final v4
  baseline.
- Mixed content passes full build and editor load/render/save/restart validation.
- Lasting reflection and package contracts reside in their owning Runtime
  documentation.

## Related Documentation

- [DAST V4 Measurement and Wire Contract Plan](../Plans/DASTV4MeasurementAndWireContract.md)
- [Class Default Object Lifecycle Plan](../Plans/ClassDefaultObjectLifecycle.md)
- [Unified Archive Serialization Plan](../Plans/Archive/2026-08/UnifiedArchiveSerialization.md)
- [Reflected Struct Operations Plan](../Plans/Archive/2026-08/ReflectedStructOperations.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageArchive.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageArchive.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageValueCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetMigration.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Content/Materials/DefaultMaterial.dasset`
