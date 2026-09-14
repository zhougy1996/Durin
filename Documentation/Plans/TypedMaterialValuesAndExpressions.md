# Typed Material Values And Expressions Plan

Summary: Replace all-alternative material values and universal authored nodes with typed parameter storage, owned MaterialExpression objects, and detached compiler snapshots.

Last reviewed: 2026-09-14

Status: Active
Completed:

## Current Status

Stage 0 is complete. The package/duplication and transaction ownership
primitives now have executable qualification for polymorphic editor collections,
independent Apply copies, detached deletion, GC retention, Undo/Redo, and Cook
descendant stripping. Stage 1 is in progress: instance persistence now uses five
typed reflected arrays, and the editor's property transactions target the matching
array. The former unified override record is a non-reflected read-only projection.
Cross-array duplicate IDs and unsupported instance schemas fail serialization;
orphan values and sampling policy remain retained. The transient variant value API
and expression/default ownership migration are still outstanding.
Stage 1/2 preparation now supplies 46 concrete reflected expression classes,
covering every supported opcode, with common parameter metadata, concrete defaults,
applicable pins, fixed coordinate defaults, and expression-owned call bindings.
All graph-side connections use `FMaterialExpressionInput` with expression GUID,
output index, and output GUID. Numeric defaults are separate fields on their
concrete expression (or call binding), remain present while connected, and survive
disconnection. Texture and Surface connections carry no numeric default storage.
Lowering explicitly creates `FMaterialProgramLink`; graph records do not embed it.
The unified-input round-trip and lowering tests passed:
`Build/.agent-state/logs/20260914-164446-220485-36068-MaterialTests.log`.
The corresponding workspace `all` build passed:
`Build/.agent-state/logs/20260914-164458-304142-28060-cmake.log`.
Per-expression lowering uses the existing program node only as a local intermediate.
The final entry point is expression `Build()` into typed IR. The current `Lower()`
and Program graph are temporary migration machinery and must be removed, rather
than retained as an adapter in the final expression-to-compiler path.
The source generator now validates detached IR directly instead of reconstructing
an authored Program graph. Direct checks cover topological indices, signatures,
depth, parameter bindings, finite constants/defaults, and swizzle bounds before
indexed generation. Five compiler tests passed, including malformed IR created
without any authored graph:
`Build/.agent-state/logs/20260914-165010-176048-31752-MaterialTests.log`.
The material regression passed all 206 cases (baseline recapture excluded):
`Build/.agent-state/logs/20260914-165031-730034-27456-MaterialTests.log`.
The final expanded-link bound and its rejection test passed the five-case compiler
suite in `Build/.agent-state/logs/20260914-165411-002037-36548-MaterialTests.log`.
The final workspace `all` build passed:
`Build/.agent-state/logs/20260914-165422-484367-39924-cmake.log`.
Direct expression construction now exists for all 43 non-function classes through
`FMaterialExpressionBuildContext` and virtual `Build()`. Numeric, coordinate,
sampling/multi-output, parameter, and Surface families emit IR without Program
nodes. The context owns traversal caches, cycle/depth/node/link checks, exact input
types, retained-default validation, and detached source/parameter records; failed
builds return diagnostics without partial IR. Six expression tests passed:
`Build/.agent-state/logs/20260914-170150-391127-31720-MaterialTests.log`.
After unique parameter-owner admission was added, the six tests passed again in
`Build/.agent-state/logs/20260914-170233-795443-34924-MaterialTests.log`; workspace
`all` passed in `Build/.agent-state/logs/20260914-170310-913201-38848-cmake.log`.
All 46 concrete classes now implement direct `Build()`, including function inputs,
outputs, and calls. A bounded owning-thread body provider supplies expression
collections and signatures; invocation-local caches bind GUID ports while sharing
detached IR storage. Numeric, texture, Surface, input-alias, and UV0 defaults are
preserved; texture defaults travel as a selected value alternative until sampling,
never as fabricated resource indices. Nested calls retain source paths and port
diagnostics. Required/mismatched bindings, missing/duplicate terminals, recursive
calls, cyclic defaults, invalid disconnected function expressions, and expanded
IR bounds reject the result without publishing partial IR or dependencies.
At this checkpoint, complete root-graph admission and material-owner/compiler
snapshot integration remained pending. The function owner exposed its expression
collection to the body provider; the production cutover is recorded below.
The final function increment passed all 14 expression/compiler cases and the
workspace `all` build:
`Build/.agent-state/logs/20260914-172718-182863-33704-MaterialTests.log` and
`Build/.agent-state/logs/20260914-172728-349041-37608-cmake.log`.
`FMaterialIRCompilerInput`, `NormalizeMaterialIR`, and `CompileMaterialIR` now
provide a detached IR-only route through normalization and backend compilation.
Validation is shared with source generation without constructing authored nodes;
parameter declarations are checked before pruning, while device layout limits
apply only to active bindings. Normalization prunes unreachable nodes, canonicalizes
commutative input ordering and selected immediate widths, excludes inactive Surface
root fields, and remaps source metadata. Both compiler entries share backend code.
The direct entry produced identical generated source, layout, stage entry points,
and shader hashes to the retained synthetic compiler baseline. An expression-built
Surface also compiled through this entry into all three required shader stages.
All 16 focused expression/compiler tests passed:
`Build/.agent-state/logs/20260914-173625-541443-37320-MaterialTests.log`.
All 213 material regression cases passed (baseline recapture excluded):
`Build/.agent-state/logs/20260914-173734-417984-31276-MaterialTests.log`.
MaterialVulkanTests and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-174126-861409-40076-MaterialVulkanTests.log` and
`Build/.agent-state/logs/20260914-174218-807732-35704-cmake.log`.
`DMaterialFunction` now persists its signature and owned expression collection
under ownership schema 2. The graph-complete load hook checks unique children,
exact ownership, abandoned descendants, expression identities, and local graph
validity. Apply duplicates candidates before replacing the collection, detaches
retired children, and preserves invalid external call references for diagnostics
at compilation. Dynamic collections have no fixed CDO children and are explicitly
serialized; load/duplication constructors do not create extra initial children.
Labels reside in presentation and survive position edits. Native Program setters
temporarily construct expression candidates, and getters produce non-reflected
projections; neither is an asset reader or upgrade path, and both are removed with
the remaining Stage 3 consumers. Old persisted function schemas are rejected.
The standard-function Cook test now constructs fresh recipe fixtures; shipped
packages remain pending the Stage 4 rebuild. The initial 214-case regression
passed 208 cases and exposed six migration assumptions; after fixing these,
all six plus the expanded expression suite passed (17 cases):
`Build/.agent-state/logs/20260914-180711-305954-35904-MaterialTests.log`.
This includes independent Apply/save/load/duplication, labels, direct Build from
the loaded owner, failed-edit atomicity, ownership/schema rejection, nested
diagnostics, and Cook/load without authored function assets.
SceneImportTests passed all 10 cases and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-180743-289632-31108-SceneImportTests.log` and
`Build/.agent-state/logs/20260914-180814-333622-32492-cmake.log`.
The final full material regression passed all 215 cases (baseline recapture
excluded): `Build/.agent-state/logs/20260914-180832-149010-30192-MaterialTests.log`.
The material owner now also persists an expression collection and concrete typed
terminal defaults under ownership schema 2. Program and the independent call
table are non-reflected projections. Material parameter setters write the owning
typed parameter expression; child property events refresh the derived schema,
classify shader changes, and notify the material owner for Undo/Redo and render
invalidation. Apply clones children before committing, and restored graphs reject
invalid ownership, orphan descendants, parameter declarations, and local links.
Function-call references are explicitly EditorOnly so Cook's dependency discovery
keeps them as build inputs rather than runtime package roots. Numeric ranges are
now restricted to scalar recipe parameters, matching their concrete owners.
The typed material owner save/load/duplication and failed-edit test passed:
`Build/.agent-state/logs/20260914-182108-148737-29444-MaterialTests.log`.
The first broad material run exposed stale reflection/fixture assumptions and
Cook reference policy; seven cases still require the shipped DefaultMaterial
rebuild. They remain outstanding Stage 4 gates, not accepted exclusions from
final validation. At this owner-storage checkpoint, production compilation still
consumed the temporary projection; the direct IR cutover is recorded below.
After updating the consumers and fixing the policy, all 209 material cases outside
the seven outstanding shipped-asset tests passed:
`Build/.agent-state/logs/20260914-182911-110013-40480-MaterialTests.log`.
The separate expensive baseline recapture was also excluded from this regression
run; it remains a final measurement gate.
Scene import now constructs explicit empty defaults for multiplier inputs so its
authored-recipe comparison matches the expression projection without mistaking a
fresh structural parent for an edited asset. All 10 SceneImportTests passed,
including parent reuse and reimport, and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-183455-085127-37156-SceneImportTests.log` and
`Build/.agent-state/logs/20260914-183543-724487-40508-cmake.log`.
Production compilation now captures the material's owned expressions through
`Build()` directly into `FMaterialIRCompilerInput`. The compile queue, synchronous
fallback, normalization, and backend compilation consume detached IR. Function
owner handles, paths, and revisions are captured with the snapshot and checked at
completion and Cook publication without rebuilding the temporary Program graph.
Root admission validates all expressions, including dead nodes, unique parameter
metadata, retained terminal defaults, exact output types, and authored link bounds.
Failed snapshots preserve the caller's previous input and owner stamps.
Parity checks compare generated source and layout with the temporary compiler,
and identity after applying the same IR normalization to both results. Legacy
Program identities can differ because direct normalization excludes inactive
terminal defaults; the existing IR encoding and version remain unchanged.
Program projections, native graph setters, and editor adapters remain temporary
consumers pending later stages. This increment does not complete the plan; work
stops after its validation and commit at the user's request.
The final runtime code passed 210 of 211 selected material cases in
`Build/.agent-state/logs/20260914-185555-139538-9892-MaterialTests.log`.
The remaining case expected an inactive connected default to change identity;
its corrected coverage verifies identity reuse while connected and a changed
identity after disconnection. All 22 expression/compiler/publication cases then
passed in `Build/.agent-state/logs/20260914-185923-504586-31324-MaterialTests.log`.
The seven shipped-asset cases and expensive baseline recapture remain excluded
from this incremental run, with their final gates still open.
All 10 SceneImportTests and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-185940-839991-25148-SceneImportTests.log` and
`Build/.agent-state/logs/20260914-190039-633122-20836-cmake.log`.
MaterialVulkanTests did not pass: loading the shipped schema-1 DefaultMaterial
failed before qualification could finish, and fixture teardown asserted after
the failed assertion. The retained failure is
`Build/.agent-state/logs/20260914-190022-999737-41368-MaterialVulkanTests.log`.
This GPU gate also remains pending the Stage 4 shipped-asset rebuild.
The IR immediate migration now replaces simultaneous literal/parameter/swizzle
members with `FMaterialIRPayload`, selecting no immediate, a bounded numeric
literal, a parameter GUID, or a bounded swizzle. Direct Build, normalization,
source generation, and tests use the selected payload. Canonical encoding rejects
opcode/payload disagreement before emitting bytes and retains the existing
explicit logical format; neither variant indices nor native layouts are encoded.
All 208 material regression cases passed (baseline recapture excluded):
`Build/.agent-state/logs/20260914-170724-973481-33104-MaterialTests.log`.
The retained compiler probe matches the pre-change receipt exactly: 10,156 canonical
bytes, 15,351 generated bytes, source hash `171b1d04339f5889f9cc577a58f72b52`,
identity `b551156d0f84f52bd667d942fdcf9cdf`, and 145,365 cooked bytes. MaterialVulkanTests
and all 10 SceneImportTests passed:
`Build/.agent-state/logs/20260914-171108-886248-29604-MaterialVulkanTests.log` and
`Build/.agent-state/logs/20260914-171216-096222-38600-SceneImportTests.log`.
The workspace `all` build passed:
`Build/.agent-state/logs/20260914-171317-875097-9060-cmake.log`.
At the initial expression-library checkpoint, neither owner had switched storage.
Both now persist concrete expression collections; the transient value API and
remaining native/editor consumers still need migration. No old-asset reader or
second persisted authored schema is introduced.
Four expression tests passed (including the complete class/opcode map, typed
Save/Load and duplication, retained defaults, and function-port lowering):
`Build/.agent-state/logs/20260914-161712-463235-32700-MaterialTests.log`.
The workspace `all` build passed:
`Build/.agent-state/logs/20260914-161905-075260-40812-cmake.log`.

Owner integration accounts for an observed loader ordering constraint:
`ApplyLinkerValues` invokes each object's Serialize in export order, so an owner's
Serialize cannot validate yet-unpopulated expression children. PostLoad runs after
all values, but its void API cannot reject publication. The new read-only
`DObject::ValidateLoadedObjectGraph` boundary now runs after package values are
restored, and after the entire selected batch for prepared reload graphs. Rejection
precedes PostLoad/final publication and preserves previous prepared output. The
same hook rejects invalid transient object graphs and duplicates after their values
are restored. Two qualification tests prove child-state visibility, cross-package
batch ordering, ordinary-load rollback, no PostLoad on failure, and output retention:
`Build/.agent-state/logs/20260914-162819-316245-40156-AssetPackageTests.log`.
The workspace `all` build passed:
`Build/.agent-state/logs/20260914-162850-788108-3916-cmake.log`.
Workspace `all` build and all 87 regular native test targets passed for this shared
lifecycle change (`20260914-162850-788108-3916-cmake.log` and
`20260914-163200-956456-5800-ctest.log` under `Build/.agent-state/logs/`).
The instance-storage increment passed 201 MaterialTests (the separate baseline
capture was excluded), all 10 SceneImportTests, MaterialVulkanTests, and the
workspace `all` build. Receipts:
`Build/.agent-state/logs/20260914-155649-432515-38264-MaterialTests.log`,
`Build/.agent-state/logs/20260914-160029-989191-33660-SceneImportTests.log`,
`Build/.agent-state/logs/20260914-160202-084851-30244-MaterialVulkanTests.log`, and
`Build/.agent-state/logs/20260914-160007-065658-39860-cmake.log`.
The preliminary ImportedSurface cleanup removes the obsolete shipped material and
its production initializer; scene imports continue to generate structural parents.
An explicit `asset material-template` entry creates PBRSurfaceMaterial_MR at an
unused destination for future instance-parent workflows. This optional recipe
remains in scope for typed-expression migration, but no template asset is shipped
or automatically initialized.
Stage 0 qualifies object ownership and captures behavioral baselines before the
shared API changes begin.

Stage 0 receipts (Win64 Debug, 2026-09-14):

- AssetPackageTests: 154 cases in 6 suites passed, including two new polymorphic
  collection/owned-child cases. Receipt:
  `Build/.agent-state/logs/20260914-152728-859524-32816-AssetPackageTests.log`.
- EditorOperationTests: 45 cases in 5 suites passed, including a new custom
  transaction that detaches a child, collects, undoes/redoes, and releases it
  when history is reset. Receipt:
  `Build/.agent-state/logs/20260914-153157-179283-40680-EditorOperationTests.log`.
- MaterialVulkanTests passed; the retained run is
  `Build/.agent-state/logs/20260914-153037-136204-9004-MaterialVulkanTests.log`.
  Its 28 reference PNGs are copied to `Build/TypedMaterialValuesBaseline/Images`.
  The preceding passing run cleaned its temporary images and is not the retained
  baseline. These runs establish correctness, not exclusive-lane timing.
- Fresh construct-free identity audits cover 18 Sandbox-mounted and 14
  RoadWeaver-mounted packages:
  `Build/TypedMaterialValues-Sandbox-identity.json` and
  `Build/TypedMaterialValues-RoadWeaver-identity.json`.
- A fresh optional template was created only in the isolated
  `Build/TypedMaterialValuesBaseline/Baseline.dproject`. Production assets were
  not rebuilt. `Build/TypedMaterialValuesBaseline/inventory.json` records source
  consumer locations, package identities/references, package section sizes,
  SHA-256 fingerprints, and retained image fingerprints; its adjacent
  `inventory.py` reproduces the inventory against the current checkout.
- Changed-document validation passed (one document). The affected-test selector
  expands these native-test-only edits to the Engine project; the two changed
  test targets were run in full, plus the explicitly required material Vulkan
  baseline. No production API changed, so no workspace `all` build was required
  for this ownership-qualification increment.

The ownership tests qualify existing primitives, not the future MaterialGraphDocument
implementation. No generic serializer defect was demonstrated. Clearing a reflected
collection alone leaves a live Outer child in authored exports; deletion must
reparent that exact child to a transient history owner before saving. Undo restores
the owner and collection, and history enumerates the child through its collector.
Do not mark a history-retained child as garbage. Apply duplicates children into
the destination and retires the old destination graph; copying pointers is invalid.
Cook removes unreachable editor-only children including their descendants.

The rebuild baseline now includes 11 freshly constructed recipe packages and 135
parameter/function-port identity records in
`Build/TypedMaterialValuesBaseline/RebuildRecipes`. The capture test passed:
`Build/.agent-state/logs/20260914-154353-732087-14732-MaterialTests.log`.
`measurements.tsv` records six save/load samples per package, with sample zero
reserved for warmup. Allocation counts/bytes are owning-thread Debug CRT allocation
requests (including reallocations), not peak memory, live bytes, worker allocations,
or release-build performance. All snapshots are taken offline without shader work.
The 92-file consumer inventory now includes program/function families and inferred
API consumers across all three projects. No downstream stage is marked complete.

Baseline medians from samples 1–5:

| Recipe | Save ms | Load ms | Save allocation requests | Load allocation requests | Save requested bytes | Load requested bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Default constructor | 66.272 | 20.688 | 67,090 | 19,147 | 3,255,948 | 1,005,877 |
| Explicit template | 3,032.630 | 1,495.500 | 3,227,215 | 1,119,410 | 181,817,900 | 58,601,947 |
| Structural plain | 159.661 | 84.523 | 173,807 | 77,454 | 8,516,817 | 3,808,872 |
| Structural transformed/packed | 391.980 | 192.599 | 410,128 | 162,740 | 22,344,720 | 8,460,080 |

Fresh structural packages are 5,211 and 9,100 bytes; their Names/Schemas/Values
sections are 3,138/365/776 and 3,270/369/4,529 bytes, respectively, with one object
export each. The default constructor package differs from the shipped default's
authored overrides; keep the shipped baseline separately. Function samples and
exact IDs are retained in the TSVs. Stage 4 compares the same recipes and sampling
instrumentation, and also rechecks the shipped package set and Vulkan images.
The unreferenced GraphAuthoringV5 fixture files will be removed in Stage 4 rather
than recreated as unsupported historical assets; current native tests construct
their authored and cooked fixtures from recipes.

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

Build the authored expression graph on the owning thread directly into an immutable
typed IR snapshot through expression `Build()` methods. Resolve connections,
applicable defaults, and function calls at this boundary; normalization and code
generation consume detached IR. `FMaterialProgramNode` may temporarily serve as
a migration target, but the final path contains no Program graph adapter or
reverse reconstruction of authored nodes. Compiler nodes use bounded typed
payloads and omit editor metadata. Worker snapshots contain no live expression pointers or object-load
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

## Field and opcode inventory

The following destinations are the implementation map, not declarations of
already implemented classes. All expression names below use the
`DMaterialExpression` prefix. Numeric family bases may share code and applicable
fields; no family base contains parameter, sampling, function, and surface payloads
together. The enum has no supported opcode at numeric values 3 and 30.

| Existing opcode | Concrete expression suffix(es) | Authored fields beyond node identity |
| --- | --- | --- |
| Constant | ScalarConstant, Vector2Constant, Vector3Constant, Vector4Constant | Concrete numeric value; class supplies width |
| Parameter | ScalarParameter, Vector2Parameter, Vector3Parameter, Vector4Parameter | Common parameter metadata and concrete default; scalar-only range |
| TextureParameter | TextureParameter | Parameter metadata, texture value including sampler/fallback, usage |
| TextureSampleParameter2D | TextureSampleParameter2D | Texture parameter fields, UV link and retained UV settings |
| TextureSample2D | TextureSample2D | Texture and UV links, retained UV settings |
| Add, Subtract, Multiply, Divide, Minimum, Maximum | Add, Subtract, Multiply, Divide, Minimum, Maximum | Two numeric inputs and retained defaults, numeric result width |
| Negate, OneMinus, Absolute, Saturate, Normalize, Sine, Cosine | Negate, OneMinus, Absolute, Saturate, Normalize, Sine, Cosine | One numeric input/default and applicable width; Normalize excludes scalar |
| Clamp | Clamp | Value, minimum, maximum inputs/defaults of the same numeric width |
| Lerp | Lerp | Two numeric inputs/defaults and scalar alpha input/default |
| MakeFloat2, MakeFloat3, MakeFloat4 | MakeVector2, MakeVector3, MakeVector4 | Exactly two, three, or four scalar inputs/defaults |
| Swizzle | Swizzle | Numeric source/default and selected component sequence; output width is sequence length |
| Splat2, Splat3, Splat4 | Splat2, Splat3, Splat4 | Scalar input/default; class supplies result width |
| TruncateToFloat, TruncateToFloat2, TruncateToFloat3 | TruncateToScalar, TruncateToVector2, TruncateToVector3 | Numeric source/default; class supplies result width |
| DecodeNormalRG | DecodeNormalRG | Float2 input/default; Float3 result |
| BlendNormalsRNM | BlendNormalsRNM | Two Float3 inputs/defaults |
| UVChannel | UVChannel | Scalar channel input/default; Float2 result |
| TextureCoordinates | TextureCoordinates | Channel, scale, offset, rotation inputs and retained defaults |
| MakeSurface | MakeSurface | Eight attribute inputs with applicable scalar/Float3 defaults |
| GetSurfaceAttributes | GetSurfaceAttributes | Surface input and selected output attribute mask |
| SetSurfaceAttributes | SetSurfaceAttributes | Base Surface input and attribute-keyed override links |
| FunctionInput | FunctionInput | Stable function port ID; signature remains function-owned |
| FunctionOutput | FunctionOutput | Stable function port ID and source link |
| FunctionCall | FunctionCall | Callee reference, GUID-keyed input/default and output bindings |

| Old field or record | Destination or discard classification |
| --- | --- |
| ParameterValue.ScalarValue / Vector2Value / VectorValue / Vector4Value | `float` / `FVector2` / `FVector3` / `FVector4` active alternative; matching reflected override array |
| ParameterValue.TextureValue / SamplerState / TextureFallback | One `FMaterialTextureValue` alternative and reflected texture override record |
| ParameterDefinition.Id, Name, DisplayName, GroupName, SortOrder, Presentation | Common parameter metadata owned by the parameter expression |
| ParameterDefinition.Type | Derived from the concrete parameter class/default; no independently writable tag |
| ParameterDefinition.Value | Concrete default on the owning typed expression; transient unified definition projection |
| ParameterDefinition.bHasRange, MinimumValue, MaximumValue | Scalar parameter expression only |
| ParameterDefinition.TextureUsage | Texture-owning parameter expression only |
| ParameterOverride.ParameterId / Type / Value | ParameterId plus concrete value in five arrays; transient unified override projection |
| ProgramNode.Id | Expression base node GUID; preserve GUID links and function-output IDs |
| ProgramNode.Opcode / ResultType | Concrete class plus bounded numeric-family width where applicable |
| ProgramNode.Inputs / InputDefaults | Shared `FMaterialExpressionInput` connection members and separate applicable defaults on the selected family; connected defaults remain retained |
| ProgramNode.Literal | Typed constant value; discard on non-constant families |
| ProgramNode.Parameter | Parameter-owner fields only; discard inactive parameter history on other families |
| ProgramNode.SwizzleLength / SwizzleX/Y/Z/W | Swizzle component sequence only; discard inactive components |
| ProgramNode.DisplayName | GUID-keyed presentation; excluded from detached compiler nodes |
| ProgramNode.FunctionPortId | Function terminal expression only |
| ProgramNode.SurfaceAttributeMask | GetSurfaceAttributes selection only |
| ProgramNode.SurfaceAttributes | SetSurfaceAttributes bindings only |
| ProgramNode.UVSettings | Sampling/coordinate families only |
| Program.Outputs | Material-owned terminal links and typed attribute defaults |
| Program.SchemaVersion / FunctionGraph.SchemaVersion | Required owner schema gate, independent of DAST v10 |
| FunctionGraph.Signature | Function-owned port definitions, stable port GUIDs, names and ordering |
| FunctionGraph.Calls / Material.FunctionCalls | Fold each call's Function, Inputs and Outputs into its expression; NodeId becomes expression Id |
| FunctionDefault.None / Numeric / Texture / Surface / Input / UV0 | Preserve the existing bounded signature-default record and validate selected semantics: absent, numeric, sampler/fallback, attribute constants, input GUID, or UV0 builtin |
| FunctionInputBinding.ExpectedType / Default | Retain port compatibility check and connected numeric default; never silently coerce |
| Graph presentation and import provenance | Separate editor-only owner data; no compiler/code identity contribution |

Value API decisions already fixed by the plan are `GetType()`, checked
`GetScalar()` / `GetVector2()` / `GetVector()` / `GetVector4()` / `GetTexture()`,
and the existing `MakeScalar` / `MakeVector2` / `MakeVector` / `MakeVector4` /
`MakeTexture` factories. Texture collection must visit and update the active
reference through `FReferenceCollector`, including reference-rewriting collectors.
Projection pointers/spans expire on owner revision changes. Snapshot lowering
replaces live texture/callee references with captured resource/dependency identities
before worker dispatch; moving a variant into a worker is not sufficient.

Concrete persistence and boundary decisions for implementation:

- Numeric operation inputs use a GUID link and numeric component vector of
  length zero (absent) or one through four; no other lengths are admitted.
  Each concrete operation declares its exact applicable pins, and its signature
  validates the selected input width before lowering. This includes fixed-width
  operations so a retained numeric input has one consistent absence/value
  representation without a separately mutable type tag. There is no
  parameter/texture/surface payload in a numeric input record. Sampling UV
  settings instead use fixed scalar channel/rotation and Vector2 scale/offset
  records with presence flags. This refines the initial fixed-pin representation;
  it is not a size or allocation improvement claim.
- Keep the existing bounded `FMaterialFunctionDefault` signature record for
  this migration. Its None/Numeric/Texture/Surface/Input/UV0 alternatives do not
  include resource object references. Validate its selected semantics using
  the function-owned port type; preserve InputId aliases, UV0, required flags,
  and connected numeric defaults. Broad signature-default compaction is outside
  the stated scope. Call expressions own only callee and typed port bindings;
  they do not contain parameter, arithmetic, or coordinate fields.
- `FMaterialParameterValue::AddReferencedObjects(FReferenceCollector&)` visits
  only the active texture alternative and writes the collector's replacement
  back. Both const and mutable checked accessors return the selected concrete
  value by reference; mutation never changes the alternative implicitly.
  `DMaterialInstance::SetParameterOverride(const FGuid&,
  const FMaterialParameterValue&) -> bool` derives its type from the value.
- The expression base has only node identity and virtual lowering behavior.
  `LowerMaterialExpressions(span<const TObjectPtr<DMaterialExpression>>,
  FMaterialProgram&, vector<FMaterialFunctionCall>&)` runs on the owning thread
  and returns `FMaterialProgramValidationResult`. Its intermediate is local to
  authoring/snapshot capture. Existing `SnapshotMaterialCompilerInput` and
  `BuildFunctionSnapshot` remain the admission boundaries that capture resource
  and callee identities before worker dispatch. The final worker node payloads
  are bounded C++ variants and contain no editor metadata or live object pointers.
- Graph replacement validates candidates before publishing owned children.
  Working documents and Apply duplicate owned hierarchies; deletion/replacement
  detaches the exact retired children into transaction ownership. A projection
  cache may be rebuilt on revision change but cannot be independently edited or
  reflected as an alternate authored graph.

The package rebuild closure contains DefaultMaterial and all seven shipped
standard functions. The only serialized material-to-material inbound edges in the
two mounted inventories are StandardPBR to SampleNormal, and StandardPBR_ORM to
SampleNormal and SampleORM. Neither mounted inventory contains a MaterialInstance;
engine runtime service lookups still require the DefaultMaterial identity.
The six files under `Engine/Tests/Data/Materials/GraphAuthoringV5` remain a separate
fixture disposition item: current test source contains no `GraphAuthoringV5`
consumer. Do not mistake their retired ImportedSurface fixture for a shipped asset
or revive its production initializer.

Fresh package byte baselines (one export per package; section bytes are measured
from the DAST directory, not estimated from reflected record counts):

| Package | Total | Names | Schemas | Values |
| --- | ---: | ---: | ---: | ---: |
| DefaultMaterial | 3,386 | 1,596 | 168 | 874 |
| DecodeImportedNormalRG | 12,712 | 2,940 | 371 | 8,462 |
| ImportedSurfaceValues | 51,236 | 2,936 | 371 | 46,990 |
| SampleNormal | 19,054 | 2,900 | 371 | 14,844 |
| SampleORM | 15,398 | 2,888 | 371 | 11,200 |
| StandardPBR | 77,540 | 3,202 | 419 | 72,938 |
| StandardPBR_ORM | 68,738 | 3,318 | 422 | 64,012 |
| UVTransform | 22,163 | 2,896 | 371 | 17,957 |
| Fresh optional PBRSurfaceMaterial_MR | 56,075 | 4,363 | 415 | 50,362 |

These are pre-expression values. No size or performance improvement is claimed.

## Implementation Stages

### Stage 0: Qualify ownership and capture rebuild baselines

- [x] Remove the unused ImportedSurface asset/initializer and retain an explicit,
  non-overwriting PBRSurfaceMaterial_MR creation entry for optional instance use.

- [x] Inventory every existing opcode, parameter type, and direct-field consumer
  across source and test roots of all projects in `Durin.dworkspace`; record the
  old-field to expression-field mapping, including function defaults and calls.
- [x] Confirm the DefaultMaterial and standard-function dependency
  closure. Capture package identities, inbound references, dependent instance
  parameter IDs/overrides, rendered reference images, and package section sizes.
  Include affected authored/cooked fixtures. Internal node IDs and graph layout
  may be regenerated; preserve identities referenced by dependent assets.
- [x] Qualify a small polymorphic owned-expression graph through Save/Load,
  DuplicateObject, edit-copy/Apply, deletion, Undo/Redo reference retention, and
  Cook stripping. Verify no deleted or editor-only descendant leaks into exports.
- [x] Finalize the concrete expression class list and value/reference-collection
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

- [x] Implement the mapped expression types and shared material/function collection.
- [x] Make parameter nodes the sole authored definition owners; move callee and
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
