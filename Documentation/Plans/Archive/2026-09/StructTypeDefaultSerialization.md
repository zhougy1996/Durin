# Struct Type Default Serialization Plan

Summary: Complete explicit serialization baseline semantics before enabling default-relative Struct fields inside replacement containers.

Last reviewed: 2026-09-15

Status: Archived
Completed: 2026-09-15

## Current Status

All stages are complete. Authored replacement containers use explicit Struct
type defaults; complete/forced/required values remain complete. All required
CPU validation, consumer checks, the all build and documentation validation pass.

### Stage 0 Decisions

- Enabled reflected roots compare with the paired CDO; nested Structs patch the
  corresponding parent value. Independent reflected Struct container values use
  the registered type default. NoDelta, cooked, native object fields, Forced and
  AlwaysSerialize boundaries are complete, recursively.
- Reuse the v10 Struct byte with an additional value 2 (type default). Values 0
  (complete) and 1 (parent) retain their meanings and validations. Older v10
  readers reject 2 before graph construction. New readers continue to read old
  complete assets; ordinary resave can make them sparse, after which omitted
  fields follow changed defaults. Complete/Forced resaves pin all saved fields.
- DStruct registration owns GetDefaultValue storage, constructs it through ops,
  traces its references through the GC schema, and destroys it at default
  teardown. Planning borrows it synchronously and caches logical captures only
  for the operation. Loading copies it into managed temporary storage; successful
  PostDeserialize precedes destination assignment. Complete loads continue to
  default-construct temporary storage and never query registered defaults.
- Type-relative hard-reference-bearing fields remain complete, including nested
  values, so omitted fields introduce no hidden package reference dependency or
  template reference requiring remapping. Ordinary graph discovery remains full.
- Missing eligible defaults fail transactionally. Opaque/custom reflected Struct
  serializers keep their existing rejection. Required descendants keep ancestors
  emitted, and Forced/AlwaysSerialize disable sparsity throughout their subtree.
  Container membership is never paired by index/key and no ledger routes are added.
- Consumer inventory across Engine, Sandbox and RoadWeaver found planner clients
  in CoreDObject tests, Engine texture tests and AssetPackageLinkerCapture;
  Struct framing clients in PackageFormat, PackageReader, linker capture/loader,
  Material cooked records and package tests. Sandbox/RoadWeaver have no direct
  consumers. Shared Archive/linker changes require an all build.

### Implementation Evidence

- `DefaultDeltaPlan.cpp` now carries explicit baseline context, captures registered
  Struct defaults with discovery/value agreement, propagates complete boundaries,
  and retains reference-bearing fields. Published plans clear source pointers.
- `EArchiveStructBaseline` is shared by Archive and detached linker values.
  PackageReader/PackageFormat preserve 0/1 and validate 2, complete-export and
  forced-boundary restrictions, tagged field identities, and hard-reference closure.
  Loading copies the registered default before field application and callbacks.
- Fixed Struct arrays exposed two existing projection bugs: the load adapter
  attempted a second Struct read when leaving the array field, and bulk inspection
  consumed only the first projected Struct record. Both paths now handle all
  validated elements without reading another enclosing Struct.
- CoreObjectTests adds `StructTypeDefaultPlanningKeepsRequiredAndCompleteSubtrees`
  and `StructTypeDefaultUnavailableFailsOnlyRelativePlanning`. Existing identity,
  custom serializer, graph, archive failure and limit tests remain enabled.
- AssetPackageTests adds `TypeDefaultContainersEvolveAndForcedSnapshotsRemainComplete`
  and `TypeDefaultReferencesRemainExplicitAndFailedRepairRollsBack`, covering
  nonzero defaults, distinct parent defaults, Arrays/fixed arrays/Map values,
  nested containers/Structs, empty elements, insertion/deletion/reordering,
  changed defaults, required/Forced descendants, canonical detached resave,
  removed fields, invalid baselines, explicit imports and callback rollback.
- Fixed fixture: three Vector elements emit 1 of 9 scalar fields; the complete
  multi-container package is 2481 bytes and authored sparse package is 1657 bytes
  (824 bytes fewer). Both preserve exact container membership and order.
- Passed `.\DevTool.bat test '@domain=reflection'`: all six targets; final run
  log `20260915-171022-800979-24936-ctest.log`.
- Passed `.\DevTool.bat test '@domain=asset-package'`: four targets and 191 cases
  (160 AssetPackage, 13 reload, 11 bulk-container, 7 registry); final run log
  `20260915-171226-103701-30080-ctest.log`.
- Validation selection: affected analysis includes unrelated GPU/integration
  targets due to Core/Engine ownership. Bounded reflection, asset-package,
  material-cook and texture consumer checks cover the changed CPU behavior.
  No GPU behavior changed; GPU/application-hosted execution is not required.
- Passed `.\DevTool.bat test MaterialCookTests`: 6 cases; log
  `20260915-171344-053214-27716-MaterialCookTests.log`.
- Passed `.\DevTool.bat test TextureTests`: 110 cases; log
  `20260915-171420-434357-39204-TextureTests.log`.
- All four new cases also passed independently using the named CoreObjectTests
  `FCoreDObjectReflectionTests.StructTypeDefault*` and AssetPackageTests
  `FPackageAssetTests.TypeDefault*` filters with `--parallel 2`.
- Passed `.\DevTool.bat build --target all` on `Win64-Debug-DurinEditor`, including
  workspace consumers (Engine, Sandbox and RoadWeaver), in 18.02 seconds; log
  `20260915-171511-725698-18732-cmake.log`.
- Lasting contracts are updated in Serialization and AssetPackages. Changed-doc
  validation and all-plan lifecycle validation pass; no acceptance gate remains.

### Corpus Resave Follow-Up

The user requested resaving the maintained assets and removing transition-only
compatibility tests. Exact-package canonical resave succeeded for all 20 unique
mounted packages: 12 Engine, 6 Sandbox, and 2 RoadWeaver packages. Shared Engine
content was processed once. Seven Engine material-function packages changed;
the other 13 packages already had identical current output. No cooked fixture
regeneration is needed because complete cooked serialization is unchanged.

Removed `V10StructDeltasUseOwningDefaultsAndPreserveLegacyReads` and the synthetic
removed-field migration branch in the container fixture, along with transitional
old-reader prose in the runtime contracts. The production implementation has no
separate legacy migration branch to remove: Complete, Parent and TypeDefault
are all current semantic modes. Current baseline, replacement, reference and
rollback tests remain. General schema-evolution infrastructure outside this
Struct serialization change is unchanged.

Follow-up validation: AssetPackageTests passes all 159 remaining cases and
MaterialFunctionTests passes all 28 cases. Both project asset checks report no
findings; unforced whole-project resave previews skip all current packages.
Repeating exact-package resave for the seven changed files produces identical
SHA-256 hashes. Their combined package size decreases by 3318 bytes. Changed-doc
validation passes. Runtime code is unchanged, so the successful all build above
remains applicable.

## Goal

Make complete serialization, parent-relative serialization, and Struct-type-
default-relative serialization explicit throughout planning and loading. Then
allow authored replacement containers to omit default-valued fields within
Struct elements while retaining their complete membership and ordering.

## Required References

- [Serialization contract](../../../Runtime/Core/Serialization.md#default-relative-logical-planning)
- [Authored override intent](../../../Runtime/Core/Serialization.md#authored-override-intent)
- [Asset package defaults and versions](../../../Runtime/Assets/AssetPackages.md#defaults-and-version-policy)
- [Default delta planner API](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h)
- [Default delta planner implementation](../../../../Engine/Source/Runtime/CoreDObject/Private/DObject/DefaultDeltaPlan.cpp)
- [Build workflow](../../../Agents/BuildAndRun.md)
- [Native test workflow](../../../Agents/Testing.md)
- [Documentation workflow](../../../Agents/Documentation.md)

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

- [x] Trace baseline handling through logical capture, planning, detached package
  conversion, codec validation, load initialization, and field application.
- [x] Locate authoritative Struct default creation/copy operations and document
  ownership, destruction, nested values, object-reference handling, and callbacks.
- [x] Determine whether v10 can distinguish and validate the proposed sparse
  type-default value without changing existing complete-value meaning. Choose
  compatible reuse or an explicit version transition before implementation;
  document old-reader behavior and the supported old-asset/resave policy.
- [x] Specify handling of unavailable defaults, custom serializers, forced
  ancestors, required descendants, and discovery/reference closure for defaults
  whose fields are omitted from the emitted package.
- [x] Record decisions here, including how complete old assets behave after
  resaving and subsequent default changes. Inventory consumers across projects
  declared in `Durin.dworkspace` before changing shared APIs.

Acceptance: every mode has an unambiguous save/load mapping; no unresolved wire
or default-reference ownership decision remains before Stage 1.

### Stage 1: Complete the Baseline Planning Foundation

Dependencies: Stage 0. Outcome: explicit baseline selection works for independent
Struct values without enabling container omission in production saves yet.

- [x] Introduce or refine explicit baseline context using the existing planner
  model. Keep complete mode distinct from type-default lookup and parent reuse.
- [x] Capture authoritative Struct type defaults through the supported logical
  serialization path; reuse captures within a planning operation where valid.
  Preserve discovery/capture consistency and avoid persistent stale-default caches.
- [x] Implement recursive field comparison with correct baseline propagation and
  transactional failures. Preserve required fields and forced complete subtrees.
- [x] Preserve existing identity rules, resource bounds, diagnostic paths,
  source-pointer cleanup, and deterministic plan equivalence.
- [x] Migrate all affected API consumers and add focused planner coverage for
  nonzero defaults, parent defaults differing from type defaults, nested Structs,
  unavailable defaults, complete modes, and unsupported custom serializers.

Acceptance: tests distinguish all three baseline meanings and prove that complete
mode neither reads defaults nor starts omitting fields.

### Stage 2: Implement Package Reconstruction and Compatibility

Dependencies: Stage 1. Outcome: sparse type-default Structs can be encoded,
validated, inspected, and reconstructed using the Stage 0 format decision.

- [x] Carry baseline meaning through detached package values and codec framing;
  validate legal baseline combinations and malformed sparse field records.
- [x] Initialize from the selected baseline before applying present fields;
  retain managed-storage lifetime, reference remapping, callback ordering, and
  failure rollback without publishing incomplete graphs.
- [x] Apply the chosen compatibility policy to old complete values, new sparse
  values, construct-free inspection, and canonical resave.
- [x] Add codec/load tests for nonzero defaults, nested omission, reference-bearing
  defaults, unknown fields, malformed baselines, and load failures. Verify that
  schema evolution follows the documented missing-field policy.

Acceptance: complete and sparse round trips pass, malformed data is rejected,
and compatibility behavior has executable evidence before container rollout.

### Stage 3: Enable Struct Defaults in Replacement Containers

Dependencies: Stage 2. Outcome: authored containers retain replacement semantics
while saving fewer default-valued Struct fields.

- [x] Apply type-default planning to supported Struct elements of Arrays and
  fixed arrays and to Map values, including nested containers.
- [x] Preserve count, order, empty/default-only elements, Map-key canonical bytes,
  whole-field omission, and complete forced or required subtrees.
- [x] Test insertion, deletion, reordering, empty containers, nested values, and
  class-default containers whose same-index elements have different values.
- [x] Verify that an omitted field follows a changed type default, an emitted
  different field retains its value, and whole-container forced replacement
  preserves default-valued fields. Ordinary emission must not create ledger marks.
- [x] Verify cooked/complete output stays complete and requires no new default
  initialization. Measure emitted-field counts and package bytes on a fixed
  representative Struct-array fixture; record results without a speculative target.

Acceptance: semantic tests pass and the fixture demonstrates reduced authored
payload with unchanged membership, ordering, and complete-save behavior.

### Stage 4: Validate Integration and Publish Contracts

Dependencies: Stage 3. Outcome: supported project targets are validated and the
long-lived documentation describes the implemented behavior.

- [x] Run affected CoreDObject and Engine native tests using the owning workflow;
  cover package save/load/resave and affected consumer targets identified in Stage 0.
- [x] Complete an `all` build if shared Engine APIs were migrated, as required by
  repository guidance. Record exact validation results and any limitations.
- [x] Update Serialization and Asset Packages contracts with baseline selection,
  default evolution, complete/forced behavior, format compatibility, and the
  deferred per-element override limitation.
- [x] Validate documentation and record evidence for all acceptance gates before
  marking this plan complete.

Acceptance: required validation passes, lasting contracts are authoritative, and
every completed checklist item has evidence in the implementation handoff.
