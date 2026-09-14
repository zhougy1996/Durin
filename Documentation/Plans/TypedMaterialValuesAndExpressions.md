# Typed Material Values And Expressions Plan

Summary: Replace all-alternative material values and universal authored nodes with typed parameter storage, owned MaterialExpression objects, and detached compiler snapshots.

Last reviewed: 2026-09-14

Status: Active
Completed:

## Current Status

The design is selected; the typed-value/expression implementation has not started.
The preliminary ImportedSurface cleanup removes the obsolete shipped material and
its production initializer; scene imports continue to generate structural parents.
An explicit `asset material-template` entry creates PBRSurfaceMaterial_MR at an
unused destination for future instance-parent workflows. This optional recipe
remains in scope for typed-expression migration, but no template asset is shipped
or automatically initialized.
Stage 0 qualifies object ownership and captures behavioral baselines before the
shared API changes begin.

Preliminary cleanup/creation validation (Win64 Debug):

- Workspace `all` build passed, including the new explicit template API/command:
  `Build/.agent-state/logs/20260914-151445-172628-39132-cmake.log`.
- Cleanup's affected selection passed all 87 regular native targets (including
  material/import Vulkan integration):
  `Build/.agent-state/logs/20260914-150759-964152-41780-ctest.log`.
- After adding the explicit template, MaterialTests passed 200 cases in 19 suites:
  `Build/.agent-state/logs/20260914-151551-987420-39428-MaterialTests.log`.
- Final SceneImportTests passed all 10 cases after fixture lifetime cleanup:
  `Build/.agent-state/logs/20260914-151934-635941-38272-SceneImportTests.log`.
- Asset command forwarding/grammar tests passed 33 cases. The new command was
  exercised against the isolated `Build/PBRSurfaceMaterialCommand/Test.dproject`:
  preview wrote no package, apply produced a compatible v10 package, and a
  repeated apply failed with unchanged file hash. Cook published one package:
  `Build/.agent-state/logs/20260914-151735-930766-19388-DurinAssetTool.log`.
- Post-removal identity audits cover 18 Sandbox-mounted and 14 RoadWeaver-mounted
  packages, with no reference to the retired template. Reports:
  `Build/ImportedSurfaceRemoval-Sandbox-after.json` and
  `Build/ImportedSurfaceRemoval-RoadWeaver-after.json`. A maintenance apply
  recreated neither the retired nor optional template.

The user explicitly requests rebuilding the small existing material set instead
of upgrading old assets. Recreate DefaultMaterial, required
standard material functions, and affected fixtures from current recipes. Do not
implement a legacy reader, conversion tool, neutral export/import bridge, or
dual-schema transition. Inventory dependencies to confirm the rebuild closure;
preserve externally referenced asset identities and built-in parameter IDs.

The inspected workspace contains Engine, Sandbox, and RoadWeaver. Current
`FMaterialParameterValue` reflects Scalar, Vector2, Vector3, Vector4, Texture,
SamplerState, and TextureFallback simultaneously; its enclosing declaration or
override supplies the discriminant. `FMaterialProgramNode` likewise reflects
every node payload, and both materials and functions persist arrays of it.
`FMaterialProgram` currently crosses authoring and compilation boundaries.
`DMaterial::ParameterSchema` is already a transient graph-derived projection;
this ownership decision must survive the refactor.

The obsolete 58,379-byte ImportedSurface template is removed before this refactor.
Use newly imported structural parents, explicit PBRSurfaceMaterial_MR templates,
and representative test graphs for size
comparisons. Per-expression object/export records introduce overhead that must
be measured alongside eliminated fields. Capture fresh baselines in Stage 0.

## Goal

An authored value contains exactly its selected value. An authored expression
owns only fields meaningful to its node family. Material instances store typed
overrides. Compilation consumes detached immutable data, and cooked execution
does not require expression objects. Stable parameter, node, and function-port
identities required by dependents and existing editing/rendering behavior survive
asset reconstruction.

## UE Reference And Selected Decisions

Epic's public API documents show three useful boundaries:

- [FMaterialParameterValue](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialParameterValue)
  has a type discriminator and union-based alternatives with typed accessors.
- [UMaterialInstance](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterialInstance)
  stores separate scalar, vector, texture, and other parameter arrays.
  [FScalarParameterValue](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FScalarParameterValue)
  combines parameter identity with a scalar value.
- [FMaterialExpressionCollection](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialExpressionCollection)
  holds object references to expressions. Concrete classes such as
  [UMaterialExpressionAdd](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterialExpressionAdd)
  declare their own inputs and retained defaults.

These are public API observations, not a claim that UE's private package or
compiler implementation is identical to the proposed Durin implementation.

### Parameter values and persisted records

Keep `FMaterialParameterValue` as a non-reflected C++ value API backed by
`std::variant<float, FVector2, FVector3, FVector4, FMaterialTextureValue>`.
`FMaterialTextureValue` groups a texture reference, sampler state, and fallback.
The alternative is the type authority: derive `GetType()` from it and expose
checked accessors and factories. Do not maintain a separately mutable tag or
silently convert a type mismatch. Equality compares only the active alternative.
Do not put an unsupported `std::variant` into `DPROPERTY`.

Persist instance overrides as five reflected typed arrays, with entries holding
`ParameterId` and their concrete value. Texture entries hold the whole texture
value including sampling policy. Reject duplicate IDs across arrays at the
validated mutation/load boundary. Preserve existing orphan overrides and their
type; reject an incompatible active assignment without silently dropping it.
An override explicitly equal to its parent remains an override.

Split common parameter identity/presentation metadata from its typed default.
Typed parameter expressions own this metadata and their concrete default.
Scalar range and texture usage belong to the applicable expression family.
The unified definition/override views become derived transient C++ projections,
with documented invalidation on owner revision changes. Migrate direct-field and
`span` consumers rather than introducing a second writable table. Retain stable
parameter GUIDs and existing built-in IDs.

Transient variants and derived caches need explicit reference collection when
they can outlive their reflected owner. Transactions, clipboard state, pending
edits, and render publication must retain their texture resources through the
existing ownership mechanisms. Cook writes explicit typed logical records;
neither variant indices nor native C++ memory layout become a wire contract.

### Authored MaterialExpression objects

Introduce `DMaterialExpression : DObject`. Materials and functions own their
expression objects through Outer and a reflected EditorOnly collection of
`TObjectPtr<DMaterialExpression>`. Expressions are package-internal objects,
not independently managed asset files. The base carries stable node identity
and the common expression interface, not Parameter, UVSettings, or a universal
Inputs/defaults payload. Presentation stays in the existing separate GUID-keyed
editor data and remains excluded from compilation.

Use concrete reflected types for constants, typed parameters, arithmetic,
sampling, swizzle, coordinate operations, surface operations, and function
terminals/calls. Factor shared implementation through bounded family bases where
the fields actually match. Stage 0 maps every existing opcode to a concrete
type; no supported opcode may fall back to the universal persisted node.

Expression inputs retain stable source-node/output identities. Keep the current
GUID link scheme instead of requiring pointer links between expressions. Each
expression declares its applicable inputs and retained disconnected defaults.
Connections continue to override defaults without erasing them. UV settings
belong only to sampling/coordinate expressions; parameter metadata belongs only
to parameter owners. A function-call expression owns its callee reference and
port bindings, eliminating the independent authored call table after migration.
Function signature metadata remains function-owned.

Class/type replacement is a validated graph command. Preserve compatible links
and identity where existing semantics allow; reject incompatible changes or
explicitly disconnect them through the existing command policy. Undo retains
the replaced state. Do not persist every old class's inactive fields as hidden
history. Rebuilt recipes establish their intended defaults directly. Preserve
meaningful connected-input defaults and instance override semantics in the new
model; historical inactive payload preservation is not an asset requirement.

### Compiler and editor boundaries

Lower the authored expression graph on the owning thread into an immutable
value snapshot, then perform function expansion, validation, normalization, and
code generation using detached data. `FMaterialProgramNode` may temporarily
serve as the lowering target, but must cease being the authored persistent
model. By completion, compiler nodes use bounded typed payloads and omit editor
metadata. Worker snapshots contain no live expression pointers or object-load
operations; capture resource/dependency identities through the established
compiler snapshot contract.

Preserve atomic candidate validation, preview working copies, Apply/Discard,
revision invalidation, and stale compilation rejection. Copying a collection
of object pointers is not a deep graph copy. Working-copy creation and Apply
must duplicate/reparent owned expressions correctly without sharing mutable
children with the source. Deleted expressions must not remain serializable
merely because their Outer still points into the asset. History retains deleted
state through the transaction system rather than abandoned package children.

Keep DAST v10, its default-baseline rules, and its whole-container replacement
semantics. Do not add generic array patches or a new global package format for
this material schema change. Qualify expression CDO/default-relative saving,
owned-object export discovery, duplication, and EditorOnly graph stripping
before relying on them. Fix a demonstrated generic ownership defect at its
owner with focused tests; do not invent a material-specific package serializer.

## Implementation Stages

### Stage 0: Qualify ownership and capture rebuild baselines

- [x] Remove the unused ImportedSurface asset/initializer and retain an explicit,
  non-overwriting PBRSurfaceMaterial_MR creation entry for optional instance use.

- [ ] Inventory every existing opcode, parameter type, and direct-field consumer
  across source and test roots of all projects in `Durin.dworkspace`; record the
  old-field to expression-field mapping, including function defaults and calls.
- [ ] Confirm the DefaultMaterial and standard-function dependency
  closure. Capture package identities, inbound references, dependent instance
  parameter IDs/overrides, rendered reference images, and package section sizes.
  Include affected authored/cooked fixtures. Internal node IDs and graph layout
  may be regenerated; preserve identities referenced by dependent assets.
- [ ] Qualify a small polymorphic owned-expression graph through Save/Load,
  DuplicateObject, edit-copy/Apply, deletion, Undo/Redo reference retention, and
  Cook stripping. Verify no deleted or editor-only descendant leaks into exports.
- [ ] Finalize the concrete expression class list and value/reference-collection
  API signatures; record any demonstrated infrastructure prerequisite here.

Completion: every supported field has a destination or an explicit discard
classification, the rebuild closure is known, and object ownership/copy/filter
behavior has executable evidence. No production rebuild precedes this gate.

### Stage 1: Introduce typed parameter values and instance storage

Depends on Stage 0.

- [ ] Implement the transient typed value API and reflected typed override records.
- [ ] Split parameter metadata/default ownership and migrate resolution, instance
  editing, render layers/proxies, resource collection, and Cook metadata consumers.
- [ ] Remove independent mutable type/value pairs from new mutation APIs; migrate
  editor controls and all workspace consumers of the old direct fields.
- [ ] Verify every value alternative, texture sampler/fallback, mismatch rejection,
  duplicate IDs, orphan behavior, inherited values, explicit equal overrides,
  reference retention, and save/load of typed records.

Completion: no new persisted parameter record contains inactive alternatives;
the unified runtime API has one type authority. Tests construct the new schema
directly; no compatibility storage or asset conversion structures are introduced.

### Stage 2: Add typed expressions and detached lowering

Depends on Stage 1 and the ownership gate in Stage 0.

- [ ] Implement the mapped expression types and shared material/function collection.
- [ ] Make parameter nodes the sole authored definition owners; move callee and
  port bindings into call expressions, and preserve function interface rules.
- [ ] Implement graph validation and deterministic detached snapshot lowering.
  Preserve cycle/depth/count limits, source diagnostics, multi-output GUIDs,
  disconnected defaults, and function dependency invalidation.
- [ ] Migrate compilation, code identity, derived schema, and Cook consumers.
  Exclude presentation and irrelevant payloads from shader identity; bump only
  affected material, compiler/cache, and cooked-payload schemas.
- [ ] Verify node-family semantics and rendered/compiler parity against Stage 0.

Completion: materials and functions compile from typed expressions without a
second writable authored program or call table; workers do not inspect objects.

### Stage 3: Migrate authoring and import workflows

Depends on Stage 2.

- [ ] Migrate MaterialGraphDocument commands, Details, pin/catalog code, preview,
  graph replacement, clipboard, transaction reference collectors, and Apply.
- [ ] Update standard function recipes, the explicit PBRSurfaceMaterial_MR template, scene import,
  asset creation, and all workspace/test graph construction helpers.
- [ ] Verify copy/paste GUID remapping, parameter ownership, function ports,
  delete/restore, class replacement, Undo/Redo, save/reopen, Apply/Discard, and
  reference lifetime after closing editors and running collection.

Completion: supported graph operations run against expressions end to end;
working and source assets never share mutable expression children.

### Stage 4: Rebuild material assets and qualify behavior

Depends on Stage 3.

- [ ] Recreate DefaultMaterial and required standard functions
  directly with the new expression APIs and updated recipes. No old-schema load
  is needed to build them; do not create an upgrade or conversion path.
- [ ] Recreate affected instances and fixtures if the dependency inventory finds
  them. Preserve referenced package/object identities, built-in parameter IDs,
  and required function-port IDs. Generate fresh internal node IDs/layout where
  appropriate. Validate staged packages before atomic replacement and keep a
  precise changed-asset manifest; leave unrelated assets alone.
- [ ] Rebuild affected derived data and cooked output for Sandbox and RoadWeaver.
  Verify graph-stripped runtime defaults, overrides, dependency residency, and
  Game startup/rendering without expression authoring data.
- [ ] Compare fresh structural import parents and the retained material set: package section
  bytes, node/object/export counts, save/load allocation/time, and render output.
  Explain object overhead separately from payload reduction. Do not claim size
  or performance wins from record counts alone; investigate regressions before
  accepting this gate.

Completion: fresh readers load all rebuilt assets; semantic and
render evidence passes, and size/performance results are recorded explicitly.
No external-asset compatibility is required by the current user instruction.

### Stage 5: Remove transition code and publish final contracts

Depends on Stage 4.

- [ ] Remove old reflected universal values/nodes, authored program/call storage,
  legacy overloads, and redundant canonicalization paths.
  Search all three projects for remaining production consumers and old schemas.
- [ ] Reject unsupported material schemas before publishing a partially loaded
  graph; never interpret a missing new collection as a valid empty old asset.
- [ ] Update the owning runtime material documentation and
  [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
  to describe implemented ownership, snapshot, transaction, and Cook contracts.
- [ ] Complete an `all` build of the workspace, affected native suites, renderer
  qualification, both projects' Cook/Game checks, and documentation validation.
- [ ] Record exact receipts and close the plan only after all gates pass.

Completion: one authored model and one typed value contract remain; no permanent
legacy material reader or alternate full-field save path remains.

## Validation And Scope Boundaries

Follow [build guidance](../Agents/BuildAndRun.md) and
[test guidance](../Agents/Testing.md) when implementing. Relevant suites include
material schema/instances, graph operations, function expansion, compile lifecycle,
render proxies, scene import, object/package serialization, clipboard and
transactions, plus Vulkan material/import coverage. Recompute exact registered
targets from the changed ownership rather than copying historical test counts.

Use [Serialization](../Runtime/Core/Serialization.md),
[Asset Packages](../Runtime/Assets/AssetPackages.md), and
[Transaction Records](../Editor/Architecture/TransactionRecords.md) as current
infrastructure contracts. This plan replaces the value-node authoring boundary
in Material Graph Operations as implementation lands, while preserving the
graph-owned parameter decision in
[Material Graph Owned Parameters](MaterialGraphOwnedParameters.md).

A universal reflected variant facility, arbitrary polymorphic containers,
generic array delta serialization, shader optimization, and unrelated material
features are outside this plan. Adjacent `FMaterialFunctionDefault` compaction
is limited to changes required for typed expression/signature correctness; a
broader redesign needs measured justification and an explicit plan update.
