# DAST V4 Reader and Compatibility Plan

Summary: Implement bounded DAST v4 header, table, value, live-load, and construct-free compatibility readers without changing ordinary saves or migration policy.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

The frozen v4 wire contract, default-relative logical semantics, and explicit
production writer are complete. Production writer bytes match the independent
reference codec for every opcode, retained closure, enabled/no-delta plan, and
the 6,275-byte Default Material fixture. AssetCore still recognizes, loads,
inspects, registers, and ordinarily saves only DAST v3; no v4 byte path is
reachable through current package policy.

## Goal

Add bounded production v4 reading and construct-free compatibility inspection
that consume the frozen writer output, preserve exact unknown descriptor
closures, and publish live objects transactionally. Keep latest-writer,
mixed-version migration, registry activation, and tracked-content changes for
their separately gated plans.

## Scope

- Header-only validation of the frozen v4 public summary and five-entry
  directory without parsing body tables.
- Complete bounded Name, Type, Schema/custom-version, Object, and Value table
  reconstruction with canonical-order validation.
- Transactional live object creation, default-relative known-field application,
  provenance-ledger restoration, dependency/reference resolution, and PostLoad.
- Construct-free compatibility and reference inspection using the same decoded
  logical descriptors and exact unknown closure/payload retention.
- Malformed-input, rollback, parity, cost, and mixed v3/v4 reader qualification
  behind explicit low-level entry points.

## Non-Goals

- Changing `AssetVersion`, ordinary save output, registry acceptance, cache
  policy, latest-writer selection, migration edges, or tracked `.dasset` files.
- Reopening any frozen v4 bytes, bounds, ordering, provenance, defaults, custom
  versions, unknown-closure semantics, or writer decisions.
- Compatibility repair, data-loss authorization, automatic resave, rollout, or
  removal of the v3 reader.

## Design Decisions and Invariants

- Header, complete logical decode, live load, and construct-free inspection are
  separate bounded layers; header-only work never allocates body tables.
- The production writer and independent reference decoder remain byte oracles.
  Reader convenience cannot relax canonical wire validation.
- All ids resolve through immutable decoded tables. Unknown provenance `02`
  retains exact closure and payload bytes without remapping through package
  tables.
- Object skeletons and Outers exist before any field value is applied.
  Dependencies resolve before references, and PostLoad runs only after every
  object succeeds.
- Any header, table, schema, version, object, value, dependency, callback, or
  PostLoad failure discards the complete new graph and preserves prior cache,
  registry, dirty state, compatibility data, and caller destinations.
- V4 reading remains explicit low-level capability until the migration plan
  separates supported readers from the latest writer and activates registry
  policy.

## Current Foundations and Gaps

AssetCore already owns bounded v3 header reads, transactional graph loading,
construct-free inspection, compatibility classification, exact legacy payload
retention, dependency resolution, and data-loss protection. The v4 writer owns
checked primitives, canonical table/value semantics, and qualified fixtures.
The remaining gap is read ownership: production has no v4 parser, decoded
immutable model, live adapter, or compatibility adapter.

## Implementation Stages

### Stage 0: Freeze reader boundaries and rollback ownership

- [ ] Select header-only, complete logical decode, live load, and construct-free
  inspection APIs with typed diagnostics and explicit limits.
- [ ] Map every decoded allocation, retained byte span, object skeleton,
  dependency, ledger mutation, callback, and publication to one rollback owner.
- [ ] Record the reference, writer, malformed, compatibility, and cost fixture
  matrix and initial working set.

#### Acceptance Gate

- Every input and mutation has one bounded owner, and no selected API changes
  ordinary v3 save, registry, migration, or latest-writer policy.

### Stage 1: Decode and validate the public header and frozen tables

- [ ] Implement header-only summary/directory validation with checked extents
  and no body-table allocation.
- [ ] Decode canonical Name, Type, Schema/custom-version, and Object tables into
  immutable pointer-free models with complete consumption and cycle checks.
- [ ] Match reference positives and every malformed primitive, extent, table,
  ordering, id, topology, version, depth, and limit fixture.

#### Acceptance Gate

- Valid writer/reference headers and tables reconstruct identically, while all
  noncanonical or malformed inputs fail before object or compatibility work.

### Stage 2: Decode values and retain unknown descriptor closures

- [ ] Decode every frozen value opcode with exact framing, canonical Map order,
  reference bounds, Struct provenance, and depth/container limits.
- [ ] Resolve known overrides against decoded schemas and retain provenance
  `02` closure/payload bytes exactly after complete closure validation.
- [ ] Prove round-trip writer equality for primitive, comprehensive,
  enabled/no-delta, Default Material, and retained-unknown fixtures.

#### Acceptance Gate

- Every supported writer package decodes to the frozen logical model and
  re-emits byte-identically; unknown bytes and descriptor meaning are unchanged.

### Stage 3: Integrate transactional live v4 loading

- [ ] Create all object skeletons and Outers, resolve dependencies, apply known
  values relative to class/Struct defaults, restore explicit/forced authored
  intent, then run PostLoad in the established order.
- [ ] Preserve unknown/removed fields in `FAssetLoadReport` with exact retained
  closures and enforce existing compatibility-risk/data-loss boundaries.
- [ ] Cover default subobjects, internal/external/soft references, custom
  versions, missing classes, unsupported Structs, callback failures, and full
  graph rollback.

#### Acceptance Gate

- Explicit low-level v4 load reconstructs qualified object graphs and all
  failures preserve the previous live state; ordinary package loading remains
  v3-only.

### Stage 4: Integrate construct-free inspection and qualify handoff

- [ ] Extend header, compatibility, and reference inspection over immutable v4
  decoded descriptors without object construction or callbacks.
- [ ] Match v3/v4 compatibility categories, risks, retained-data behavior,
  reference occurrences, freshness, and bounded cost for equivalent content.
- [ ] Run focused CoreDObject, Archive, AssetPackage, compatibility, registry-
  isolation, documentation, asset-baseline, hash, and full-build validation.
- [ ] Move lasting reader contracts into Runtime documentation, complete this
  plan, and activate only the mixed-version migration child.

#### Acceptance Gate

- Writer/reader round trips, malformed and rollback suites, compatibility
  parity, cost gates, asset baseline/hashes, all-plan validation, and the full
  build pass from one baseline without activating v4 package policy.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Header | Header-only allocation/read bounds, exact directory extents, summary parity |
| Tables | Canonical order/ids, descriptor cycles, versions, object topology, complete consumption |
| Values | Every opcode, defaults, nested Structs, containers, Maps, references, provenance |
| Unknowns | Exact closure/payload bytes, root resolution, compatibility retention, no remap |
| Live load | Skeleton/Outer order, dependencies, ledgers, PostLoad, complete rollback |
| Inspection | No construction/callbacks, compatibility/reference parity, freshness and cost |
| Isolation | Ordinary v3 save/load, registry, migration, hashes, and tracked content unchanged |
| Qualification | Reference/writer round trips, focused suites, asset baseline, docs, full build |

## Definition of Done

- Production v4 header, logical, live, and construct-free readers consume every
  supported writer fixture and reject every frozen malformed fixture.
- Default-relative live state, explicit/forced provenance, references, custom
  versions, and unknown descriptor closures survive byte-identical round trips.
- Every failure is bounded and transactional; v3 behavior, package policy,
  registry/cache behavior, and tracked authored content remain unchanged.
- The mixed-version migration plan is the only activated child.

## Deferred Follow-ups

- Supported-reader/latest-writer separation and registry/cache activation.
- Explicit atomic v3-to-v4 migration and mixed-corpus qualification.
- Final rollout, tracked-content resave, and retirement of temporary v3 support.
- Custom Struct codecs only if an evidence-gated production audit requires one.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Writer.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageArchive.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4ReferenceModel.*`
