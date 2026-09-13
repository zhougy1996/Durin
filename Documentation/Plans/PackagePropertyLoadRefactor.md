# Package Property Load Refactor Plan

Summary: Reuse package name and schema bindings during property loading and restore authored override intent without repeated string sorting or whole-object recapture.

Last reviewed: 2026-09-14

Status: Active
Completed:

## Current Status

Investigation identified synchronous package property loading as the dominant
measured cost when opening the shipped ImportedSurface material. No production
optimization has been retained. Temporary timing probes and the unsuccessful
material expansion index experiment were removed; the checkout was rebuilt.
This document selects a refactoring direction, not authorization to begin its
implementation in the planning turn. Stage 0 remains outstanding.

The measured asset was `/Engine/Materials/ImportedSurface.ImportedSurface` with
82 authored nodes. Documentation or later asset revisions may describe a
different graph; reproduce against a recorded asset revision before comparison.

## Goal

Reduce editor stalls caused by synchronous property loading, especially for
materials containing many nested fields. Preserve canonical package bytes,
authored intent, migration behavior, malformed-input rejection, and atomic
publication. The initial implementation must not require a package version bump
or resaving existing assets.

## Evidence and Boundaries

On Windows, `Win64-Debug-DurinEditor`, a direct MaterialTests process loaded the
actual shipped asset with the task scheduler and asset compilation manager
running. One representative instrumented run recorded:

| Operation | Time or count |
| --- | --- |
| Initial LoadObject, including dependencies | 2,989 ms |
| Main material override restoration and publication | 1,587 ms |
| Override entries for the main material | 7,552 |
| Sorting those entries | 967 ms; 94,444 path comparisons |
| Discovery/authored captures and shape comparison during override validation | 426 ms |
| Validation of individual paths against captured fields | 14 ms |
| Working material initialization immediately after load | 23 ms |
| Graph inspection immediately after initialization | 5 ms |

Rows overlap and must not be summed. Subsequent working-copy initialization was
approximately 11 ms, with graph inspection approximately 5 ms. Independent
63/127/255-node graph normalization measurements were approximately 3/6/12 ms;
replacing expansion scans with hash indexes did not demonstrate a benefit.

Local investigation evidence is retained in ignored
`Build/.agent-state/logs/20260914-034548-908153-19936-MaterialTests.log`, with earlier
phase measurements in the adjacent `20260914-034423-899273-40408-MaterialTests.log`.
These are local supporting artifacts, not durable CI inputs. This was not a
complete GUI/GPU capture, a cold OS file-cache qualification, or a Release
performance result. Render-command admission was stopped in the direct host;
its render-proxy diagnostics must not be interpreted as an editor reproduction.

The relevant implementation boundaries are:

- CoreDObject package reader: the existing sorted, unique package name table is
  decoded into string-valued linker schemas and properties.
- Engine `AssetPackageLinkerLoader.cpp`: validates schemas, applies values, and
  reconstructs nested authored override paths.
- CoreDObject `AuthoredOverrideLedger.cpp`: validates, sorts, rejects duplicates,
  and publishes the replacement ledger. Its path comparator repeatedly calls
  `FName::ToString()` for shared path segments.
- CoreDObject `DefaultDeltaPlan.cpp`: override validation captures Discovery and
  AuthoredPackage representations and checks their shape before validating paths.

## Selected Design

1. Reuse existing package dictionaries and schemas. Build a load-scoped binding
   context mapping serialized identities to resolved runtime fields and names.
   Preserve package name IDs or references where practical; do not introduce a
   second persistent dictionary or change the wire format.
2. Resolve reflection migrations once in that context. Original name-table order
   is usable only where it agrees with the existing path comparator. Renamed
   fields, case behavior, numbered names, and canonical type aliases require
   explicit treatment. Runtime FName IDs and package-local IDs are not portable
   ordering keys or interchangeable identities.
3. Restore authored intent while traversing and applying validated fields.
   Reuse field bindings, container positions, canonical Map-key tokens, and the
   current property path. Remove redundant whole-object recapture only after an
   equivalent validation path exists, including custom serializers.
4. Keep validation and publication transactional. Only an internal, bounded load
   builder may publish its verified result. Do not add a public unchecked ledger
   setter or allow file data to assert that it is trusted. General editing and
   non-package callers retain their required validation.
5. Separate runtime lookup requirements from canonical output ordering. Stage 0
   must establish whether current consumers require the ledger's sorted vector.
   Initially preserve its observable behavior; obtain canonical order from
   resolved keys or ordered traversal without repeated string construction.
   A tree or prefix-sharing representation is a later option only if this audit
   and measurements justify it, with all public consumers migrated together.

Do not coalesce parent/child override entries merely because values currently
match defaults. LoadedExplicit and Forced intent, default evolution, nested
container semantics, and transaction restoration remain authoritative.

## Unreal Engine Reference

Epic's public interfaces provide design evidence, not proof of an identical
private implementation or performance result:

- [FLinker](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FLinker)
  retains a NameMap of runtime name-entry IDs;
  [FNameMap](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FNameMap)
  maps serialized names directly to FName values.
- [FArchiveSerializedPropertyChain](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FArchiveSerializedPropertyChain?lang=en-US)
  maintains the active FProperty pointer stack.
- [FPropertyTag](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPropertyTag)
  binds a property and carries an override operation.
- [FOverriddenPropertySet](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FOverriddenPropertySet)
  restores operations using the current serialized property chain and property;
  [FOverriddenPropertyNode](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FOverriddenPropertyNode)
  exposes child nodes. This overridable serialization feature is experimental
  and is not equivalent to ordinary serialization of every UE asset.

The selected adaptation is reuse of resolved identity and traversal context.
It does not assume UE sorts its package dictionary like DAST or skips validation.

## Implementation Stages

### Stage 0: Establish reproducible evidence and semantic requirements

Dependency: none. Outcome: a bounded benchmark and a reviewed contract map.

- [ ] Add reproducible asset-load measurements for ImportedSurface and synthetic
  nested-property assets at several sizes. Record source revision, asset hash,
  configuration, process/cache state, repetitions, and median/tail times.
- [ ] Separate dictionary/schema binding, value application, intent capture,
  intent validation, ordering, and publication timings. Include an ordinary
  small asset and reopen/resident cases; do not fold waiting for compilation
  into the editor's actual open call.
- [ ] Reproduce the stall in the editor and distinguish initial resource load
  from first-frame rendering. Measure both Debug and Release before setting
  absolute time budgets.
- [ ] Audit ledger ordering, prefix lookup, equality, duplicate rejection,
  transaction, save/cook, and copy consumers. Document constraints on stable
  ordering and error precedence before selecting the internal representation.
- [ ] Identify which recapture checks protect custom serializer behavior and
  specify their equivalent validation or explicit fallback.
- [ ] Record baseline artifacts and agreed performance gates in this plan.

### Stage 1: Bind serialized names and fields once per load

Dependency: Stage 0. Outcome: reusable validated bindings without wire changes.

- [ ] Introduce a load-scoped binding context at the CoreDObject/Engine ownership
  boundary, retaining dictionary/schema identity and resolved runtime fields.
- [ ] Reuse bindings across validation, deprecated-route handling, value
  application, and intent generation; avoid repeated name conversion and schema
  scans for every nested element.
- [ ] Handle aliases and renamed fields before deriving ordering keys. Preserve
  dictionary bounds, type compatibility, unknown-field policy, and diagnostics.
- [ ] Prove context lifetime cannot escape into runtime ledgers as dangling
  views or package-local IDs; bound its memory by admitted package metadata.
- [ ] Validate loading of existing canonical-v9 fixtures without resaving them.

### Stage 2: Eliminate repeated string work in override ordering

Dependency: Stage 1. Outcome: equivalent ledger ordering without per-comparison
string reconstruction.

- [ ] Produce or compare paths using the bound identity/order information,
  sharing common prefixes where justified by Stage 0.
- [ ] Preserve exact canonical comparison, duplicate detection, provenance, and
  prefix behavior for all supported token kinds. Keep general caller behavior
  intact, including names absent from a particular package dictionary.
- [ ] Compare results against the existing comparator for reordered inputs,
  renamed/case-sensitive/numbered names, arrays, and Map values.
- [ ] Demonstrate reduced ordering time on the real material and size series;
  reject changes that only move equivalent work to another measured phase.

### Stage 3: Restore verified intent during property application

Dependency: Stages 1-2. Outcome: one coordinated traversal for applying values
and recording authored intent, with equivalent validation before publication.

- [ ] Carry the resolved property path through value application and produce
  override records from that context instead of rediscovering live fields.
- [ ] Replace redundant Discovery/AuthoredPackage captures for supported reflected
  fields with equivalent binding and consumption checks. Keep a measured,
  documented fallback for serializers not covered by the new mechanism.
- [ ] Publish only after all values, paths, provenance, and duplicates validate;
  preserve failure rollback and existing load/reload publication ordering.
- [ ] Exercise malformed tokens, wrong types, out-of-range indices, unavailable
  Map keys, serializer shape mismatches, and migration failures without partial
  object or ledger publication.

### Stage 4: Qualify compatibility, performance, and editor behavior

Dependency: Stage 3. Outcome: measured improvement with unchanged authored intent.

- [ ] Verify load/save/load equivalence, deterministic output across processes,
  unchanged legacy-fixture compatibility, explicit values equal to defaults,
  Forced provenance, changed defaults, nested containers, duplication, Undo/Redo,
  package reload, custom serialization, and cooked exclusion behavior.
- [ ] Run relevant CoreDObject serialization/ledger and Engine package/material
  tests selected from the configured registry, then affected-test validation.
- [ ] Compare phase and total times with Stage 0 in the same environment. Require
  at least a 50% reduction in the measured override-restoration phase and no
  material regression in small-asset loading or retained memory. This is a
  proposed acceptance target, not a claimed result; change it only with evidence.
- [ ] Confirm the real editor's initial material opening improves; report CPU
  loading separately from compilation and GPU first-frame costs.
- [ ] Validate every affected project if shared APIs change and complete an all
  build for a shared Engine API migration or runnable-editor integration handoff.
- [ ] Update the owning serialization and package contracts, remove temporary
  probes, record final evidence, and complete this plan only after its gates pass.

## Execution References and Non-goals

Follow [build and run](../Agents/BuildAndRun.md),
[testing](../Agents/Testing.md), and
[native test authoring](../Development/Build/NativeTestAuthoring.md).
The authoritative semantics remain in
[Serialization](../Runtime/Core/Serialization.md) and
[Asset Packages](../Runtime/Assets/AssetPackages.md).

This plan does not change material graph semantics, shader compilation,
Studio lighting, GPU pipeline preparation, package loading thread ownership,
or the serialized meaning/granularity of authored overrides. Async loading and
an independently redesigned override representation require separate evidence
and an explicit plan amendment; they are not prerequisites for this refactor.
