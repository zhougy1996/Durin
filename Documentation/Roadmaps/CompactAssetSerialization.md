# Compact Asset Serialization Roadmap

Summary: Coordinate the reflection foundations, deterministic DAST v3 format, dual-version migration, and authored-content rollout needed to compact asset packages without weakening compatibility guarantees.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- The program is deliberately deferred. No DAST v3 implementation plan is
  active, and DAST v2 remains the only accepted and emitted package version.
- The previous monolithic implementation plan has been converted into this
  roadmap so future work can be activated as bounded child plans only when its
  entry gates are satisfied.
- `Engine/Content/Materials/DefaultMaterial.dasset` remains the motivating
  corpus. Its 2026-08-05 v2 baseline is 82,636 bytes; repeated reflection names,
  textual type signatures, field metadata, and default-valued bytes account for
  most of the package.
- General-purpose compression proves that the bytes are redundant, but it does
  not remove repeated parsing, allocation, or schema reconstruction.
- The [Reflected Struct Operations](../Plans/ReflectedStructOperations.md)
  prerequisite is complete. Struct lifecycle, logical equality, reference
  collection, runtime Archive customization, post-deserialize repair, and
  authored fail-closed semantics are now declarative. This satisfies a
  foundation gate but does not start DAST v3 implementation.

## Outcome

Deliver a deterministic authored-package format that stores reflection metadata
once per package, encodes object and struct values relative to declared defaults,
preserves bounded compatibility inspection and unknown-field retention, and
migrates DAST v2 content only on an authorized save.

The canonical Default Material package must become no larger than 25 percent of
its same-content v2 baseline and no larger than 16 KiB without relying on block
compression.

## Scope

- Reflection and default-value foundations required by sparse struct and object
  encoding.
- A versioned DAST v3 public header and deterministic Name, Type, Schema,
  Object, and Value sections.
- Compact canonical scalar, container, reference, enum, and intrinsic math
  encodings.
- Bounded DAST v2/v3 loading, inspection, compatibility reporting, registry
  caching, and save-time migration.
- Determinism, malformed-input, size, parsing-cost, integration, and authored-
  content rollout validation.

## Non-Goals

- Starting DAST v3 implementation before an explicit child plan is activated.
- General-purpose compression inside the authored `.dasset` envelope.
- Cooked-only unversioned property serialization tied to a frozen runtime
  reflection layout.
- Asset-specific material serialization, async loading, memory mapping, soft
  references, redirects, or hot reload.
- Silent bulk migration, automatic compatibility repair, or writable actions in
  compatibility inspection tools.

## Program Decisions and Invariants

### Authored Format Boundary

- DAST v3 remains a field-tagged authored format. Package-local tables replace
  repeated metadata, but stable declaring-type, field, and logical-type
  identities remain available for compatibility inspection.
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
- DAST v2 remains bounded and readable after v3 becomes the only writer. Merely
  scanning or loading v2 never rewrites or dirties it.
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

- DAST v2 already separates bounded public-header reads from complete package
  loading and atomic publication.
- Existing reflection metadata exposes qualified class, struct, enum, and field
  identities plus recursive property kinds.
- Compatibility inspection, data-loss consent, registry fingerprints, frozen
  fixtures, and package tests provide a baseline that v3 must preserve.
- `DStruct` exposes immutable operation capabilities, managed aligned value
  storage, recursive logical equality, and transactional Archive/authored
  loading. `FPropertyValueSnapshot` uses the same equality and roots reflected
  plus declared hidden references.

### Gaps

- Reflected classes do not own deterministic class default objects.
- Struct schemas and textual type signatures repeat at each occurrence.
- Package-version handling conflates the latest writer with the supported reader
  set.
- Existing tests do not enforce package metadata cardinality, section byte
  accounting, or an authored-package size budget.

## Milestone Map

| Milestone | Kind | Dependencies | Deliverable | State |
| --- | --- | --- | --- | --- |
| Reflected struct operations | External prerequisite | None | Declarative and fail-closed lifecycle, equality, reference, and serialization semantics | Completed 2026-08-05 |
| V3 measurement and wire contract | Required child plan | Struct-operations audit complete; program explicitly scheduled | Reproducible v2 accounting and a frozen bounded v3 byte contract | Proposed, deferred |
| Default-relative reflection | Required child plan | Frozen default contract and successful struct-operations plan | Class default objects, struct defaults, logical equivalence, and no-delta policy | Proposed, deferred |
| Deterministic v3 writer | Required child plan | Default-relative reflection complete | Canonical tables and compact value emission meeting size gates | Proposed, deferred |
| V3 reader and compatibility | Required child plan | Writer fixtures and frozen schema model | Bounded loading, inspection, unknown retention, and malformed-input coverage | Proposed, deferred |
| Mixed-version migration | Required child plan | V2/v3 reader and writer stable | Registry/version separation and authorized atomic v2-to-v3 save migration | Proposed, deferred |
| Qualification and rollout | Required child plan | Migration suite complete | Full validation, lasting documentation, and authored repository content resaved | Proposed, deferred |
| Custom struct asset codecs | Conditional child plan | Struct audit identifies durable state that reflected fields plus post-load repair cannot represent | Versioned codecs with dependency discovery, inspection, and migration contracts | Evidence-gated |

## Child Plan Boundaries

### Reflected Struct Operations

Owns `DStruct` capability declaration, generated registration, lifecycle call
contracts, logical equality hooks, custom runtime Archive dispatch, post-load
repair, hidden reference collection, and authored-serialization fail-closed
policy. It does not define DAST v3 bytes or class default objects.

### V3 Measurement and Wire Contract

Will own byte-accounting fixtures, section kinds, opcodes, bounds, canonical
ordering, intrinsic logical layouts, and the exact compatibility model. It must
not implement the production reader or writer while wire decisions remain open.

### Default-Relative Reflection

Will own class default-object lifecycle, deterministic struct default storage,
recursive logical equivalence, and no-delta policy. It will consume the
completed struct-operations contract rather than adding alternate lifecycle
callbacks inside AssetCore.

### Writer, Reader, Migration, and Rollout Plans

Each later plan owns only its named production slice and focused validation.
The writer does not alter registry behavior; the reader does not resave content;
the migration plan does not bulk-resave; and repository content changes occur
only in the final rollout plan after dual-version validation passes.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Struct semantics | Every serialized struct has explicit usable capabilities or a deterministic unsupported diagnostic |
| Default lifecycle | One deterministic default object per constructible class and safe struct default storage |
| Wire primitives | Golden little-endian and canonical VarUInt fixtures for every logical opcode |
| Metadata tables | Stable ordering, deduplication, recursive descriptors, and invalid-reference rejection |
| Delta encoding | Default omission, forced override, changed-default semantics, and nested struct coverage |
| Containers and graphs | Canonical maps, arrays, Outers, cycles, and internal/external references |
| Compatibility | Unknown and removed fields, mismatches, exact payload retention, and data-loss consent for v2 and v3 |
| Registry and migration | Header-only reads, mixed versions, cache reuse, authorized atomic migration, and rollback |
| Size and parsing cost | Default Material within both size gates and no per-occurrence metadata materialization |
| End to end | Required native suites, full build, editor load/render/save/restart, and deterministic resave |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Risks and Control Gates

- **Constructor side effects:** no default-relative child plan starts until
  asset classes and serializable structs have been audited for deterministic
  construction and external references.
- **Opaque custom state:** a custom Archive serializer is not automatically an
  authored-package codec. The conditional codec plan is mandatory if reflected
  fields plus post-load repair cannot preserve durable state.
- **Compatibility regression:** v3 cannot become the writer default until
  equivalent v2/v3 compatibility categories and exact unknown payload retention
  are proven.
- **Nominal compression only:** rollout is blocked unless measurements show
  reduced metadata parsing and allocation as well as reduced bytes.
- **Migration blast radius:** scanning and loading remain read-only; only an
  explicitly authorized save may migrate one package.

## Completion Criteria

- Every required milestone has completed its exit gate, and the conditional
  custom-codec milestone is either completed or dispositioned by audit evidence.
- DAST v3 is the only authored writer while bounded DAST v2/v3 readers remain
  supported.
- Default Material satisfies both size gates without block compression.
- Compatibility inspection, unknown-field retention, and data-loss protection
  remain effective across both versions.
- Mixed content passes full build and editor load/render/save/restart validation.
- Lasting reflection and package contracts reside in their owning Runtime
  documentation.

## Related Documentation

- [Reflected Struct Operations Plan](../Plans/ReflectedStructOperations.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Content/Materials/DefaultMaterial.dasset`
