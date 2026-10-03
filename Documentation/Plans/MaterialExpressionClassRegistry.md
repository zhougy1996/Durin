# Material Expression Class Registry Plan

Summary: Make reflected expression classes the authoring and editor registration boundary while preserving the existing detached Build-to-MIR compiler pipeline.

Last reviewed: 2026-10-03

Status: Completed
Completed: 2026-10-03

## Current Status

All four stages are complete. Engine owns immutable class-associated descriptions
and shared input access; MaterialEditor owns class-keyed creation/palette policy,
inspection and registered factories. Graph commands preserve typed initialization,
validation and transactions. The final all build and 399 cases across ten material
feature targets passed; documentation and plan validators also passed.

The former 70 opcode descriptor families are represented by concrete class
adapters (separate constant widths and scalar/vector parameter classes). MIR
opcodes remain semantic metadata rather than registration identity. Existing
Build, signatures, normalization, generation, cooked formats and reflected asset
fields remain unchanged. Collection inspection now correctly reads its live type.

The prior unrelated macOS application-test discovery limitation and two baseline
StaticMesh reimport failures remain recorded below. They are not passing gates or
part of this registration migration; no application smoke or GPU qualification
was required or executed.

## Goal

A normal expression is added through its concrete class, associated authoring
description, and Build implementation. Palette generation, inspection, and
creation consume the same class-associated information without separate
opcode-to-class/name/input-label lists. Internal MIR changes do not require
editor registration.

Authoring descriptions and normalized MIR signatures have distinct purposes:
descriptions expose editable inputs and output behavior, while MIR signatures
validate emitted operations. Both continue to use the existing value types and
stage/spatial semantic rules.

## Scope and Selected Decisions

- Use the reflected DMaterialExpression class as authoring identity and the
  registry key. Do not add a parallel exhaustive authored-node enum.
- Associate stable authoring information with the class. Keep menu layout,
  shortcuts, search ranking, and creation presentation in MaterialEditor.
- Retain typed creation payloads for selected assets, function ports, and numeric
  initialization. A class-based factory must not erase required asset context or
  bypass graph commands and transactions.
- Preserve DMaterialExpression::Build(MIR::FEmitter&) and the detached compiler
  input contract. Build emits a node once and publishes its indexed or stable
  GUID outputs; do not introduce output-index compilation callbacks.
- Preserve node and parameter GUIDs, function port identities, retained input
  defaults, multi-output sample sharing, width inference, and Undo/Redo behavior.
- Keep normalized MIR independent of expression-object lifetime. No authoring
  object or editor registry pointer may escape into worker inputs/results.
- Preserve MIR opcode numbers and canonical/cooked formats unless Stage 0 finds
  a necessary change and records a revised decision before implementation.
- Preserve complete typed shapes for inspection and structured creation while
  exposing selected shapes in the palette. Asset-bound expressions remain
  inspectable even when not directly creatable without an asset payload.
- This plan does not introduce a universal serialized node record, multiple
  compiler backends, a shader generator rewrite, or a compile-lifecycle rewrite.

## Implementation Stages

### Stage 0: Confirm ownership, consumers, and compatibility

Outcome: a bounded migration contract with no unresolved identity or metadata
ownership decisions.

- [x] Inventory expression classes and all consumers of class/opcode mappings,
  input traversal, signature lookups, creation actions, and inspection payloads.
  Cover the source and test roots of every project in Durin.dworkspace.
- [x] Classify ordinary math, fixed-width/context, sampling, parameter,
  asset-bound, and dynamic function/Surface expressions. Identify information
  that comes from a class versus an instance or function signature.
- [x] Select the smallest class-associated description mechanism: reflected
  metadata, typed static descriptors, or class adapters. Record placement,
  editor-only stripping, and ownership; Engine must not depend on MaterialEditor.
- [x] Define registry initialization and borrowed class/descriptor lifetimes.
  Avoid per-frame discovery, temporary expression construction during palette
  search, or unnecessary owner retention.
- [x] Audit Build, normalization, and validation to distinguish existing checks
  from actual gaps. Confirm which authored operations disappear before code
  generation; record the precise remaining compiler-boundary work.
- [x] Freeze baseline catalog shapes, search aliases, representative inspected
  graphs, compiler identities, and the affected asset corpus. Decide whether
  any package change is needed before altering persisted fields or class names.
- [x] Record API changes, affected targets, and selected validation lanes using
  the repository test registry. Record existing failures separately.

Completion condition: selected metadata/factory contracts and migration inventory
are written here, with no unresolved questions that block Stage 1.

### Stage 1: Associate authoring descriptions with expression classes

Dependency: Stage 0.

Outcome: expressions supply consistent authoring information through one
class-associated contract.

- [x] Implement descriptions for names/search aliases, authored input labels,
  input access, and fixed/adaptive/dynamic output behavior according to the
  Stage 0 ownership decision. Share underlying type and semantic rules without
  treating normalized MIR signatures as the complete authoring schema.
- [x] Migrate ordinary expressions and cover constants, parameters, sampling,
  TextureCoordinates, collections, function ports/calls, and Surface attributes.
  Preserve instance-dependent pins, output IDs, and texture/resource outputs.
- [x] Migrate description/input consumers and remove superseded mappings only
  after all consumers use the replacement. Search all workspace project roots
  for shared API consumers.
- [x] Verify input traversal/default editing, width inference, multi-output
  sampling, parameter reachability, and source-located diagnostics with relevant
  existing tests and targeted tests for new contract boundaries.
- [x] Validate affected project targets and complete an all build for shared
  Engine API changes.

Completion condition: class-associated descriptions are authoritative for the
migrated authoring information, with no competing legacy mapping and no change
to representative graph compilation results.

### Stage 2: Generate inspection and creation from a class-keyed registry

Dependency: Stage 1.

Outcome: the editor catalog, existing-node inspection, and creation commands
share a class-keyed registration boundary.

- [x] Key expression registrations by reflected class and bind palette policy
  and initialization strategy to the authoring description. Validate duplicate
  registrations and incomplete direct-creation entries.
- [x] Generate catalog shapes and inspect existing expressions by class rather
  than using compiler opcodes as node identity. Keep opcode use confined to
  places where operation semantics are actually required.
- [x] Route ordinary and asset-aware creation through class factories with
  typed initialization payloads, graph validation, and transaction publication.
- [x] Retain current search ordering/aliases, compatible-source filtering,
  hidden dimensional shapes, numeric inference, and material/function eligibility.
- [x] Remove the superseded opcode-to-class catalog machinery and update all
  structured callers, clipboard paths, and tests.
- [x] Verify direct creation, selected-asset initialization, dynamic pins,
  clipboard, Undo/Redo, package round trips, and preview/Apply behavior with
  bounded material editing coverage. Complete an all build for editor changes.

Completion condition: adding a representative ordinary expression requires no
new opcode switch in catalog construction, inspection, or creation. Internal
MIR operations remain unavailable as authoring creation actions.

### Stage 3: Close compiler-boundary and documentation acceptance

Dependency: Stage 2.

Outcome: authoring identity and normalized compiler semantics have a verified
boundary, with implemented contracts documented outside this plan.

- [x] Implement only the compiler-boundary gaps identified in Stage 0. Preserve
  the existing lowering path and operation numbering; record any necessary scope
  change before proceeding.
- [x] Verify TextureCoordinates lowers to UVChannel, normal samples produce
  internal decoding, functions expand, and static selectors prune inactive
  branches. Check that normalized input rejects unsupported authored-only
  operations at the appropriate validation boundary.
- [x] Compare representative canonical identities, layouts, resources,
  diagnostics, generated results, and cooked round trips against the baseline.
  Record intentional differences rather than silently updating expectations.
- [x] Run the selected material feature regression and affected project builds.
  Reuse applicable passing evidence; keep environmental and baseline failures
  explicit and do not turn them into passes.
- [x] Update the authoritative expression-building and graph-operation
  contracts, validate documentation, and close this plan only after all required
  gates have evidence.

Completion condition: the class-driven authoring flow is complete, compiler and
asset compatibility gates are satisfied, and lasting contracts reside in their
owning documentation domains.

## Stage 3 Audit Refinement

The targeted malformed-IR test passes against the unchanged compiler. Further
inspection confirms `ResolveMaterialProgramNodeSemantics` explicitly returns no
semantics for StaticBool, StaticSwitch, QualitySwitch, and FeatureLevelSwitch;
`IsValidMaterialValueSemantics` also excludes StaticBool from normalized values.
Together with the FunctionInput..AppendVector validation exclusion, this already
rejects every authored-only operation before generation, including unreachable
nodes. The Stage 0 assumption of a remaining selector-validation gap is therefore
withdrawn: Stage 3 adds boundary regression coverage and documentation, with no
compiler implementation change. Existing lowering, canonical identities, resources,
layouts and cooked formats keep their baseline implementations and expectations.

## Validation Evidence

Stage 1: all build passed. MaterialCompilerTests passed 68 cases (including the
new description lifetime/shape contract), MaterialGraphEditingTests passed 102,
and MaterialEditorInteractionTests passed 66. Names/input labels and search aliases
have no competing editor copy; only the legacy policy key/class selector remains
for Stage 2. Engine, Sandbox, and RoadWeaver consumers were searched and migrated.

Stage 2: all build passed. Direct feature targets passed 317 cases: graph editing
104, editor interaction 66, function 30, runtime 91, editing persistence 13,
package 6, and cook 7. New checks cover shared-opcode classes, forged class/width
payload rejection without mutation, and inspection fallback by actual class.
Collection asset creation/persistence additionally checks the live declaration's
output type. No opcode-to-class selector or opcode-keyed editor registry remains.
Function ports/calls are explicitly context-bound registrations, with separate
creation payloads rather than empty direct-creation shapes.

Intentional inspection correction: collection reads previously missed direct
catalog lookup and could acquire Constant/Float display metadata. Class-based
inspection now reports CollectionParameter and the live declaration type. This
changes only the detached editor view, not the asset or compiled program.

Stage 3: the final direct feature regression passed 399 cases with zero failures
and zero disabled cases: compiler 69, graph editing 104, editor interaction 66,
function 30, runtime 91, editing persistence 13, package 6, cook 7, compile
lifecycle 5, and thumbnail 8. XML receipts are under
`Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`. The final all build passed
(log `Build/.agent-state/logs/20261003-183702-329638-12984-cmake.log`). All four newly added cases also passed when executed alone. Final ABI
export annotations and compile-time adapter inheritance checks do not change
runtime test inputs; applicable passing runtime evidence is reused.

The new normalized-boundary test covers reachable and unreachable authored-only
nodes, failed generation and empty failed-normalization publication. Existing
expression/function/static-selection tests retain their UV lowering, normal
sample sharing/decoding, function expansion, pruning and diagnostic expectations.
Compiler, canonical/layout/resource and Cook round-trip expectations were not
rewritten. Diff comparison against `75cc5e095` confirms unchanged Build methods,
function lowering, signatures, compiler/generator, MIR declarations, reflected
expression fields, Cook implementation and all three project asset content roots.
The only intentional inspected-graph difference is the collection correction
recorded above. Lasting contracts now live in expression-building and graph-operation
documentation; changed-document and all-plan validation passed.

Prior unrelated limitations retained for handoff: batch CTest application-host
discovery failed in `Build/.agent-state/logs/20261003-180739-499640-4333-ctest.log`;
StaticMeshMaterialSlotReconciliationPreservesStableIndices and
FixedRowAssignmentRoundTripsByIndex failed against baseline `b3666ce05` in
`Build/.agent-state/logs/20261003-180958-155204-5240-StaticMeshMaterialTests.log`.
Direct material targets avoided the former; this plan did not change or rerun the
latter reimport behavior.

## Stage 0 Migration Contract

- Inventory: Engine owns reflected expression classes, Build, numeric defaults,
  signatures, validation, normalization, and detached snapshots. MaterialEditor
  owns catalog/search, document inspection/creation, input commands, edit sessions,
  clipboard, layout, shortcuts, and the read model. Native material tests consume
  those public contracts and the private input helper. Searches of Engine,
  Sandbox, and RoadWeaver source/test roots found no additional catalog consumers
  in the latter two projects. All three projects are declared in Durin.dworkspace.
- Use typed static class adapters in Engine: one process-lifetime description per
  concrete reflected class, with names, aliases, input labels, supported authoring
  shapes, and fixed/adaptive/instance output behavior. Register concrete constant
  widths separately; the shared vector parameter class retains its supported
  width variants. Class pointers are borrowed from reflection, never asset owners.
  Initialize once after reflection initialization; never construct probe objects.
  Descriptions are nonserialized runtime metadata (no editor stripping is needed
  in this first implementation); Engine has no MaterialEditor dependency.
- MaterialEditor owns a smaller class-keyed policy table with category, palette
  shape, and direct/asset-bound creation policy. Class plus result type identifies
  a catalog shape. Opcode remains a semantic field for numeric rules and Build
  compatibility, not a registry key. Factory creation uses the registered class
  and existing typed asset/port payloads inside validated graph commands.
- Fixed/context nodes and concrete constants expose fixed shapes. Adaptive math
  exposes numeric shapes; swizzle and AppendVector retain instance-controlled
  width. Samples retain indexed outputs/resource selectors. Collections, function
  ports/calls, and Surface masks/bindings derive live pins from their instances.
  Move reflected input traversal/default lookup to Engine so all editors share
  the same authored connection access contract. MIR signatures supply shared type
  rules at description initialization; authoring overrides/broadcast and dynamic
  pins remain separate from normalized operation signatures.
- Compatibility baseline is commit `75cc5e095`: preserve the 70 descriptor families,
  all catalog widths and palette ordering/aliases, the representative existing
  compiler/cook/package expectations, and the 22 repository .dasset files. No
  reflected field/class rename, opcode renumbering, canonical version, cooked
  version, or package migration is necessary. Registration becomes concrete-class
  based, so its record count may differ from descriptor-family count.
- Compiler audit: TextureCoordinates emits UVChannel; samples share a fetch and
  emit DecodeNormalRG only for normal RGB; function expansion and static pruning
  occur during Build. Existing normalized validation rejects the contiguous
  FunctionInput..AppendVector authoring range. Stage 3 will make this boundary
  explicit for all authored-only operations, including later static selectors,
  and test malformed detached IR. No lowering rewrite is required.
- Validation lanes: MaterialGraphEditingTests, MaterialEditorInteractionTests,
  MaterialCompilerTests, MaterialFunctionTests, MaterialRuntimeTests,
  MaterialEditingPersistenceTests, MaterialPackageTests, MaterialCookTests,
  MaterialCompileLifecycleTests, and MaterialThumbnailTests; all build after
  shared/editor changes validates workspace targets. Prior batch discovery and
  StaticMesh reimport failures remain separately recorded; direct test targets
  avoid unrelated application-host discovery. StaticMesh reimport behavior is
  outside the changed registration/compiler boundary.

## Execution and References

Follow [Build and Run](../Agents/BuildAndRun.md),
[Testing](../Agents/Testing.md), and
[Documentation](../Agents/Documentation.md). Implementation commits update this
plan's status/checklists and carry the exact Plan and Stage trailers required by
the repository handoff rules. Creating this plan does not complete an
implementation stage.

Authoritative local contracts:

- [Material expression building](../Runtime/Rendering/MaterialExpressionBuilding.md)
- [Material graph operations](../Editor/Architecture/MaterialGraphOperations.md)
- [Material editor lifecycle](../Editor/Architecture/MaterialEditorLifecycle.md)
- [Material compatibility boundary](../Runtime/Rendering/MaterialSystem.md#compatibility-boundary)

UE design references are official API documentation, not a claim that Durin must
copy UE implementation or historical compilation paths:

- [UMaterialExpression](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterialExpression): class-associated categories, labels, keywords, inputs, and outputs.
- [UMaterialEditingLibrary](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/MaterialEditor/UMaterialEditingLibrary): class-based and selected-asset expression creation.
- [UMaterialExpression::Build](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UMaterialExpression/Build): lowering through MIR::FEmitter.
- [MIR::FEmitter](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FEmitter): high-level expression lowering and internal IR emission.
