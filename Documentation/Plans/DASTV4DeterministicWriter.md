# DAST V4 Deterministic Writer Plan

Summary: Implement the bounded production DAST v4 writer by consuming the frozen wire contract and default-relative logical plan without activating a v4 reader or changing ordinary v3 saves.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

The entry gate is satisfied by the completed measurement/wire-contract and
default-relative-reflection plans. Runtime now publishes deterministic class and
Struct defaults, tri-state identity, default-object graph pairing, a canonical
wire-neutral delta plan, pointer-free authored override intent, and complete
no-delta planning. The test-only v4 reference codec remains the byte oracle.
AssetCore production save/load and every tracked package remain DAST v3.

## Goal

Add one production-owned, deterministic, bounded DAST v4 byte writer whose
output exactly matches the frozen reference codec for supported logical plans.
Expose it only through an explicit low-level writer boundary so later reader and
migration plans can consume it; do not change the current package version or
ordinary `SavePackage` behavior.

## Scope

- Canonical discovery and freezing of v4 Name, Type, Schema/custom-version,
  Object, dependency, and Value inputs.
- Checked primitive, table, record, section, summary, directory, and value
  emission for every frozen supported opcode.
- Adaptation from `FDefaultDeltaPlan`, including explicit/forced provenance,
  nested Struct omission, no-delta plans, default-subobject object records, and
  graph-relative hard references.
- Exact retained-unknown descriptor-closure passthrough at the writer boundary.
- Determinism, rollback, malformed-plan, limit, byte-parity, cost, and
  Default Material size qualification.

## Non-Goals

- A v4 reader, construct-free inspector, compatibility policy, registry change,
  latest-writer switch, automatic resave, migration, or tracked `.dasset` edit.
- Reopening the frozen v4 layout, opcodes, bounds, provenance values, custom
  versions, unknown closure, default semantics, or size gates.
- Compression, asset-specific codecs, container edit deltas, or custom Struct
  codecs without a separately activated evidence-gated plan.

## Design Decisions and Invariants

- CoreDObject remains the sole owner of defaults, identity, authored intent, and
  delta decisions. AssetCore receives a completed logical plan and does not
  re-compare live values or query a ledger.
- Discovery closes every table and dependency before byte emission. Any late,
  duplicate, cyclic, unsupported, noncanonical, out-of-range, or mismatched
  input fails before destination publication.
- Canonical ordering and ids follow the lasting
  [Asset Packages](../Runtime/Assets/AssetPackages.md#frozen-dast-v4-wire-contract)
  contract. The executable test-only codec remains an independent oracle until
  production byte parity is complete.
- The writer operates on temporary owned bytes and publishes atomically only
  after complete validation. Failure preserves the caller's prior destination.
- The new API is explicitly versioned/low-level. `AssetVersion`, production v3
  readers, ordinary `SavePackage`, registries, and authored content remain
  unchanged.

## Current Foundations and Gaps

The complete Default Material reference package is 6,275 bytes with section
sizes `{1803, 62, 107, 5, 4219}`, XXH64 `C4111B7609C78D4F`, 554 emitted and 231
omitted logical fields, and 1,275 comparisons. Primitive and malformed-input
goldens already freeze every wire rule. The remaining gap is production
ownership: table freezing and byte emission currently exist only in test code,
and there is no low-level production v4 writer API.

## Implementation Stages

### Stage 0: Freeze the production writer boundary

- [ ] Audit the test reference codec against lasting Runtime contracts and list
  the minimal production-owned data model, without copying test-only policy.
- [ ] Select the low-level API, destination publication contract, diagnostics,
  limits, and retained-unknown input boundary.
- [ ] Record the initial working set and byte-parity fixture matrix.

#### Acceptance Gate

- The selected API consumes only frozen logical/package inputs, cannot switch
  ordinary saves to v4, and assigns one owner to every emitted byte.

### Stage 1: Implement canonical discovery and table freezing

- [ ] Implement bounded Name, Type, Schema/custom-version, Object, dependency,
  and retained-closure discovery with exact structural deduplication.
- [ ] Freeze canonical ordering, ids, object topology, reference resolution, and
  discovery/emission manifests before writing.
- [ ] Add positive, reverse-order, duplicate, cycle, overflow, and late-input
  tests against the independent reference model.

#### Acceptance Gate

- Repeated and perturbed discovery produces identical frozen tables, while every
  malformed or late input fails transactionally with a stable diagnostic.

### Stage 2: Emit the frozen v4 envelope and values

- [ ] Implement checked primitive encoders, public summary, directory, five
  sections, records, and exact length framing.
- [ ] Emit all supported logical node kinds from `FDefaultDeltaPlan`, including
  nested Struct provenance, canonical Maps, references, no-delta, custom
  versions, and exact unknown closure/payload retention.
- [ ] Prove byte-for-byte parity with every primitive, comprehensive,
  malformed-plan, and Default Material reference fixture.

#### Acceptance Gate

- Production bytes exactly equal the frozen reference bytes for every supported
  fixture, and failure cannot partially publish output.

### Stage 3: Integrate the explicit low-level writer

- [ ] Add an explicit opt-in package-to-v4 planning/writing entry that composes
  Archive discovery, `BuildDefaultDeltaPlan`, frozen tables, and byte emission.
- [ ] Preserve production v3 save/load, registry, audit, migration, and package
  version behavior; prove no call path reaches v4 accidentally.
- [ ] Cover enabled/no-delta options, authored provenance, default subobjects,
  dependencies, unknown retained values, unsupported Structs, and rollback.

#### Acceptance Gate

- The low-level writer produces qualified v4 bytes on explicit request while
  ordinary package operations and all tracked content remain exactly v3.

### Stage 4: Qualify and hand off to the v4 reader

- [ ] Run focused CoreDObject, Archive, AssetPackage reference/writer,
  Material/default, StaticMesh/default-subobject, documentation, asset-baseline,
  hash, and full-build validation through DurinDevTool.
- [ ] Record exact Default Material bytes, sections, digest, plan/table counts,
  writer work, repeated/reverse determinism, and rollback evidence.
- [ ] Move lasting writer contracts into Runtime documentation, complete this
  plan, and activate only the bounded v4 reader/compatibility child.

#### Acceptance Gate

- Production writer/reference parity, both size gates, focused suites, all-plan
  validation, asset baseline/hashes, and the full build pass from one baseline;
  ordinary saves remain v3 and no authored package changes.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Discovery | Complete pre-emission closure, canonical ordering/ids, late-input rejection, reverse-order determinism |
| Primitive bytes | Exact endian, VarUInt/ZigZag, UTF-8, float/NaN, GUID, string, and length goldens |
| Tables | Structural Type keys, canonical schemas/versions/objects/dependencies, cycles and bounds |
| Values | Every opcode, nested Struct deltas, Arrays/Maps/fixed arrays, references, explicit/forced provenance |
| Unknowns | Exact provenance `02` closure and payload preservation without package-table remapping |
| Failure | Typed diagnostics and atomic destination rollback for malformed or unsupported plans |
| Compatibility | Ordinary v3 save/load/audit/registry behavior and all tracked package hashes unchanged |
| Qualification | Independent reference parity, Default Material budgets/digest, focused suites, docs, asset baseline, full build |

## Definition of Done

- One production-owned low-level writer emits only the frozen v4 contract and
  matches the independent reference codec byte for byte.
- Every supported logical plan has deterministic output; every unsupported or
  malformed input fails before publication.
- Default Material remains within 16,384 and 20,659 bytes with recorded cost and
  determinism evidence.
- AssetCore's ordinary writer/readers remain v3-only, all activation hashes are
  unchanged, and the v4 reader/compatibility plan is the only activated child.

## Deferred Follow-ups

- Bounded v4 reading and construct-free compatibility inspection.
- Explicit mixed-version migration and latest-writer policy.
- Final qualification/rollout and tracked authored-content resave.
- Custom Struct codecs only if a later production audit establishes need.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Source/Runtime/AssetCore/`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4ReferenceModel.*`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4Feasibility.*`
