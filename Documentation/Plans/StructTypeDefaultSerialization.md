# Struct Type Default Serialization Plan

Summary: Complete explicit serialization baseline semantics before enabling default-relative Struct fields inside replacement containers.

Last reviewed: 2026-09-15

Status: Active
Completed:

## Current Status

Planning only; no runtime implementation or native validation has started.
The current planner emits complete save-selected Struct fields inside Arrays,
fixed arrays, and Map values. Container emission can still be omitted when the
whole field matches its paired class default and no required or forced field
keeps it present. Ordinary Struct fields already support recursive comparison
with the corresponding parent baseline.

`EDefaultDeltaBaselineKind` already declares `None`, `ClassDefault`, and
`StructTypeDefault`. However, `BuildPlannedValue` currently selects its baseline
from a nullable default-node pointer and passes no default to container elements.
DAST v10 also already carries Struct baseline framing. These are foundations to
audit and extend, not evidence that the proposed behavior works end to end.

## Goal

Make complete serialization, parent-relative serialization, and Struct-type-
default-relative serialization explicit throughout planning and loading. Then
allow authored replacement containers to omit default-valued fields within
Struct elements while retaining their complete membership and ordering.

## Required References

- [Serialization contract](../Runtime/Core/Serialization.md#default-relative-logical-planning)
- [Authored override intent](../Runtime/Core/Serialization.md#authored-override-intent)
- [Asset package defaults and versions](../Runtime/Assets/AssetPackages.md#defaults-and-version-policy)
- [Default delta planner API](../../Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h)
- [Default delta planner implementation](../../Engine/Source/Runtime/CoreDObject/Private/DObject/DefaultDeltaPlan.cpp)
- [Build workflow](../Agents/BuildAndRun.md)
- [Native test workflow](../Agents/Testing.md)
- [Documentation workflow](../Agents/Documentation.md)

## Selected Design and Scope

1. Represent the requested baseline explicitly rather than inferring every
   behavior from whether a default pointer is null. Reuse existing baseline
   types where their meaning fits; do not create a parallel serialization stack.
2. Complete values omit no default-valued save-selected fields. Parent-relative
   Structs compare against the corresponding initialized parent value. Independent
   Struct values compare against an authoritative initialized type default.
   Type defaults are not assumed to be zero-filled memory.
3. Arrays and fixed arrays remain whole replacements; Map values may use the
   same Struct capability. Do not pair elements with a class-default container
   by index or key. Map keys retain complete canonical identity encoding.
4. Enable this omission for ordinary authored saving after the foundation gates
   pass. Preserve complete `NoDelta`, cooked values, forced replacements, and
   `AlwaysSerialize` boundaries, including their descendants.
5. An omitted field inherits its selected baseline at load time. A later change
   to a Struct type default can therefore affect an existing sparse asset. This
   is an intentional authored-data contract, not transparent byte compression.
6. Preserve current whole-field forced replacement as the way to pin complete
   container contents. Per-element field override editing and stable element
   identity are deferred; do not silently add indexed ledger paths. Record this
   limitation in the implemented contract and verify existing override behavior.
7. Keep custom/native Struct serializers under their existing supported-mode
   rules until an explicit comparison and reconstruction contract exists. Do not
   interpret a missing baseline as permission to sparsify opaque data.

Non-goals: array edit scripts, indexed inheritance from default arrays, network
delta replication, canonical Map-key changes, new editor override controls, and
generic package compression. UE API signatures alone do not establish exact
array serialization behavior; this plan does not depend on an unverified UE
implementation claim.

## Implementation Stages

### Stage 0: Audit Baseline and Compatibility Contracts

Dependencies: none. Outcome: a reviewed mapping from each save mode to its
planner baseline, wire representation, initialization, and override semantics.

- [ ] Trace baseline handling through logical capture, planning, detached package
  conversion, codec validation, load initialization, and field application.
- [ ] Locate authoritative Struct default creation/copy operations and document
  ownership, destruction, nested values, object-reference handling, and callbacks.
- [ ] Determine whether v10 can distinguish and validate the proposed sparse
  type-default value without changing existing complete-value meaning. Choose
  compatible reuse or an explicit version transition before implementation;
  document old-reader behavior and the supported old-asset/resave policy.
- [ ] Specify handling of unavailable defaults, custom serializers, forced
  ancestors, required descendants, and discovery/reference closure for defaults
  whose fields are omitted from the emitted package.
- [ ] Record decisions here, including how complete old assets behave after
  resaving and subsequent default changes. Inventory consumers across projects
  declared in `Durin.dworkspace` before changing shared APIs.

Acceptance: every mode has an unambiguous save/load mapping; no unresolved wire
or default-reference ownership decision remains before Stage 1.

### Stage 1: Complete the Baseline Planning Foundation

Dependencies: Stage 0. Outcome: explicit baseline selection works for independent
Struct values without enabling container omission in production saves yet.

- [ ] Introduce or refine explicit baseline context using the existing planner
  model. Keep complete mode distinct from type-default lookup and parent reuse.
- [ ] Capture authoritative Struct type defaults through the supported logical
  serialization path; reuse captures within a planning operation where valid.
  Preserve discovery/capture consistency and avoid persistent stale-default caches.
- [ ] Implement recursive field comparison with correct baseline propagation and
  transactional failures. Preserve required fields and forced complete subtrees.
- [ ] Preserve existing identity rules, resource bounds, diagnostic paths,
  source-pointer cleanup, and deterministic plan equivalence.
- [ ] Migrate all affected API consumers and add focused planner coverage for
  nonzero defaults, parent defaults differing from type defaults, nested Structs,
  unavailable defaults, complete modes, and unsupported custom serializers.

Acceptance: tests distinguish all three baseline meanings and prove that complete
mode neither reads defaults nor starts omitting fields.

### Stage 2: Implement Package Reconstruction and Compatibility

Dependencies: Stage 1. Outcome: sparse type-default Structs can be encoded,
validated, inspected, and reconstructed using the Stage 0 format decision.

- [ ] Carry baseline meaning through detached package values and codec framing;
  validate legal baseline combinations and malformed sparse field records.
- [ ] Initialize from the selected baseline before applying present fields;
  retain managed-storage lifetime, reference remapping, callback ordering, and
  failure rollback without publishing incomplete graphs.
- [ ] Apply the chosen compatibility policy to old complete values, new sparse
  values, construct-free inspection, and canonical resave.
- [ ] Add codec/load tests for nonzero defaults, nested omission, reference-bearing
  defaults, unknown fields, malformed baselines, and load failures. Verify that
  schema evolution follows the documented missing-field policy.

Acceptance: complete and sparse round trips pass, malformed data is rejected,
and compatibility behavior has executable evidence before container rollout.

### Stage 3: Enable Struct Defaults in Replacement Containers

Dependencies: Stage 2. Outcome: authored containers retain replacement semantics
while saving fewer default-valued Struct fields.

- [ ] Apply type-default planning to supported Struct elements of Arrays and
  fixed arrays and to Map values, including nested containers.
- [ ] Preserve count, order, empty/default-only elements, Map-key canonical bytes,
  whole-field omission, and complete forced or required subtrees.
- [ ] Test insertion, deletion, reordering, empty containers, nested values, and
  class-default containers whose same-index elements have different values.
- [ ] Verify that an omitted field follows a changed type default, an emitted
  different field retains its value, and whole-container forced replacement
  preserves default-valued fields. Ordinary emission must not create ledger marks.
- [ ] Verify cooked/complete output stays complete and requires no new default
  initialization. Measure emitted-field counts and package bytes on a fixed
  representative Struct-array fixture; record results without a speculative target.

Acceptance: semantic tests pass and the fixture demonstrates reduced authored
payload with unchanged membership, ordering, and complete-save behavior.

### Stage 4: Validate Integration and Publish Contracts

Dependencies: Stage 3. Outcome: supported project targets are validated and the
long-lived documentation describes the implemented behavior.

- [ ] Run affected CoreDObject and Engine native tests using the owning workflow;
  cover package save/load/resave and affected consumer targets identified in Stage 0.
- [ ] Complete an `all` build if shared Engine APIs were migrated, as required by
  repository guidance. Record exact validation results and any limitations.
- [ ] Update Serialization and Asset Packages contracts with baseline selection,
  default evolution, complete/forced behavior, format compatibility, and the
  deferred per-element override limitation.
- [ ] Validate documentation and record evidence for all acceptance gates before
  marking this plan complete.

Acceptance: required validation passes, lasting contracts are authoritative, and
every completed checklist item has evidence in the implementation handoff.
