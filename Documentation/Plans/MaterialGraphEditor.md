# Material Graph Editor Plan

Summary: Build the command-driven material graph operations workflow, canvas, diagnostics, preview integration, and structured automation surface.

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

Material System milestone 7 is complete. MaterialEditor now exposes one
UI-independent, candidate-validated command surface for the existing bounded
`FMaterialProgram`, a typed ImGui canvas, reflected editor-only presentation,
atomic transactions, versioned clipboard operations, deterministic layout, and
diagnostic navigation. Base-material documents preserve independent canvas
state and handle asset relocation, deletion, save/discard, compile failure, and
last-known-good preview behavior without changing the M5 program or M6 compiler
lifecycle architecture.

The lasting ownership and interaction contracts now live in
[Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md).

## Goal

Let artists and structured editor automation inspect and edit the same bounded
typed material program through one validated command surface, with a responsive
node canvas, deterministic Undo/Redo and copy/paste, actionable compile
diagnostics, and live last-known-good preview behavior.

## Scope

- A UI-independent MaterialEditor graph inspection and editing API over
  `FMaterialProgram`, stable node GUIDs, typed pins, and surface outputs.
- Atomic commands for node creation/removal, literal and parameter selection,
  connections, disconnections, surface-output assignment, node movement,
  duplication, copy/paste, and automatic layout.
- Reflected, bounded, package-persisted node presentation data owned by the
  base `DMaterial` and kept separate from program semantics.
- One canvas per open base-material document with pan, zoom, selection,
  marquee, node dragging, typed pin linking, context creation, deletion,
  framing, and keyboard operations.
- Existing canonical material-parameter definitions exposed as authorable
  parameter and texture nodes; edits that require a new program schema remain
  outside this plan.
- Compile status and bounded diagnostics with navigation to nodes, inputs, and
  surface outputs; preview continues to show last-known-good output during
  pending or failed edits.
- Per-document Undo/Redo, dirty/save/discard, save/reload, multi-document,
  relocation, deletion, and failure/recovery behavior required by the M7 exit
  gate.
- Focused command-level, serialization, editor-model, workflow, and rendered
  qualification.

## Non-Goals

- Turning the graph, nodes, or pins into `DObject` subobjects, or introducing a
  second polymorphic authoring graph alongside `FMaterialProgram`.
- Extending the M5 value domain, opcode library, implicit-conversion policy,
  surface ABI, parameter-declaration schema, compiler IR, generated source, or
  pass integration.
- Allowing arbitrary shader source, user-defined node classes, Blueprint/plugin
  node registration, material functions, nested graphs, loops, or cycles.
- Replacing the M6 compile service, RenderCore shader cache, last-known-good
  policy, Cook payload, or runtime loading contract.
- Persisting per-user canvas pan, zoom, hover, selection, transient link drag,
  compile status, or diagnostics in the material asset.
- Graph authoring on material instances; instances continue to edit inherited
  parameter overrides and never own a program.
- Runtime dynamic material instances or profiling-selected batching and reuse;
  Material System milestone 8 owns that work.
- General-purpose graph-framework extraction before the material workflow has
  supplied a second proven consumer.

## Design Decisions and Invariants

### Authored model and ownership

- `DMaterial` remains the only object and package authority. The existing
  reflected `FMaterialProgram`, `FMaterialProgramNode`, links, stable node GUIDs,
  and surface-output record remain the canonical authored semantic graph.
- Nodes and pins remain bounded reflected values, not `DObject` instances.
  Canvas views, selection adapters, and property models are non-authoritative
  editor state and never enter compilation or package identity.
- `DMaterial` gains a reflected graph-presentation value containing at most one
  integral canvas position per live node GUID. Missing entries receive
  deterministic layout; duplicate or dangling entries are pruned without
  invalidating an otherwise valid material program.
- Node positions are shared authored presentation and round-trip with the
  package. Viewport pan/zoom and selection are per-document session state.
  Presentation never enters validation semantics, normalized IR, shader-map
  identity, compile snapshots, derived data, or the cooked DMAT payload.
- Material instances resolve the root base program exactly as today. Opening an
  instance keeps the existing parameter workflow and offers navigation to its
  base material rather than an editable inherited canvas.

### Command and transaction boundary

- MaterialEditor exposes one UI-independent graph service with structured
  requests/results and no ImGui types. Canvas code, tests, and automation call
  the same inspection and mutation operations.
- A semantic command edits a candidate program, validates it against the
  material's parameter definitions and M5 bounds, and commits through the
  `DMaterial` mutation boundary only on success. Failed commands leave authored,
  dirty, compile-generation, and presentation state unchanged.
- Commands address nodes by stable GUID and pins by the existing explicit input
  index/output index or surface-output enum. Screen coordinates and display
  labels are never semantic identifiers.
- Each user-visible action is one document transaction. Continuous node moves
  coalesce from pointer-down to pointer-up; paste, duplicate, delete, automatic
  layout, and multi-node edits each undo atomically. Undo/Redo restores semantic
  and presentation state together and requests compilation only when semantic
  state changed.
- Semantic commits use the existing M6 generation-safe compile request path.
  Presentation-only commands mark the package dirty but do not compile.

### Graph operations

- The node catalog is derived from the closed M5 opcode/type rules. Creation
  either chooses an unambiguous result type or requires the caller to supply one;
  it does not infer an undocumented conversion.
- Connection admission rejects missing endpoints, occupied inputs, cycles,
  depth/count overflow, and type mismatch before mutation. Replacing an occupied
  input is an explicit command policy, not a drag-side effect.
- Removing nodes also removes inbound links, affected surface-output links, and
  presentation entries in the same transaction; the resulting candidate must
  still satisfy the required surface-output contract or the command is rejected.
- Copy/paste uses a versioned bounded value payload. Paste generates fresh node
  GUIDs, remaps internal links, drops external links, preserves parameter GUIDs
  and relative positions, and passes ordinary validation before commit.
- Automatic layout is deterministic for identical semantic graphs and writes
  presentation only. It must handle the maximum 256-node/depth-64 program
  without changing node GUIDs, node order, links, outputs, or normalized identity.

### Canvas, diagnostics, and preview

- The first canvas is MaterialEditor-owned and implemented on the existing
  ImGui drawing/input stack. It does not add a third-party graph dependency or
  prematurely establish a repository-wide graph framework.
- Rendering and hit testing consume a read-only graph view produced by the
  service. Direct writes from widgets into `FMaterialProgram` are forbidden.
- Diagnostics retain the M5 `Program`, `Node`, `Input`, and `SurfaceOutput`
  locations. Activating a located diagnostic selects and frames its target;
  program-wide or stale locations remain visible without fabricating a node.
- Pending and failed authored states remain visually distinct from the accepted
  preview. Canvas errors never replace the M6 last-known-good result, and stale
  compile results never alter selection or diagnostic navigation.
- Graph interaction remains responsive while compilation runs and while another
  material document is active. Document-local view/selection state cannot leak
  across resource IDs.

## Current Foundations and Gaps

### Landed foundations

- Bounded version-1 `FMaterialProgram` with stable node and parameter GUIDs,
  typed links, eight required surface outputs, deterministic validation and
  normalization, and bounded source locations.
- `DMaterial::SetMaterialProgram()` validation and the M6 generation-safe
  request/publication lifecycle with last-known-good behavior.
- MaterialEditor document dirty/save/discard and Undo/Redo infrastructure,
  compile status, diagnostic list, previews, thumbnails, parameter controls,
  Content Browser drag/drop, and multi-document hosting.
- Existing ImGui rendering, input, styling, and workspace layout facilities.

### Gaps selected by this plan

- No non-widget graph inspection/mutation service or graph-specific transaction.
- No persisted node positions or deterministic layout policy.
- No canvas rendering, selection, navigation, typed link interaction, or node
  property model.
- No versioned copy/paste payload or command-level structured automation seam.
- Diagnostics are listed but cannot select/frame their graph locations.
- Save/reload, failure recovery, relocation/deletion, and multi-document graph
  workflows lack end-to-end qualification.

## Locked Stage 0 Contract

### Runtime and editor boundaries

| Concern | Authority and rule |
| --- | --- |
| Semantic graph | `DMaterial::Program` remains the only authored program. A graph command copies it, edits the copy, calls `ValidateMaterialProgram()` with the live canonical parameter definitions, and commits only through `DMaterial::SetMaterialProgram()`. |
| Compile lifecycle | A successful semantic commit advances the existing authored revision and submits exactly one generation through the M6 service. Rejection and no-op commands submit none. Undo/Redo of a semantic snapshot uses the same mutation boundary and therefore submits one generation. |
| Presentation | `DMaterial::GraphPresentation` is an `EditorOnly` reflected value with schema version 1 and at most 256 `{NodeId, X, Y}` records. Coordinates are signed integral graph-space units bounded to +/-1,048,576. Presentation commits mark the package dirty and never invalidate render data or request compilation. |
| Sanitization and migration | Load, inspection, and presentation commit retain the first bounded record for each live node GUID, discard invalid, duplicate, dangling, and out-of-range records, and deterministically lay out missing live nodes. A package without the field retains the empty constructor default and receives layout on inspection; sanitization alone never rejects an otherwise valid program. |
| Identity and Cook | Presentation is excluded from `FMaterialProgram`, normalization, shader-map identity, compile snapshots, DDC keys, and the DMAT Cook payload. Duplication and editor package exchange copy it through ordinary reflection; Cook strips it with the existing `EditorOnly` property policy. |
| Transactions | One command records one reflected snapshot containing `Program` and `GraphPresentation`. A node drag opens on pointer-down, replaces its proposed presentation snapshot while active, and commits or cancels on pointer-up/Escape. Failed or stale-owner commands create no history. |

### Inspection and command vocabulary

The UI-independent MaterialEditor service exposes deterministic catalog, node,
pin, surface-output, parameter, and selection-payload views. Stable node GUIDs,
input/output indices, parameter GUIDs, and `EMaterialSurfaceOutput` values are
the only semantic addresses.

| Operation | Request | Atomic result and compile rule |
| --- | --- | --- |
| Create | opcode, explicit result type when ambiguous, graph position | Generated node GUID; semantic plus presentation commit; compile once. |
| Remove | bounded node-GUID set | Removes nodes, inbound links, affected outputs, and positions only if the final program validates; compile once. |
| Edit node | node GUID plus literal, parameter GUID, swizzle, or display-name value | Validated replacement; compile once when semantic data changed. |
| Connect/disconnect | source node/output plus destination node/input, or surface output; explicit replace policy | Missing endpoints, occupied inputs, cycles, depth/count overflow, and type mismatch reject before commit; successful semantic change compiles once. |
| Move | bounded node-GUID/position set | Presentation-only commit; no compile; continuous requests may coalesce. |
| Delete/duplicate | bounded selected node-GUID set and optional placement anchor | One transaction; duplication allocates fresh GUIDs and preserves only remapped internal links. |
| Copy/cut/paste | versioned selection payload, placement anchor | Copy is read-only. Cut is copy plus validated delete. Paste remaps every node GUID, drops external links, preserves parameter GUIDs and relative offsets, and validates before one commit. |
| Layout | optional bounded node-GUID set | Deterministic presentation-only placement; no compile and no semantic reordering or identity change. |

Every mutation returns a bounded structured result with a status (`Succeeded`,
`NoChange`, `Rejected`, or `StaleOwner`), affected and generated GUIDs, and up
to the M5 diagnostic count/message limits. Widgets display this result but do
not infer success from labels or mutate reflected storage directly.

### Copy/paste and layout

- Selection payload schema 1 contains at most 256 complete nodes, their
  presentation offsets relative to the selection's minimum corner, and links
  whose endpoints are both selected. It contains no material pointer, package
  path, surface-output assignment, compile state, or viewport state and is
  rejected for unknown versions, duplicate/invalid GUIDs, invalid enums,
  excessive counts/bytes, or malformed internal links.
- Paste generates fresh nonzero GUIDs in payload order, remaps internal links,
  preserves parameter GUIDs, applies one graph-space translation, and uses the
  ordinary candidate validator. No partial node or presentation state survives
  rejection.
- Automatic layout is a stable topological column layout: dependencies precede
  consumers, ties use node GUID order, columns use the longest distance to a
  sink, and rows use GUID order. Fixed integral spacing and surface-output
  placement make identical semantic graphs produce identical positions in
  linear time within the 256-node/depth-64 bounds.

## Implementation Stages

### Stage 0: Lock graph authoring and presentation contracts

- [x] Inventory the M5 opcode/type/input rules, `DMaterial` mutation hooks, M6
  compile transitions, document transactions, and asset lifecycle notifications
  used by this plan.
- [x] Specify the reflected presentation record, independent sanitization,
  package migration/default-layout behavior, bounds, and Cook exclusion.
- [x] Specify the command request/result vocabulary, stable addressing,
  validation/commit sequence, transaction coalescing, and compile-trigger rules.
- [x] Lock the versioned copy/paste payload and deterministic layout contract.

#### Acceptance Gate

- One reviewed contract maps every planned canvas or automation action to the
  same command surface and preserves the M5/M6 ownership boundaries.

### Stage 1: Land presentation state and graph command core

- [x] Add bounded reflected node-presentation storage to `DMaterial`, including
  legacy defaults, duplication, import/exchange, save/reload, and sanitization.
- [x] Implement read-only graph/node/pin/catalog views derived from the existing
  program and parameter definitions.
- [x] Implement candidate-based semantic and presentation commands with
  structured failures and exact dirty/compile behavior.
- [x] Integrate snapshot transactions with MaterialEditor's existing per-document
  Undo/Redo model, including drag coalescing and stale-object rejection.
- [x] Add command tests for success, rejection without mutation, maximum bounds,
  Undo/Redo, and semantic-versus-presentation compile generation.

#### Acceptance Gate

- Tests can build and edit representative valid programs without constructing a
  widget, every invalid request is atomic, and presentation round-trips without
  affecting program identity or Cook data.

### Stage 2: Implement the base canvas and typed linking

- [x] Add a document-local canvas controller and read-only view model with pan,
  zoom, framing, grid, selection, marquee, and maximum-graph culling/hit testing.
- [x] Draw surface outputs, typed nodes/pins, links, selection, hover, invalid
  targets, and pending/failed compile state using existing editor style tokens.
- [x] Route node creation, movement, deletion, connection, disconnection, and
  literal/parameter editing exclusively through the command service.
- [x] Provide keyboard/context-menu operations and deterministic automatic
  layout for legacy materials with no positions.

#### Acceptance Gate

- A user can create, connect, edit, navigate, and undo a representative material
  entirely on the canvas; invalid links are rejected before mutation and the
  256-node graph remains interactive under the plan-owned workload.

### Stage 3: Complete authoring operations and structured automation

- [x] Implement multi-selection duplicate, delete, copy, cut, and versioned paste
  with GUID remapping, relative placement, bounds checks, and one-step Undo/Redo.
- [x] Expose deterministic graph enumeration and the complete command vocabulary
  through the UI-independent MaterialEditor API used by focused automation.
- [x] Add stable operation results containing affected/generated node GUIDs and
  bounded validation diagnostics; no caller must scrape canvas labels or screen
  positions to determine the outcome.
- [x] Add command-sequence fixtures proving the canvas and structured callers
  produce the same persisted program and normalized identity.

#### Acceptance Gate

- Representative materials can be authored, copied, repaired, and laid out via
  structured commands with no canvas input, then opened and continued in the UI
  without semantic or identity drift.

### Stage 4: Integrate diagnostics, preview, and document lifecycle

- [x] Make diagnostic activation select/frame node, input, or surface-output
  locations and degrade deterministically for program-wide or stale locations.
- [x] Surface authored-dirty, pending, accepted, failed, canceled, superseded,
  and last-known-good states without blocking canvas input or misrepresenting the
  rendered preview.
- [x] Preserve independent canvas/controller state across base materials,
  instances, tab activation, close/reopen, save/discard, and multi-document use.
- [x] Integrate available asset relocation/deletion notifications needed by the
  M7 exit gate without adding a MaterialEditor-local asset catalog mirror.
- [x] Qualify compile failure/recovery, dependency reload, preview and thumbnail
  refresh, and deletion while a compile or edit gesture is active.

#### Acceptance Gate

- Located diagnostics navigate to the authored cause, failed edits preserve the
  accepted preview, recovery publishes the newest valid generation, and document
  lifecycle operations neither lose authored graph state nor retain stale owners.

### Stage 5: Qualify and document the landed workflow

- [x] Run the selected native command/schema/compiler tests, MaterialEditor
  workflow tests, rendered preview checks, Cook regression, and maximum-graph
  interaction workload under the repository test workflow.
- [x] Record exact targets, configurations, workloads, timings, and any justified
  exceptions in this plan.
- [x] Move lasting authoring, presentation, command, and interaction contracts to
  the owning Runtime and Editor documentation.
- [x] Update the Material System roadmap M7 status and route any measured runtime
  scalability evidence to M8 without implementing it here.

#### Acceptance Gate

- The validation matrix passes, lasting contracts are authoritative outside the
  plan, and Material System milestone 7 satisfies its exit gate.

## Completion Record

Completed on 2026-08-26 in `Win64-Debug-DurinEditor`.

| Command or workload | Result |
| --- | --- |
| `.\DevTool.bat test MaterialTests` | 93/93 passed; 7.254 seconds of test execution, 7.88 seconds total. Covers the schema, sanitizer, opcode catalog and pins, atomic commands, transactions, copy/paste, normalized identity, diagnostic navigation, compiler lifecycle, Cook round-trip, and a deterministic 256-node layout asserted below one second. |
| `.\DevTool.bat test EditorShellTests` | 49/49 passed; 95 milliseconds of test execution, 0.66 seconds total. Includes open and deferred document resource-ID remapping. |
| `.\DevTool.bat test MaterialThumbnailTests` | 6/6 passed; 835 milliseconds of test execution, 4.97 seconds total. |
| `.\DevTool.bat test MaterialVulkanTests` | 1/1 passed; 6.405 seconds of test execution, 11.47 seconds total. Qualifies material rendering correctness; the run is not authoritative GPU-performance evidence because external GPU exclusivity was not established. |
| `.\DevTool.bat build` | The full `all` target linked successfully in 14.11 seconds. |
| Hidden editor startup smoke | `DurinEditor.exe` remained alive for eight seconds after startup and was then intentionally force-stopped. |
| `.\DevTool.bat doc validate --scope changed` | Passed for all changed documents. |
| `.\DevTool.bat doc plan validate --scope all` | Passed for the complete plan set. |
| `.\DevTool.bat doc roadmap validate --scope all` | Passed for the complete roadmap set. |

The rendered qualification uses the existing Vulkan material test, thumbnail
tests, full editor link, and startup smoke rather than introducing a brittle
canvas pixel baseline. Canvas interaction semantics and maximum-graph behavior
are qualified through the shared command/controller tests; runtime update
scalability remains deliberately evidence-gated in roadmap milestone 8.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Schema/persistence | Presentation defaults and sanitization, save/reload, duplication/import, stable node GUIDs, legacy packages, and exclusion from normalized/program/Cook identity |
| Commands | Every operation succeeds or rejects atomically; typed links, cycle/depth/count limits, affected IDs, surface-output validity, and semantic/presentation compile triggers are exact |
| Transactions | Add/edit/connect/move/delete/layout/paste Undo/Redo, gesture coalescing, dirty/save/discard, and stale-owner rejection |
| Copy/paste/layout | Payload version/bounds, fresh node IDs, internal remapping, external-link policy, parameter GUID preservation, relative placement, deterministic maximum-graph layout |
| Canvas | Pan/zoom/frame, selection/marquee, creation, linking, property editing, keyboard/context actions, clipping/hit testing, and responsive maximum-graph interaction |
| Compiler/preview | Generation-safe requests, diagnostic location navigation, pending/failure/recovery, last-known-good preview, reload, thumbnail refresh, and no presentation-only compile |
| Lifecycle | Base versus instance behavior, multi-document isolation, save/reload, relocation, deletion, close during edit/compile, module unload, and shutdown |
| Automation | Headless enumeration and mutation use the same service as canvas actions and produce equivalent persisted programs and normalized identities |

## Definition of Done

- A base material can be authored through a typed node canvas using every M5
  opcode and surface output supported by version 1.
- The same workflows are available through stable structured inspection and
  command APIs without coordinate-based UI automation.
- Graph and presentation state survive save/reload, duplication, Undo/Redo,
  relocation, deletion, compile failure/recovery, and multi-document editing.
- Invalid operations never partially mutate the asset; presentation changes do
  not compile or affect shader identity; semantic changes preserve M6 stale
  rejection and last-known-good behavior.
- Selected validation passes with evidence, lasting contracts are updated, and
  the Material System roadmap marks M7 complete.

## Deferred Follow-ups

- User-defined parameters beyond the canonical M5 declaration schema.
- Material functions, nested graphs, reusable subgraphs, and an open plugin or
  scripting node type system.
- A shared editor graph framework, selected only after another concrete consumer
  demonstrates common requirements.
- Runtime dynamic material instances and measured update scalability in M8.
- MaterialEditor lifecycle polish not required by the M7 exit gate remains M9.

## Related Documentation

- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
- [Material Compile Lifecycle and Derived Data](MaterialCompileLifecycleAndDerivedData.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/Material.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramTypes.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramTypes.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialCompileLifecycle.h`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Source/Editor/DurinEd/Public/Editor/WorkspaceRootWindow.h`
- `Engine/Tests/Native/EngineTests/Private/Materials`
