# Material Graph Owned Parameters Plan

Summary: Make graph nodes own material parameters, retire the separate declaration table, and rebuild retained assets instead of maintaining historical material compatibility.

Last reviewed: 2026-09-14

Status: Archived
Completed: 2026-09-14

## Current Status

Completed on 2026-09-14. All stages and acceptance gates passed. Base material
parameters are owned by graph nodes; separate authored declaration storage,
mutation APIs, hidden parameter defaults, clipboard rebinding and historical
material conversion paths are removed. Instances and compiler/Cook consumers use
validated derived metadata, including graph-stripped runtime defaults.

Validation evidence (Win64 Debug, NVIDIA GeForce RTX 3090):

- Registry-selected `test all`: 87/87 native targets passed, including import,
  serialization, transactions, clipboard, instance inheritance and Vulkan material
  and scene-import coverage. Receipt:
  `Build/.agent-state/logs/20260914-002841-433183-32960-ctest.log`.
- Final MaterialTests: 192/192 cases in 19 suites passed after the final UI and
  load-validation edits. Receipt:
  `Build/.agent-state/logs/20260914-004329-850175-29352-MaterialTests.log`.
- Missing ownership markers reject both parent and instance without publication.
  Malformed saves preserve the original package; a valid package envelope with
  dangling graph links is rejected before residency. Focused receipts:
  `Build/.agent-state/logs/20260914-001852-353848-35088-StaticMeshTests.log` and
  `Build/.agent-state/logs/20260914-003542-295818-7476-StaticMeshTests.log`.
- Actual production ImGui draw output covers empty materials, node metadata,
  shared outputs, texture/UV extraction, instance overrides and 900-pixel windows.
  Inspected PNGs are retained in `Build/MaterialGraphOwnedParameters/UI`.
  These captures exercise real widgets; preview imagery is validated separately
  through Vulkan. Parameter names remain visible as primary node titles.
- Vulkan material tests load the reconstructed DefaultMaterial and ImportedSurface
  from disk and compare independent texture/UV, normal, instance and masked
  rendering. Captures are retained in `Build/MaterialGraphOwnedParameters/GPU`.
  Explicit directional-shadow qualification, including masked coverage and motion,
  passed: `Build/.agent-state/logs/20260914-003449-811222-38872-ctest.log`.
- Both projects' Cook outputs were rebuilt with explicit roots for DefaultMaterial
  and ImportedSurface. Sandbox also includes its dynamically spawned GrayboxPawn
  mesh. Receipts: `Build/.agent-state/logs/20260914-004554-855445-27620-DurinAssetTool.log`
  and `Build/.agent-state/logs/20260914-003855-635797-32076-DurinAssetTool.log`.
  Outputs remain in `Build/MaterialGraphOwnedParameters/CookedSandbox` and
  `Build/MaterialGraphOwnedParameters/CookedRoadWeaver`.
- Both retained default scenes ran for 120 frames in standalone Game using their
  cooked output, with no warnings, missing assets, load failures or error
  fallbacks. Integration exposed and fixed Game's missing project-root module
  admission before level loading. Receipts:
  `Build/.agent-state/logs/20260914-004614-413900-1928-DurinGame.log` (Sandbox) and
  `Build/.agent-state/logs/20260914-004542-708301-37768-DurinGame.log` (RoadWeaver).
  Scene/package reference audits and material pixel comparisons complement these
  runtime checks; retained scene bytes were not changed.
- Final `all` builds passed for the workspace's Engine, Sandbox and RoadWeaver in
  both Editor and Game. Receipts:
  `Build/.agent-state/logs/20260914-004519-058367-35784-cmake.log` and
  `Build/.agent-state/logs/20260914-004531-172260-40848-cmake.log`.
  The source/test audit across all three projects found no remaining authored
  table mutators or transition adapters.

The nine retained Engine material/function recipes have been rebuilt with program
schema 7/function schema 3, including a 58-node ImportedSurface with 48 explicit
owners. The exact 12-file lighter set below was removed after a fresh identity and
reference audit matched the Stage 0 baseline and confirmed no external inbound
references. Original files and SHA-256 hashes are retained under
`Build/MaterialGraphOwnedParameters/original-content/manifest.json`. Post-rebuild
identity and recipe reports for Sandbox and RoadWeaver are under the same work
folder; both recipe inventories match their standard function implementations.

The user explicitly permits removing the imported lighter and its exclusively
owned asset content if that simplifies implementation, then rebuilding remaining
materials. Historical material compatibility is not an acceptance requirement.
This permission applies to that imported asset set, not arbitrary shared engine
or project content. The current execution request includes the staged cleanup
and reconstruction work, subject to the reference inventory below.

This plan supersedes the separate declaration ownership, inline parameter-table
references and 11-visible-element target of the completed
[Material Graph Authoring Simplification](MaterialGraphAuthoringSimplification.md)
plan as its stages land. Coordinate compiler, instance and Cook changes with the
active [Material Instance Shader Variants](MaterialInstanceShaderVariants.md)
plan; preserve its intended current-model publication and runtime behavior.

## Goal

The graph is the complete base-material authoring surface. Ordinary nodes own
literal values and graph connections. Exposed parameters are visible nodes that
own their definitions. No separately editable parameter declaration table or
base-material Parameters management window remains. Material instances retain
an override list derived from the parent graph because they have no graph.

A new material contains only Material Output. Users add constants and operations,
convert a value to a parameter when instance exposure is needed, and edit the
parameter through selected-node Details.

## Selected Data Model

| Element | Ownership and behavior |
| --- | --- |
| Constant / ordinary input | Owns a typed literal or a graph source link; never looks up a root declaration by parameter ID. |
| Numeric parameter node | Owns a stable parameter GUID, unique name, type, default and supported group/order/range metadata; its output can fan out. |
| Texture parameter node | Owns a texture parameter definition, resource default and sampler/fallback policy. A sample-parameter node may also sample and expose channels. |
| Material Output | Owns property connections and literal fallbacks. Parameter-driven properties connect to parameter nodes. Surface remains an aggregate value type, not a parallel declaration table. |
| Function interface | Owns typed named ports. Functions consume passed values/resources and do not declare root instance parameters. |
| Instance | Overrides parameters by stable parameter GUID in the new model. Rename and movement do not change that identity. |
| Compiler / inspector projection | Derives a deterministic read-only parameter schema from graph owners; cannot be independently authored or mutated. |

Node GUID and parameter GUID remain distinct: links/presentation use node
identity and instances use parameter identity. Exactly one node owns each
parameter GUID in a root graph. Sharing uses graph fan-out, not multiple
independently editable definitions with the same name or GUID.

Multiple samplers sharing a texture parameter connect to its resource output;
they can retain different UV/sampling operations. Do not merge sampling work
solely because the resource identity matches. Stage 0 selects the concrete
resource/channel outputs of combined texture sample parameter nodes.

Node-owned definitions participate in serialization, validation, reference
collection, transactions and clipboard accounting. Remove root table mutation
APIs after callers move to graph commands. A derived read API may remain for
instances and compilation. Cooked parameter metadata/layout must still resolve
instance overrides when editor graphs are stripped; compiled descriptors are
not the retired authored declaration table.

## Simple Input Semantics

Ordinary disconnected inputs, call-site fallback values and local UV settings
store literals or their supported typed resource/interface defaults. Exposing a
value creates a visible parameter node and an explicit graph connection.
Disconnecting restores the retained non-parameter default. There is no hidden
parameter binding behind a connected input and no second retained parameter link
solely to preserve historical disconnect behavior.

Rebuild affected assets to match this simpler rule rather than introducing
compatibility-only fallback structures. Parameterized UV fields use explicit
parameter nodes and supported graph connections; ordinary UV settings remain
inline literals. Function signature defaults retain legitimate interface
semantics, including existing supported dependencies between function inputs;
removing root parameter references must not accidentally remove those features.

Preserve disconnected parameter nodes authored in the new model until the user
deletes them. Distinguish the full graph-owned schema from reachable parameters
used by compilation and instance controls. Report stale instance overrides rather
than silently applying them to unrelated parameters. Deletion and type changes
use explicit commands and undo support.

Duplication creates fresh node and parameter GUIDs with unique names. Sharing
uses existing outputs. Same-root paste can retain external source links;
foreign-root paste remaps owners and links together, without implicit name-based
rebinding. Undo/Redo restores a complete graph and its derived schema atomically.

## Asset Replacement and Compatibility Policy

Prefer one new authored schema and one runtime path. Do not add historical readers,
automatic old-graph conversion, dual table/node ownership or long-lived adapters
merely to retain the existing lighter assets. Reject unsupported old material
schemas with a clear rebuild diagnostic. Do not globally remove unrelated package
or serialization compatibility code.

Stage 0 inventories the imported lighter by actual paths and references. Remove
its imported models, material instances, textures and scene entries only when
confirmed exclusive to that asset set. A naming prefix alone is insufficient.
Shared engine functions, ImportedSurface, shared textures, default materials and
assets referenced by other projects require retention or explicit reconstruction.
Record exact removal/rebuild paths and inbound references before execution.
Use the repository asset workflow where appropriate; preserve repository recovery
through version control or backups for any untracked asset content.

Reconstruct remaining engine/project materials and any retained instances in the
new schema. Old parameter GUIDs, instance package bytes, historical clipboard
payloads and the old imported graph node count need not be preserved. Retained
instances must be deliberately rebound/rebuilt against the new parents, and scene
or mesh material references must resolve after replacement. Never leave silent
fallback rendering as evidence of successful reconstruction.

Update import recipes and shipped functions so newly imported assets work directly
with the new model. Regenerate affected derived/cooked data through normal build
paths and advance schema/cache identity where needed. Remove temporary transition
code before completion rather than keeping both models indefinitely.

## Scope

Primary owners: Engine material types, validation, compilation and serialization;
MaterialEditor graph commands, Details, clipboard and layout; AssetForgeBuiltins
and AssetMaintenance import/content producers. Follow direct consumers in
AssetTools and DurinEd for creation, property editing and transactions.

Search changed shared API symbols across source and native-test roots of every
project in `Durin.dworkspace` (currently Engine, Sandbox and RoadWeaver). Complete
an all-project build after the final Engine API migration.

Do not change shading models, remove Surface aggregate operations, make functions
own root parameters, or fold unrelated preview/docking redesign into this work.
Compatibility is intentionally reduced; correctness of new materials, instances,
functions, import, save/load and cooked runtime use remains mandatory.

## Implementation Stages

Follow [build guidance](../../../Agents/BuildAndRun.md) and
[test guidance](../../../Agents/Testing.md) for target selection and execution. Each
stage depends on the preceding stage and must leave the repository buildable.
Record observed evidence before closing checklists.

### Stage 0: Inventory assets and freeze the new contract

- [x] Inventory declaration producers/consumers, schemas, compiler/cache identity,
  instance lookup, transactions, clipboard and cooked readers across all projects.
- [x] Record exact lighter removal candidates, exclusive/shared dependencies and
  inbound references; list every remaining material/instance to reconstruct.
- [x] Select node payloads, resource outputs, parameterized UV connections, literal
  disconnect defaults, schema/cache version changes and unsupported-schema errors.
- [x] Resolve overlap with Material Instance Shader Variants before implementation.

Gate: a concrete removal/rebuild inventory and fully specified current-model
contract; no requirement to retain historical root parameter bindings.

#### Stage 0 execution evidence (2026-09-13)

Read-only `asset identity-audit --project Sandbox/Sandbox.dproject` and
`asset identity-audit --project RoadWeaver/RoadWeaver.dproject`, through
`DevTool.bat`, inspected 27 and 15 packages respectively. Every package and
reference inspection returned Ready. Local full receipts are
`Build/MaterialGraphOwnedParameters/Sandbox-before.json` and
`Build/MaterialGraphOwnedParameters/RoadWeaver-before.json`. Both projects mount
the same 13 Engine packages; Engine.dproject is not a standalone asset-tool
project. The following inventory records the actionable facts independently of
those disposable receipts.

Baseline `.\DevTool.bat test MaterialTests` passed 190/190 cases in 19 suites
on Win64-Debug-DurinEditor (93.58 seconds including the incremental build).
Receipt: `Build/.agent-state/logs/20260913-210922-361088-25116-MaterialTests.log`;
build receipt: `Build/.agent-state/logs/20260913-210921-656126-25116-cmake.log`.
This validates the pre-refactor baseline only, not any Stage 1–4 gate.
Changed-document validation and `git diff --check` passed for the inventory.

All lighter candidates below are tracked. Paths are relative to
`Sandbox/Content/Models/VintageLighter/`; the matching virtual prefix is
`/Game/Models/VintageLighter/`. A listed companion is part of that exact package's
candidate set, not permission to remove other files in the directory.

| Candidate package (`.dasset`) | Companion | Actual inbound references |
| --- | --- | --- |
| `Meshes/vintage_lighter_1k` | None | None in either project's package audit |
| `Materials/vintage_lighter` | None | Mesh `MaterialSlots[0].DefaultMaterial` |
| `Materials/vintage_lighter_alpha` | None | Mesh `MaterialSlots[1].DefaultMaterial` |
| `Textures/vintage_lighter_diff_BaseColor` | Same stem `.dbulk` | Both instances, override 4 |
| `Textures/vintage_lighter_diff_Opacity` | None | Alpha instance, override 5 |
| `Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic` | Same stem `.dbulk` | Opaque override 5, alpha override 6 |
| `Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness` | Same stem `.dbulk` | Opaque override 6, alpha override 7 |
| `Textures/vintage_lighter_nor_gl_Normal` | Same stem `.dbulk` | Opaque override 7, alpha override 8 |

Both instances have `Parent` references to
`/Engine/Materials/ImportedSurface.ImportedSurface`. This outbound dependency
does not make the Engine material or its functions disposable. No lighter path
references were found in the source/native-test roots of Engine, Sandbox or
RoadWeaver. The saved Sandbox `Levels/GrayboxStage15.dasset` references Box,
DefaultStudioCube, PureSky and the two cloud volumes; it has no lighter entry.
RoadWeaver's `Levels/L_RoadNet.dasset` and `NewRoadNet.dasset` have no lighter
references. Recheck fingerprints, mounted references and untracked content
immediately before Stage 3 removal. Preserve all unrelated scene objects.

Selected Stage 3 path: remove the eight lighter packages and four exact
companions above. No retained material instances remain in the audited content.
Reconstruct `Engine/Content/Materials/DefaultMaterial.dasset` and
`Engine/Content/Materials/ImportedSurface.dasset` at their existing paths. Rebuild
all seven packages under `Engine/Content/Materials/Functions/`: `UVTransform`,
`SampleNormal`, `SampleORM`, `StandardPBR`, `StandardPBR_ORM`,
`ImportedSurfaceValues`, and `DecodeImportedNormalRG` (each `.dasset`). The
StandardPBR functions reference SampleNormal; StandardPBR_ORM also references
SampleORM. ImportedSurface references ImportedSurfaceValues and
DecodeImportedNormalRG. Retain every Engine model, renderer asset, Sandbox asset
outside the exact lighter set, and every RoadWeaver asset. Validate both retained
scenes after reconstruction even where their stored reference lists are unchanged.

The shared-symbol audit covered all three descriptors' source and native-test
roots. Production ownership is concentrated in these boundaries:

| Boundary | Migration sites and obligations |
| --- | --- |
| Authored storage | `Material.h/.cpp`, `MaterialTypes.h`, `MaterialParameterTypes.cpp`, `MaterialProgramTypes.h/.cpp` and `MaterialProgramSignatures.cpp`: remove root storage/mutators and hidden input/UV IDs |
| Runtime projection | `MaterialInterface.cpp`, `MaterialInstance.cpp`, `MaterialRenderRepresentation.cpp`, `MaterialCompiledLayout.cpp`: full owner schema versus reachable layout, GUID/type override resolution |
| Compiler | `MaterialCompileLifecycle.cpp`, `MaterialProgramCompiler.cpp`, `MaterialFunctionExpansion.cpp`, `MaterialProgramGenerator.cpp`: detached snapshots, owner validation, resource/channel lowering and identity |
| Cook | `MaterialCook.cpp`, `MaterialCookedProgram.cpp`, compiled layout and root metadata: retain graph-stripped parameter resolution without an authored table |
| Editor | MaterialEditor `MaterialGraphOperations`, `MaterialGraphInputCommands/Details`, `MaterialGraphClipboard`, `MaterialGraphTransactions`, edit sessions, document/catalog/canvas/creation menu and texture previews |
| Session/UI | `MaterialEditingSession.cpp`, `MaterialParameterPanelModel.cpp`, `MMaterialEditor.cpp`, `MaterialAssetCreation.cpp`: Apply/Discard, reflected routes, instance rows and removal of the base management window |
| Producers | AssetForgeBuiltins `ImportedSurfaceMaterial.cpp`, standard function producers, `SceneDirectImport.cpp`, `DurinAssetTool/Private/AssetToolMain.cpp`: new recipes and deliberate reconstruction |
| Direct consumers | AssetTools/DurinEd have no direct old declaration symbols; retain their generic reflected-property, reference-collection and transaction contracts. AssetMaintenance owns canonical resave, not the material recipe. Sandbox/RoadWeaver have no direct old declaration symbols |
| Tests | Engine native material schema/editing, graph, functions, editing session, instance, compiler lifecycle, rendering/proxy/representation, panel, mesh and scene import fixtures; query the registry for Cook, clipboard, transaction and GPU selections |

#### Frozen implementation contract

- Add a reflected `FMaterialParameterDefinition Parameter` payload to parameter
  owner nodes. Its `Id` is the instance identity; `Node.Id` is independently
  generated. Remove the authored node's standalone parameter-reference ID.
  Non-owner nodes require an empty parameter payload; functions reject owners.
  Numeric owner result type must match its definition. Texture owners require
  Texture. Validate all owners, including disconnected ones, before publication;
  reject duplicate GUIDs/names, invalid metadata/resources and conflicting links.
- Keep `GetParameterDefinitions()` only as a const derived view. Rebuild the
  bounded full projection in GUID order at validated mutation/load boundaries;
  never reconstruct the graph from it. No public operation accepts an independently
  authored declaration array. Compiler/instance reachable views remain separate.
  Default edits update the owner and projection together without treating dynamic
  values or presentation as shader identity. Retain cooked-only generated
  descriptors and defaults so graph-stripped roots resolve GUID/type overrides.
- Preserve sample output slots 0 RGBA, 1 RGB, 2 R, 3 G, 4 B, 5 A, 6 RG. Add slot
  7 Texture2D only to TextureSampleParameter2D; requesting it lowers to the owned
  resource without evaluating sampling or UV work. TextureParameter output 0
  remains Texture2D. Sampling consumers keep their own UV operations; do not
  deduplicate different samples solely by parameter GUID. Sampler/fallback policy
  remains the existing typed resource-value policy, including instance overrides.
- Ordinary input/call-site defaults support None or Literal only; remove their
  parameter kind and ID. UV settings retain literal channel/scale/offset/rotation.
  Parameterized UV connects owners to TextureCoordinates inputs 0 Float channel,
  1 Float2 scale, 2 Float2 offset and 3 Float rotation, then connects its Float2
  output to the sample UV input. Disconnection restores literals. Function
  signature defaults retain Numeric/Texture/Surface/Input/UV0 semantics and their
  legitimate interface InputId dependencies.
- Compiler snapshots must contain no live texture/object ownership after adding
  reflected node payloads. Detach graph owners to GUID/type-only compiler values
  on the owning thread before enqueueing; never copy object-bearing authored
  defaults into worker snapshots or identity bytes. Cook serializes validated
  current generated metadata, never repopulates authored graph ownership.
- Rename preserves parameter and node IDs. Promotion preserves the node ID and
  creates a fresh parameter ID; duplicate/paste creates both fresh IDs and unique
  names, remapping copied internal links together. Same-root external links can
  remain; foreign-root external links reject. No name-based definition reuse.
  Transactions/clipboard retain owner textures and account for nested strings and
  arrays. Restore program, calls, presentation and projections atomically.
- Advance authored program 6 to 7, function schema 2 to 3, clipboard 5 to 6,
  compiler identity 4 to 5 and envelope 7 to 8. Advance DMAT 5 to 6 for generated
  parameter metadata. Keep package DAST v9, presentation schemas and pass contract
  unchanged. IR 4/generator 5 can remain if resource-output lowering uses the
  existing IR operations. Replace the old declaration marker with a new authored
  contract marker whose absent/unsupported stored value cannot default to current.
  Reject old material/function schemas with an explicit rebuild diagnostic before
  compilation, accepted generation publication or save. Remove material-only
  PostLoad v4/v5 upgrades; retain unrelated package/property compatibility.
- Preserve Material Instance Shader Variants' complete per-owner publication,
  exact effective configuration keys, shared compile jobs, pending retention,
  current-failure ErrorMaterial and compiler-free Cooked execution. This work
  changes parameter ownership and rebuilds affected content; it does not reinstate
  parent-only programs or claim that plan's remaining Win64 Game, reimport and
  GPU gates have passed. Keep its per-field property override migration intact.

### Stage 1: Implement graph-owned parameter storage

The shared API removal also requires the mechanical migration of editor/import
callers and native fixtures in this stage; delaying those callers until Stages
2–3 would leave the repository unbuildable or require the forbidden dual-model
adapter. Stages 2–3 retain their complete workflow/UI and persisted-content gates.

- [x] Add node-owned payloads, validation and deterministic derived schemas.
- [x] Move compiler snapshots and instance lookup to the derived schema; retain
  required cooked descriptors after graph stripping.
- [x] Introduce the new authored version with explicit rejection of unsupported
  old material data; remove separate-table authority and transition adapters.
- [x] Test new-schema round trips, duplicate identities, invalid links, reachability,
  resource retention and rejection without partial publication.

Gate: one authored ownership model; save/load, instance lookup and compilation
work on new-schema fixtures without historical conversion machinery.

### Stage 2: Move all authoring to graph commands

- [x] Implement creation, rename, defaults, conversion to parameter, fan-out,
  deletion/type-change diagnostics and undoable resource assignment.
- [x] Update texture/UV and function-call editing, extraction/inlining, clipboard,
  Apply/Discard, dirty checkpoints and reference collection.
- [x] Remove the base-material Parameters management window and its layout slot;
  retain instance override controls and complete selected-node metadata editing.
- [x] Verify one action produces one transaction, disconnected inputs restore
  supported non-parameter defaults, and selection never displaces the canvas.
- [x] Capture actual UI output for empty material, parameter editing, shared use,
  texture/UV editing and instance workflows, including narrow windows.

Gate: complete base authoring through nodes and Details without a declaration
manager or hidden root parameter binding path.

### Stage 3: Remove disposable content and rebuild retained assets

- [x] Execute the inventoried lighter cleanup if needed; resolve scene/mesh inbound
  references and verify shared assets are retained.
- [x] Update import recipes, shipped functions, preview wrappers, maintenance tools
  and all project consumers to produce graph-owned parameters.
- [x] Rebuild retained materials/instances and regenerate affected derived/cooked
  artifacts; update mesh/scene material references deliberately.
- [x] Remove obsolete API callers and old material-only compatibility code made
  unnecessary by reconstruction; do not retain dual-model mutation routes.

Gate: retained content loads in the new schema with no dangling dependencies,
new imports work, and old disposable assets are not required by any shipped path.

### Stage 4: Validate the new model and finish integration

- [x] Pass registry-selected graph, serialization, instance inheritance, import,
  clipboard, transaction and graph-stripped Cook/load coverage.
- [x] Validate rebuilt default/imported materials and retained scenes for intended
  appearance, valid textures, normal/UV behavior and absence of error fallbacks.
- [x] Run affected preview/forward and masked/shadow GPU coverage; unavailable
  execution remains an outstanding gate rather than a pass.
- [x] Complete an `all` build for all workspace projects after the final shared
  API change and verify no current declaration-table mutation path remains.
- [x] Update [Material System](../../../Runtime/Rendering/MaterialSystem.md),
  [Material Graph Operations](../../../Editor/Architecture/MaterialGraphOperations.md)
  and [Canonical Resave](../../../Editor/Guides/CanonicalResave.md), then record completion.

Gate: new-schema authoring, instances, import, save/load and cooked runtime work;
retained assets and references are rebuilt and verified; compatibility-only
scaffolding is gone. Historical lighter parity and old asset GUID retention are
explicitly not completion gates.
