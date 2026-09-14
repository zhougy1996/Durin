# Default Baseline Serialization Plan

Summary: Propagate explicit default baselines through reflected values and introduce independently tagged Struct deltas with versioned reconstruction.

Last reviewed: 2026-09-14

Status: Active
Completed:

## Current Status

Stage 0 is complete. Stage 1 implements paired default propagation, a v10
Struct baseline byte, independently tagged present fields, and initialization
of fresh loaded objects from paired defaults. Focused validation passed 87
CoreObjectTests and 152 AssetPackageTests. Shared-API all build passed (57.74 seconds), covering Engine, Sandbox, and
RoadWeaver. Stage 2 remains active; ordinary saving still uses v9 NoDelta. The preceding sparse authored
replacement change remains the foundation; ordinary loaded fields must never
recreate an override ledger.

## Goal

Make saving and loading agree on the baseline of every omitted field. A class
default Struct modified by its owning class must remain the baseline of nested
ordinary fields. Containers and explicit replacements retain complete values.
Preserve v9 asset reads and transactional load/publication behavior.

## UE Design Evidence

The following are official public API contracts, not a claim that every native
UE serializer follows one implementation path:

- [UStruct::SerializeTaggedProperties](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UStruct/SerializeTaggedProperties)
  accepts both the default layout and default data. Property tags support
  mismatched property layouts.
- [UScriptStruct::SerializeItem](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UScriptStruct/SerializeItem)
  accepts an explicit default instance; null means no default comparison.
  Durin should propagate a baseline rather than unconditionally fetch the
  Struct type default at each recursive step.
- [FPropertyTag](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPropertyTag)
  separates property identity/type/size from the value and accepts defaults
  when serializing a tagged property. Durin can retain shared schemas while
  making each value's field presence independent.
- [FArchiveState](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FArchiveState)
  distinguishes property delta, intra-property delta, and binary/unversioned
  serialization. Durin will keep containers as complete replacements in this
  plan; this is a selected subset, not a claim that UE always replaces them.
- [FObjectInstancingGraph::AddNewInstance](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FObjectInstancingGraph/AddNewInstance)
  maps source archetypes to instances. Default subobject reference identity
  must be evaluated through graph correspondence, not raw pointer equality.
- [FOverridableSerializationLogic](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FOverridableSerializationLogic)
  is explicitly enabled separately. Its public documentation marks it
  experimental. Keep Durin's sparse replacement intent separate from ordinary
  value emission instead of importing this entire subsystem.

## Selected Design

An ordinary Struct receives the corresponding default value from its parent.
The initial baseline is the paired CDO/default subobject. A standalone Struct
may use a specifically selected type default; lack of a baseline is represented
explicitly and means complete emission. Arrays, fixed arrays, Maps, and Forced
replacement boundaries emit complete contained values. Custom/native serializers
retain their existing capability checks.

v9 already writes a field count, name, type, provenance, and value per Struct
field. However, its reader requires that count to equal the shared type child
count, and its runtime Archive initializes emitted Structs from type defaults.
Reinterpreting v9 silently would change historical asset semantics. A new
format revision must distinguish parent-baseline reconstruction and independent
field presence; reuse the existing package tables and framing where possible.
The selected revision is v10: a Struct baseline byte precedes its existing
field-count/tag stream. Present fields carry explicit types independently of
the full shared descriptor. The detached linker retains this field-type vector;
v9 retains positional fields and type-default reconstruction. General readers
select the version from validated framing; v9 compatibility entrypoints remain
strict. Registry, codec dispatch, inspection, bulk resources, and isolated graph
loading propagate the source version. Reuse the codec implementation for both
versions rather than duplicating it.

Ordinary package saving may switch to delta only after dynamic owned objects,
missing template counterparts, and reference remapping are covered. Complete
save mode remains explicit. Never silently fall back from a failed delta plan
to complete save; report a diagnosed unsupported case.

## Implementation Stages

### Stage 0: Verify UE contracts and freeze compatibility decisions

- [x] Verify default, tag, Archive policy, override, and instancing contracts
  against official UE documentation.
- [x] Inspect v9 field-count validation and Struct load initialization.
- [x] Finalize version propagation and all package-reader consumer migration
  points before changing the writer default.

Acceptance: selected wire and runtime semantics have an explicit version
boundary, with historical v9 reconstruction unchanged.

### Stage 1: Propagate default baselines and implement tagged Struct deltas

- [x] Carry paired baselines through nested ordinary Struct planning.
- [x] Keep container elements and Forced replacement contents complete.
- [x] Add a format revision supporting independent Struct field presence and
  parent-baseline reconstruction; preserve the v9 reader.
- [x] Migrate inspection, bulk, reference, codec, and version-policy consumers.
- [x] Cover class-modified defaults, nested fields, default evolution,
  malformed tags, container values, explicit replacement, and v9 reads.

Acceptance: saving only a changed nested field reconstructs the exact value
against its paired defaults, with transactional failure and no ordinary ledger.

### Stage 2: Complete object graph coverage and enable ordinary delta saving

Depends on Stage 1. Dynamic-root graph traversal was implemented with Stage 1
because save/load baseline correspondence must agree before the writer switch.

- [ ] Cover dynamic owned objects and their defaults without losing exports.
- [ ] Verify default subobject references and newly created object references.
- [ ] Switch ordinary SavePackage to the new delta policy, retaining an explicit
  complete-save option and cooked policy.
- [ ] Verify old asset resave, unload/reload, duplication, default reset, and a
  65-node material; record file size and bounded load/allocation measurements.
- [ ] Update the owning serialization and asset-package documentation.
- [ ] Complete focused tests, affected tests, the shared-API all build, and
  documentation validators.

Acceptance: the default save path passes asset/object lifecycle coverage and
measurements are reported without claiming unmeasured UI improvements.

## Validation and Handoff

Follow [agent testing](../Agents/Testing.md),
[build guidance](../Agents/BuildAndRun.md), and
[documentation validation](../Agents/Documentation.md). Search migrated shared
API symbols in Engine, Sandbox, and RoadWeaver source and tests. Commit validated
stages with this plan and the exact stage title as provenance. Keep unpassed
gates open and distinguish implementation status from research conclusions.
