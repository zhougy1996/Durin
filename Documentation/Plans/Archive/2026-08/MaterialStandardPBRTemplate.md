# Material Output and Graph Parameters Plan

Summary: Replace the expanded canonical default graph with an eight-input Material Output node and derive editable parameters from reachable graph nodes.

Last reviewed: 2026-08-26

Status: Archived
Completed: 2026-08-26

## Current Status

New `DMaterial` objects now retain the canonical definition catalog but author
zero expression nodes. The derived terminal is rendered as `Material Output`;
its eight typed inputs retain editable fallback literals and lower directly to
eight IR constants until an upstream expression is explicitly connected.

Source schema 2 upgrades legacy mandatory links without rewriting their 66
nodes, parameter GUIDs, normalized identity, or rendering. The old expanded
builder remains explicitly available for compatibility fixtures. Graph
commands cover fallback editing/reset, disconnect, parameter promotion, and
texture-chain creation with one validated transaction boundary.

Base-material panels, instance eligibility, orphan classification, and active
render bindings now use deterministic reachable dependency inspection.
Selected scalar/vector parameter nodes edit the same canonical value used by
Details, and ordinary value changes remain dynamic rather than recompiling.

## Goal

Make a newly created material open with one understandable terminal node and no
hidden standard expression graph, while preserving typed surface authoring,
atomic commands, material instances, compilation caching, cook behavior, and
last-known-good preview publication.

The completed workflow must provide:

- one terminal `Material Output` node with Base Color, Normal, Metallic,
  Roughness, Ambient Occlusion, Emissive, Opacity, and Opacity Mask inputs;
- useful defaults for every unconnected input, so a new material has zero
  expression nodes and remains immediately renderable;
- graph commands that connect, replace, and disconnect surface inputs, with a
  disconnect returning to the stored default rather than producing an invalid
  committed material;
- explicit Parameter, Texture Parameter, Texture Coordinate, and Texture Sample
  nodes only when the author needs dynamic or textured behavior;
- base-material and instance panels derived from parameters reachable from the
  Material Output inputs; and
- evidence that an empty/default material generates no unnecessary texture
  samples and that adding a branch compiles only that reachable branch.

## Scope

- Source-material surface-input representation, default literals, validation,
  serialization, normalized IR, generated Slang, identity, compile lifecycle,
  cache behavior, and cook output.
- The derived graph-space Material Output terminal, its eight typed input pins,
  inline defaults, connection/disconnection, framing, focus, diagnostics,
  clipboard boundaries, and rendered presentation.
- New-material initialization with zero expression nodes and one derived output
  terminal.
- Parameter promotion and creation workflows that place explicit graph nodes
  upstream of a selected Material Output or ordinary node input.
- A UI-independent reachable-parameter dependency snapshot shared by Engine,
  MaterialEditor, base-material Details, material instances, render binding,
  tests, and automation.
- Filtering of direct Parameter/TextureParameter dependencies and implicit
  texture-role UV/sampler dependencies.
- Legacy expanded-program compatibility, save/reload, Undo/Redo, compile
  failure/recovery, retained cache, rendered qualification, and lasting docs.

## Non-Goals

- A Standard PBR expression opcode with eight outputs. Surface data flows into
  the terminal; reversing that direction would obscure graph semantics.
- A general `Surface` value type or a reusable PBR function with eight inputs
  and one Surface output. That becomes useful only with material functions or
  multiple surface consumers.
- General material functions, nested graphs, arbitrary composite nodes,
  user-authored macros, reroute nodes, or subgraph expansion.
- Reproducing the old 66-node graph internally for new materials. Unconnected
  surface inputs lower directly to constants.
- Changing shading models, adding surface channels, changing normal encoding,
  or redesigning renderer GBuffer semantics.
- Automatically rewriting or dirtying existing expanded materials on load.
- Automatically deleting unused base values or instance overrides. Orphan
  instance overrides remain explicit cleanup candidates.
- Arbitrary user-defined parameter metadata in this plan. Canonical definitions
  remain the available value/metadata catalog; reachability controls active use
  and presentation.

## Design Decisions and Invariants

### Material Output is the semantic root

- `FMaterialSurfaceOutputs` remains the semantic graph root and is presented as
  a derived, non-persisted Material Output node. It is not added to
  `FMaterialProgram::Nodes` and cannot be duplicated, deleted, or dragged.
- Replace each mandatory raw link with a typed surface-input value containing
  an optional source link and a fallback literal. An invalid source GUID means
  unconnected, not malformed. The expected type is fixed by
  `EMaterialSurfaceOutput` rather than serialized redundantly.
- Input order is Base Color, Normal, Metallic, Roughness, Ambient Occlusion,
  Emissive, Opacity, and Opacity Mask. Types are Float3 for Base Color, Normal,
  and Emissive, and Float for the other five.
- Initial fallback values preserve the current visible defaults: Base Color
  `(0.95, 0.62, 0.22)`, tangent Normal `(0, 0, 1)`, Metallic `0`, Roughness
  `0.5`, Ambient Occlusion `1`, Emissive `(0, 0, 0)`, Opacity `1`, and Opacity
  Mask `1`.
- A surface input may be connected to any compatible graph output. Replace is
  atomic. Disconnect is always valid and restores the authored fallback. A
  connected input retains its fallback value for later disconnect and Undo.
- Material Output participates in pan, zoom, culling, Frame All, focus,
  diagnostics, link hit testing, and selection emphasis through the shared
  geometry authority, but its derived position and selection remain transient.

### Compilation follows only connected intent

- Validation checks the source link only when a surface input is connected. It
  validates fallback finiteness/type bounds independently and requires every
  connected source to exist and match the input type.
- Normalization traverses only connected links. For an unconnected surface
  input it emits a typed constant directly into normalized IR; it does not
  synthesize Parameter, texture, UV, sampler, or sample nodes.
- Generated Slang still assigns all eight `FMaterialSurface` fields, but an
  untouched new material contains only the eight default expressions and zero
  texture-sample expressions.
- The new default program identity intentionally differs from the old canonical
  parameterized graph because its dynamic capabilities and shader work differ.
  Identical new default materials still share one identity, retained compiled
  result, and single-flight request.
- Connecting, replacing, disconnecting, or editing a fallback changes
  normalized identity and submits one compile generation. Editing the value of
  a reachable Parameter or TextureParameter remains a dynamic render update and
  does not compile.
- Shader-affecting static properties retain their current compile and pipeline
  boundary. Last-known-good preview remains published until a new candidate is
  accepted.
- Cook consumes the accepted compiled representation as today. Source links,
  fallbacks, canvas state, and parameter-panel state do not enter cooked DMAT
  bytes except through the resulting compiled identity/program.

### Parameters are graph declarations

- Constant nodes contain compile-time literals. Parameter and TextureParameter
  nodes contain stable definition GUIDs and declare dynamic values visible to
  base materials and instances. A value is not active merely because a
  canonical definition exists.
- Add a UI-independent `InspectParameterDependencies` traversal beginning at
  connected Material Output inputs. It returns each reachable parameter once,
  with source node, parameter GUID, type, first-use order, and presentation
  metadata. Shared dependencies are de-duplicated deterministically.
- Reachable TextureParameter/TextureCoordinate/TextureSample roles contribute
  the associated UV channel, scale, offset, rotation, and sampler-state GUIDs
  actually used by generator helpers. Unconnected texture roles contribute
  nothing.
- Base-material Details shows only reachable dependencies. Selecting a
  parameter node shows the same value editor inline and in Details; both mutate
  the same nested `ParameterDefinitions` value through one transaction path.
- Material instances derive override eligibility from the resolved root base
  graph. A default material with no parameter nodes has no override rows.
- When topology makes an existing instance override unreachable, it moves to a
  collapsed `Orphan Overrides` group with source information and explicit
  Remove. It never continues to look active.
- Unreachable canonical definition values may remain serialized for source
  compatibility and Undo, but are hidden from ordinary Details and excluded
  from active local render bindings. Reconnecting the same ParameterId restores
  its preserved value and instance eligibility.

### Authoring workflows

- The default canvas contains only the derived Material Output node. Its eight
  input rows display the connected source or the formatted fallback value.
- Editing an unconnected fallback is one presentation-local widget gesture that
  commits one validated surface-default command on completion. Escape and
  lifecycle cancellation restore the authored value.
- `Promote to Parameter` on an unconnected surface input creates a compatible
  Parameter node, initializes its definition value from the fallback, connects
  it, places it one column upstream, and commits the program/presentation as one
  transaction.
- `Add Texture` is available only for surface roles with a canonical texture
  definition. It creates TextureParameter, TextureCoordinate, TextureSample,
  and required channel/normal-decode nodes explicitly, then connects the result
  as one bounded candidate transaction. The created graph is visible and
  editable; no hidden texture branch exists.
- Ordinary palette creation remains explicit. It never chooses an unrelated
  compatible source. Surface reconnection retains the old link until a valid
  replacement drop succeeds.
- Constant fallback, promoted Parameter, and texture workflows are distinct in
  labels and tooltips so authors understand compile-time versus dynamic edits.

### Compatibility and lifecycle

- Existing expanded canonical and custom programs remain valid and load with
  their current connected surface links. They are not collapsed, simplified,
  or marked dirty automatically.
- A source schema upgrade converts each legacy mandatory surface link into a
  connected typed input and initializes its fallback to the canonical value.
  It does not change nodes, parameter GUIDs, normalized identity, or rendering.
- New construction uses zero expression nodes and eight unconnected defaults.
  Tests and fixtures that require the old standard graph call an explicitly
  named legacy/expanded builder rather than assuming all `DMaterial` objects
  contain it.
- Graph, fallback, promotion, and texture-branch commands are candidate
  validated. Rejection leaves program, surface defaults, presentation,
  parameter values, dirty state, transaction history, compile generation, and
  accepted preview unchanged.
- Active gestures cancel on Escape, document switch, close, discard, deletion,
  stale owner, module unload, or shutdown. Document-local transient state never
  leaks across materials or instances.

## Current Foundations and Gaps

### Landed foundations

- `FMaterialProgram` already provides a bounded typed DAG, eight surface roots,
  deterministic reachability, normalized IR, generated Slang, program identity,
  compile cache/single-flight, last-known-good publication, and cooked output.
- The existing canonical builder precisely records the legacy 66-node
  parameter-by-texture behavior and remains useful as a compatibility fixture.
- `FMaterialGraphOperations` owns candidate-validated commands, detached views,
  transactions, layout, clipboard, and semantic mutation.
- The graph-space Surface Outputs proxy already pans, zooms, frames, focuses,
  and renders eight typed target lanes; it can become Material Output without a
  second persisted node.
- `FMaterialParameterPanelModel` already resolves base values, instance
  overrides, reflected edits, continuous-gesture coalescing, and orphan removal.

### Gaps selected by this plan

- Surface outputs are mandatory links; there is no valid unconnected/default
  state, so new material construction must manufacture source nodes.
- `DMaterial` always creates the expanded canonical graph and requests its full
  reachable shader work even when the author has connected nothing.
- The output proxy labels eight targets but does not own editable fallback
  values or parameter-promotion workflows.
- Parameter panels enumerate all 56 definitions instead of reachable graph
  declarations, leaving ineffective controls visible after topology changes.
- All canonical definitions are uploaded into the local render layer even when
  the accepted graph cannot reference them.
- Tests use the default `DMaterial` as an implicit canonical-graph fixture, so
  creation semantics and legacy compatibility evidence are not separated.

## Implementation Stages

### Stage 0: Lock default-surface and dependency baselines

- [x] Capture fixtures for a newly constructed material, legacy expanded
  canonical graph, constants-only output, one connected scalar parameter, one
  connected texture role, shared parameters, disconnected parameters, orphan
  instance overrides, compile failure/recovery, and the 256-node bound.
- [x] Record current authored node count, reachable IR node count, generated
  texture-sample count, source hash, program identity, compile/cache outcome,
  active render bindings, cook identity, and rendered output for the legacy
  default.
- [x] Lock the eight input types and fallback values, literal encoding,
  connection flag representation, schema upgrade, disconnect behavior, and
  diagnostics for invalid fallbacks and connected links.
- [x] Lock parameter dependency ordering, implicit texture-role dependencies,
  Details/instance/orphan grouping, promotion placement, texture-branch shape,
  Undo/Redo boundaries, and lifecycle cancellation.
- [x] Record visual equivalence tolerances for the new default constants versus
  the intended legacy default appearance, plus gates proving zero default
  texture samples and no unexpected compile on dynamic parameter edits.

#### Acceptance Gate

- Old and new fixtures are reproducible, default values and dataflow direction
  are closed, migration is non-destructive, and later stages have exact IR,
  shader, UI, instance, cook, and lifecycle gates.

### Stage 1: Make surface inputs optionally connected

- [x] Introduce the versioned typed surface-input/default representation and
  deterministic upgrade from legacy mandatory links without changing connected
  legacy program semantics.
- [x] Update validation, normalized identity, canonical encoding, clipboard
  boundaries, diagnostics, and command requests for connected versus fallback
  surface inputs.
- [x] Lower unconnected inputs directly to typed IR constants and traverse only
  connected upstream nodes; retain the existing generated Surface assignment
  and shader entry points.
- [x] Allow surface disconnect as a valid atomic command that restores the
  retained fallback; preserve replace, rejection, Undo/Redo, compile generation,
  and last-known-good behavior.
- [x] Add schema, validation, normalization, generated-source, identity, cache,
  cook, and maximum-bound tests for all-unconnected, partially connected,
  fully connected legacy, malformed, and repeated programs.

#### Acceptance Gate

- Zero-node material programs are valid and render all eight defaults, connected
  legacy graphs normalize unchanged, only connected branches are reachable,
  and disconnecting a surface input is safe and atomic.

### Stage 2: Make Material Output the default canvas workflow

- [x] Change new `DMaterial` construction to zero expression nodes and the eight
  locked fallback values; retain definitions, static properties, compile
  lifecycle, render publication, and source/cooked loading contracts.
- [x] Rename and render the derived Surface Outputs proxy as one terminal
  Material Output node with eight input pins, connected-source/default rows,
  tooltips, inline fallback controls, semantic zoom, focus, and diagnostics.
- [x] Add explicit connect, replace, disconnect-to-default, reset-default,
  `Promote to Parameter`, and `Add Texture` commands with complete candidate
  validation and one transaction/compile boundary per completed action.
- [x] Keep ordinary creation, linking, reconnection, selection, framing,
  clipboard, layout, and keyboard/context alternatives correct when the graph
  contains no expression nodes or only partial branches.
- [x] Separate new-default fixtures from the explicitly named legacy expanded
  builder and update affected callers without silently changing legacy assets.
- [x] Add command/controller and rendered overview/editing tests for empty,
  partially connected, promoted, textured, disconnected, failed, Undo/Redo,
  multi-document, and stale-owner workflows.

#### Acceptance Gate

- A new material opens with only Material Output, its defaults are editable and
  renderable, upstream nodes appear only through explicit authoring, and common
  promotion/texture workflows are atomic and discoverable.

### Stage 3: Drive parameter panels and bindings from the graph

- [x] Add deterministic reachable dependency inspection for direct Parameter
  and TextureParameter nodes, shared use, and implicit UV/sampler dependencies;
  expose stable source and presentation data without mutable storage.
- [x] Filter base-material Details to reachable parameters and make node-inline
  and Details controls edit the same definition value with coalesced transactions
  and dynamic render updates.
- [x] Filter instance override eligibility through the resolved root graph;
  preserve inherited/local source labels and move unreachable local overrides
  into a collapsed removable orphan group.
- [x] Publish only reachable definitions as active local render bindings while
  preserving unused serialized values; verify reconnect restores the old value
  and makes it eligible for instance override again.
- [x] Prove fallback/constant edits compile, reachable parameter/texture value
  edits do not compile, and topology/parameter identity/static-property changes
  retain current compile/cache/last-known-good boundaries.
- [x] Add panel-model, reflected transaction, graph command, instance,
  render-proxy, save/reload, Undo/Redo, and multi-document tests for used,
  shared, unused, reconnected, implicit, inherited, overridden, and orphan
  parameters.

#### Acceptance Gate

- Every visible base or instance parameter corresponds to a reachable graph
  declaration, every declaration has one value authority, unused controls and
  bindings disappear, and ordinary dynamic values update without compilation.

### Stage 4: Qualify, document, and complete

- [x] Run the smallest registered native targets covering Engine material
  schema/compiler/cache/cook, MaterialEditor graph commands/canvas/panels,
  instances, render proxies, serialization, and lifecycle.
- [x] Complete the affected runtime/editor build and rendered checks for empty,
  constants-only, parameterized, textured, partial, legacy expanded, dense, and
  maximum-size graphs at overview and editing zoom, plus editor startup smoke.
- [x] Verify new creation, legacy load, save/reload, relocation, deletion,
  dirty/save/discard, Undo/Redo, compile failure/recovery, retained cache,
  single-flight, last-known-good, cook, and multi-document/instance isolation.
- [x] Record exact authored/IR node counts, generated sample counts, identities,
  cache outcomes, compile generations, active bindings, rendered comparison,
  configuration, timing method, and pass/fail evidence in this plan.
- [x] Update [Material Graph Operations](../../../Editor/Architecture/MaterialGraphOperations.md)
  and [Material System](../../../Runtime/Rendering/MaterialSystem.md) with lasting
  surface-default, reachability, compilation, parameter, instance, migration,
  and cook contracts; add a user guide only if promotion needs durable help.
- [x] Run changed-document and all-plan validation, close only evidenced gates,
  and mark this plan completed after no required validation remains.

#### Acceptance Gate

- New materials contain no manufactured expression graph or default texture
  samples, Material Output owns the correct eight-input flow, panels and
  bindings reflect reachable declarations, legacy assets remain compatible,
  and all selected build/test/render/cook/documentation gates pass.

## Completion Evidence

Configuration: Windows MSVC x64, `Win64-Debug-DurinEditor`, Debug, Vulkan 1.5
material target. Timings below are wall-clock DurinDevTool results from the
agent checkout on 2026-08-26; compiler microseconds are the native test's own
steady-clock measurements.

| Fixture | Authored nodes | Reachable IR | Texture samples | Generated bytes | Source hash | Program identity | Active reflected bindings |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| New default | 0 | 8 | 0 | 8,971 | `abb5c71ce7645ae99e6d5de87d629e5b` | `48f0be88bbf58e1846683f2eb32c0f59` | 12 across 3 stages |
| Legacy expanded | 66 | 66 | 8 | 10,825 | `259384f9fd8a4e6446cb7bf79fc5568f` | `01b1233ffa2d1b12d6bac55dff557cd3` | 44 across 3 stages |

The legacy cold compile produced 134,580 SPIR-V bytes; the measured run spent
799 us normalizing, 437 us generating, 450,852 us in the forced cold compile,
and 1,240 us on the retained warm compile. Cook identity is the accepted
program identity, so source/cooked parity is locked by the existing cook and
publication tests. Fallback/topology edits advance one compile generation;
reachable parameter/texture values only dirty dynamic bindings. Existing
single-flight, retained-hit, failure/recovery, last-known-good, serialization,
relocation/deletion, Undo/Redo, instance-isolation, and maximum-bound cases all
passed in the selected native targets.

Validation results:

- `MaterialTests`: 99/99 passed, including schema, normalization, compiler,
  cache/cook, graph commands/canvas/panels, instances, render proxies,
  serialization, and lifecycle.
- `StaticMeshTests`: 74/74 passed; `MaterialThumbnailTests`: 6/6 passed;
  `MaterialVulkanTests`: 1/1 passed with the rendered roughness sweep and
  resolved-material comparisons.
- Full `all` editor build passed in 2.84 s after the final code change.
- Hidden-window editor startup and normal shutdown after 30 ticks passed in
  2.65 s using `Sandbox/Sandbox.dproject`.
- Lasting contracts were updated in Material Graph Operations and Material
  System; changed-document and all-plan validators passed at completion.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Surface schema | Eight fixed input types/defaults, connected flag/link encoding, legacy upgrade, round trip, invalid fallback/link diagnostics |
| Normalization | Zero-node validity, constant injection, connected-only reachability, deterministic IR/identity, partial branch behavior, maximum bounds |
| Generated shader | Eight Surface assignments, zero texture samples for new default, exact samples for connected texture branches, no hidden legacy expansion |
| Default creation | Zero expression nodes, one derived terminal, intended default appearance, stable static properties, shared identity/cache behavior |
| Graph commands | Connect/replace/disconnect/default edit/reset/promotion/texture creation, atomic rejection, one transaction, Undo/Redo, compile generation |
| Canvas | Eight named input pins, inline fallbacks, typed targets, semantic zoom, framing, focus, diagnostics, empty/partial/legacy rendered evidence |
| Dependencies | Direct and shared parameters, implicit UV/sampler roles, stable first-use order, unused exclusion, reconnect restoration |
| Base panel | Same value through node and Details, coalesced Undo, dynamic update, unused controls hidden, no duplicate surface configuration |
| Instances | Root graph filtering, inherited/local values, override eligibility, orphan grouping/removal, parent changes, isolation |
| Compile/cache | Fallback/topology edits compile; value/texture edits do not; retained-hit/single-flight reuse; failure and last-known-good recovery |
| Runtime/cook | Active binding set, render-proxy values/textures, legacy/new save-reload, source/cooked parity, relocation/deletion safety |
| Documentation | Lasting graph and material contracts updated; changed-document and all-plan validation pass |

## Definition of Done

- A newly created material has zero expression nodes and displays one terminal
  Material Output node with eight correctly typed inputs and useful defaults.
- Unconnected inputs lower directly to constants; only explicitly connected
  graph branches enter IR and generated shader source, and the new default has
  zero texture samples.
- Parameters and textures are explicit reachable graph nodes. Base Details and
  instance overrides are views over those declarations rather than an
  independent fixed Surface Parameters system.
- Surface fallback editing, connection, disconnection, parameter promotion, and
  texture creation are candidate-validated, deterministic, and atomic with
  predictable Undo and compile behavior.
- Existing expanded/custom materials load and compile without silent rewrite or
  dirtying, and legacy/source/cooked behavior has explicit regression evidence.
- New-default, partial, parameter, texture, instance, cache, failure/recovery,
  cook, rendered, and maximum-bound validation passes.
- Lasting contracts are updated, exact evidence is recorded, every checklist is
  closed, plan validation passes, and the plan is marked Completed.

## Deferred Follow-ups

- A reusable Standard PBR function with eight inputs and one first-class Surface
  output after material functions or multiple surface consumers justify a
  Surface value type.
- General material functions, nested graphs, arbitrary composites, user macros,
  reroute nodes, and a shared graph-expansion framework.
- User-created parameter definitions, promotion metadata authoring, custom
  groups, and removing the remaining canonical-definition catalog constraint.
- Automated simplification of legacy 66-node standard graphs. Existing assets
  remain explicit until a separately qualified semantic matcher is justified.
- Static switches or compile-time optional texture branches; this plan already
  removes unconnected branches structurally.

## Related Documentation

- [Material Graph Operations](../../../Editor/Architecture/MaterialGraphOperations.md)
- [Material System](../../../Runtime/Rendering/MaterialSystem.md)
- [Material Graph Editor Usability Plan](MaterialGraphEditorUsability.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Agent Documentation Workflow](../../../Agents/Documentation.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramTypes.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramTypes.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramCompiler.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramCompiler.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramGenerator.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/Material.h`
- `Engine/Source/Runtime/Engine/Private/Materials/Material.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`
- `Engine/Source/Editor/MaterialEditor/Public/MaterialGraphOperations.h`
- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphOperations.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphCanvas.h`
- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphCanvas.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialParameterPanelModel.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialGraphOperationsTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialSchemaAndEditingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialCompileLifecycleTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialInstanceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderProxyTests.cpp`
