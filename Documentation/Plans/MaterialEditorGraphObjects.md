# Material Editor Graph Objects Plan

Summary: Introduce editor-owned material graph and node objects and map the existing asset, pin, and transaction contracts against UE.

Last reviewed: 2026-11-19

Status: Active
Completed:

## Current Status

Stages 0 through 2 are complete. Material and function canvases read their session graphs and submit drag connections through the schema. Stage 3 covers the remaining callers. `FMaterialGraphDocument` holds a weak asset owner and a schema reference, exposes graph inspection, and dispatches editing commands. The material and material-function schemas already govern expression creation and connection compatibility. The existing behavior is documented in [graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md), [canvas interaction](../Editor/Architecture/Materials/MaterialGraphCanvas.md), and [editor lifecycle](../Editor/Architecture/Materials/MaterialEditorLifecycle.md).

## Goal

Give each open material or material-function editing session a transient `DMaterialGraph` that owns editor nodes through a common node base. Expression nodes resolve an asset expression by stable ID. The material root specializes the same base and binds the existing output expression. A function graph exposes its function ports and has no material root. Expression inputs and function port IDs remain authoritative for links. The graph can rebuild from the current editing owner after undo, redo, reload, or owner replacement. Keep graph objects in MaterialEditor and reuse the existing stateless schema policies. Stage 0 records the current asset format and graph infrastructure. Later stages may revise either when the selected UE design requires it.

The UE references for this division of responsibility are [UMaterialGraph](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UMaterialGraph), [UMaterialGraphNode](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UMaterialGraphNode), and [UMaterialGraphSchema](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UMaterialGraphSchema).

## Public API and extension points

Follow UE 5.8 method names and call direction for the operations below. Use Durin ownership, stable pin addresses, and diagnostic result types in signatures. Stage 0 records each Durin signature, its UE counterpart, and the reason for any semantic difference.

| Owner | Public operations | Extension contract |
| --- | --- | --- |
| Graph | `GetSchema`, `GetMaterialOrFunction`, `AddExpression`, `RebuildGraph`, `LinkGraphNodesFromMaterial`, `LinkMaterialExpressionsFromGraph` | The graph resolves the editing owner, constructs the appropriate node for each expression, and publishes completed changes to observers. Adding an existing expression during rebuild must leave the asset clean. |
| Node and pin | `AllocateDefaultPins`, `CreateInputPins`, `CreateOutputPins`, `ReconstructNode` | A node owns its transient pins. Specializations define pin layout. Reconstruction restores links by stable endpoint identity and preserves missing-port diagnostics. The canvas consumes a common pin interface. |
| Schema | `CanCreateConnection`, `TryCreateConnection`, `BreakPinLinks`, `GetGraphContextActions` | The query returns an allow, reject, or replace decision with a reason. Gesture previews and execution share the rule. Schema actions delegate mutations to the graph transaction path. Material and function policies remain reusable internally. |

Keep the common graph, node, pin, and schema contracts independent of material expression types wherever graph operations are generic. Material-specific operations live on the material types. Introduce only contracts exercised by these editors. A subsequent shared editor graph layer can take ownership of generic operations while preserving these material-facing entry points.

Asset load, undo, and reload synchronize from expressions into graph pins. A successful user connection synchronizes the edited graph endpoints back to expression inputs in one transaction. Recheck both endpoints before writing. On failure restore the derived graph from the editing owner, discard the proposed change, and publish no successful edit. Emit the graph change notification after synchronization so details and preview observe consistent state.

## Stage 0 design record

### Scope of the baseline

The following contracts are candidates for UE alignment. Passing cases describe the starting revision. A later stage may adopt different behavior and update the corresponding acceptance check. Record the UE reference, affected callers or assets, and the intended change in the owning stage.

| Current Durin contract | UE alignment decision available to later stages |
| --- | --- |
| MaterialEditor builds graph views over an explicit material or function owner. | Introduce common editor graph, node, pin and schema bases where shared operations justify them. Review reflected type and ownership choices for the schema. |
| Links carry expression IDs, indices and function port GUIDs. The material output is itself an authored expression. | Choose editor pin identity, reconstruction and root representation independently of the current serialized representation. Specify old-asset handling if the on-disk representation changes. |
| Commands write expression inputs through the document and return structured results. | Move graph edit authority and synchronization into graph and schema methods. Review result types, replacement policy, validation timing and bulk link breaking against UE. |
| Transient working copies, package history, Apply, undo publication and preview invalidation have different material and function paths. | Review each lifecycle boundary separately against UE before changing transaction ownership, publication timing or graph notifications. |
| Presentation, read-model caches and action menus depend on current command and change-set APIs. | Reassign pin construction, context actions, graph notifications and canvas refresh to the corresponding UE-style owners. |

Stage 1–3 signatures and parity checks below are initial implementation choices. Stage 0 establishes no new runtime or serialization contract.

### Owners and callers at 79ed1a2e0

The source asset identifies the toolkit. Each material or function editing session will strongly own one transient graph for its current working copy. Nodes belong to that graph. Bind the graph weakly to the working asset and resolve expressions by ID. Release subscriptions and cancel gestures before replacing the draft or closing the session. A deferred callback must verify the session and working owner still match. Headless asset creation and tests can still edit an explicit owner without opening a toolkit.

| Path | Material | Material function |
| --- | --- | --- |
| Open and Apply | `FMaterialEditingSession` owns the transient working material and package. Apply waits for successful compilation and checks the source revision before publishing. | `FMaterialFunctionEditingSession` owns the transient function, package, canvas, and separate preview. Apply validates the dependency closure and publishes synchronously. |
| Undo and redo | Replay changes on the working copy, including after Apply. The source changes on a subsequent Apply. | Successful replay republishes each affected open function session. Unrelated dirty drafts stay isolated. |
| Save, replacement, and close | Save applies and persists. Clean reload replaces the draft. Retirement forgets working-package history and cancels compilation. | Save applies then persists. Discard rebuilds from the in-memory source. Clean reload replaces the draft. Close retires preview, history subscriptions, and working package. |

Across the three projects, 31 files reference `FMaterialGraphDocument`. The workspace scan found document references in Engine's MaterialEditor and native tests. Sandbox and RoadWeaver had no direct references. The following groups cover the command surface of [FMaterialGraphDocument](../../Engine/Source/Editor/MaterialEditor/Public/MaterialGraphDocument.h).

| Current entry points and consumers | Current transaction and refresh behavior |
| --- | --- |
| Toolkit, canvas, read model, menus and picker use owner/schema access, inspection, creation queries, layout calculation, and copy. | Queries leave history and dirtiness unchanged. The read model subscribes to owner and dependency changes. Reset, additions, removals, interface or input changes trigger full inspection. Content changes inspect affected nodes. Position updates patch presentation in place. |
| Canvas, picker, asset creation, clipboard and tests create, replace, remove, cut, paste, duplicate, connect, disconnect, insert calls and edit ports. | Preserve explicit owner and transaction mode. Topology edits resolve the transactor, stage expression mutations and commit to the working owner. Compound commands retain their existing undo unit, generated IDs, affected IDs and diagnostics. Owner change notifications feed the read model. |
| Details, input commands and parameter tools change defaults, swizzles, constants, parameters, configuration and surface inputs. | Keep typed command results and existing command-specific transactions, including configuration replay. Preserve current content versus interface invalidation and preview scheduling. |
| Canvas move sessions, output move, layout and numeric editing sessions | Preserve begin/apply/commit/cancel grouping. Position replay restores only touched positions. Layout-only updates retain cached topology. |

### Proposed Durin signatures and compatibility differences

These are interface designs for Stages 1 and 2. Parameter names in the table denote the current editing graph or one of its stable endpoints. The reference API is UE 5.8.

| UE method | Proposed Durin signature | Existing implementation and difference |
| --- | --- | --- |
| Graph schema and owner access | `GetSchema() const -> const FMaterialGraphSchema&`, `GetMaterialOrFunction() const -> DObject*` | Delegate the existing document accessors to the session graph. The weak working owner may expire. |
| `AddExpression` | `AddExpression(DMaterialExpression&) -> DMaterialGraphNode*` | Attach an existing owned expression during projection, reusing its ID. The current `CreateExpression` authors asset data and has distinct transaction semantics. Route the material-output expression to the root specialization during rebuild. |
| `RebuildGraph`, `LinkGraphNodesFromMaterial` | `RebuildGraph() -> void`, `LinkGraphNodesFromMaterial() -> void` | Split current `Inspect` into node construction and endpoint resolution. Both operate on the draft and leave package and history untouched. |
| `LinkMaterialExpressionsFromGraph` | `LinkMaterialExpressionsFromGraph(span<const FMaterialGraphPinAddress> changedInputs, ETransactionMode) -> FMaterialGraphCommandResult` | Commit changed inputs together via existing edit-session machinery. Preserve bindings for unrelated or missing ports and reject stale endpoints atomically. |
| `AllocateDefaultPins`, `CreateInputPins`, `CreateOutputPins`, `ReconstructNode` | Same four names, each `() -> void` on the node | Replace materialized pin views with graph-owned nodes carrying transient pins. Current addresses use kind, node GUID, index and optional port GUID. Function inputs normalize the index to zero and bind by GUID. Restore surviving connections by those keys. |
| `CanCreateConnection` | `CanCreateConnection(const FMaterialGraphPinAddress& a, const FMaterialGraphPinAddress& b) const -> FMaterialGraphConnectionResponse` | Proposed response carries allow, reject or replace and a reason. Normalize either drag direction, then share the current schema and document admission checks with execution. Preserve editable incomplete calls and the existing compiler diagnostics for incompatible inferred widths. |
| `TryCreateConnection`, `BreakPinLinks` | `TryCreateConnection(DMaterialGraph&, FMaterialGraphPinAddress a, FMaterialGraphPinAddress b, bool replaceExisting, ETransactionMode) -> FMaterialGraphCommandResult` and `BreakPinLinks(DMaterialGraph&, FMaterialGraphPinAddress, ETransactionMode) -> FMaterialGraphCommandResult`. | Adapt current target-first `Connect` and input-only `Disconnect`. Breaking an output resolves all consumers and commits once. Preserve explicit replacement admission, no-change and stale-owner results. |
| `GetGraphContextActions` | `GetGraphContextActions(const DMaterialGraph&, optional<FMaterialGraphPinAddress>) const -> vector<FMaterialGraphCreationAction>` | Move action enumeration behind the schema. Reuse catalog, function and collection discovery, and current creation policy. Search ranking and popup state stay in the canvas. |

Source entry points for parity checks are the [pin address and result types](../../Engine/Source/Editor/MaterialEditor/Public/MaterialGraphOperations.h), [connection command](../../Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphDocument.cpp), [edit commit](../../Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphEditSession.cpp), [pin projection](../../Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphCatalog.cpp), and [read model refresh](../../Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphReadModel.cpp).

### Compatibility fixtures

IDs generated during a test are compared to the same fixture's captured or returned IDs. Fixed function port keys below use the test helper's GUID prefix `{0xa04759c1,1,2,N}`.

| Native case | Observed behavior for migration comparison |
| --- | --- |
| `FMaterialFunctionEditingTests.UnfinishedCallsRemainEditableAndCompileAfterConnecting` | Required Float Amount (401) at (0,0) feeds Result (402) at (10,20). Wrapper call at (30,40) recovers its generated ID on redo. Material call at (100,200) feeds Roughness from output 402. Offline manual `CompileEdits` fails with Amount absent and succeeds after scalar 0.6 connects to call input 401. Disconnecting the wrapper binding invalidates the closure and undo restores it. |
| `FMaterialGraphInteractionTests.DuplicateMixedOutputSelectionPreservesCopiedNodePositions` | Root at (0,0) retains its ID. Constants at (500,300) and (700,200) duplicate to (540,340) and (740,240) through both document and canvas paths. |
| `FMaterialGraphPersistenceTests.TextureCoordinatesRetainChannelLinkAndDefaultAcrossPackageRoundTrip` | Channel links to the scalar expression ID and retains default 2. Expression snapshots match after duplicate and package reload. Reloaded package is clean. |
| `FMaterialFunctionPersistenceTests.WorkspaceSavesAndReloadsFunctionsAcrossOpenDocuments` | Function call at (0,300) survives replacement of its callee. Applied output name “Saved Surface” survives save/discard. Pasting at (300,300) targets the reloaded function. |
| `FMaterialFunctionTests.CallBindingsUseGuidAndDiagnoseRemovedRetypedOrRequiredPorts` | Call ID `{4,3,2,1}` binds input 1 to `{9,8,7,6}` and output 3. Reorder and rename pass. Retyping the bound output diagnoses the call and port IDs. Removing it fails validation. The current catalog projects retained stale bindings as “Missing input” or “Missing output” with the bound GUID and `bMissing=true`. Stage 1 must additionally compare that editor projection. |
| `FMaterialFunctionTests.ExpansionPreservesIndependentInputsMultipleOutputsAndEquivalentKeys` | Two call sites feed Metallic and Roughness from Sum and AmbientOcclusion from Original. Reordering expressions and renaming Sum preserve canonical bytes and identity. Slang generation succeeds. |

### Execution receipt

Revision `79ed1a2e0`, Windows MSVC x64 Debug, `Win64-Debug-DurinEditor`. The native inputs were unchanged during capture. The host DevTool ran the following selections sequentially.

| Selection | Result |
| --- | --- |
| `MaterialGraphEditingTests` | 106/106 passed, including the unfinished-call edit and offline compile case. |
| `MaterialFunctionTests` | 30/30 passed, including GUID diagnostics and canonical IR/Slang generation. |
| `MaterialEditorInteractionTests` filtered to the mixed-output duplicate case above | 1/1 passed. |
| `MaterialEditingPersistenceTests` filtered to the two persistence cases above and `FMaterialFunctionDocumentTests.UndoRedoAfterApplyRepublishTheWorkingGraph` | Build stopped before execution. MSVC LNK2019 from MaterialEditingSession.cpp.obj references the unresolved FMaterialGraphCanvas destructor, followed by LNK1120. The persistence and function publication expectations above are source-derived. The first comparable passing runtime receipt requires resolving this existing test-target link failure. |

## Implementation Stages

### Stage 0: Establish the compatibility baseline

- [x] Trace the editing owner through open, Apply, undo, and close for both asset types. Inventory document callers across workspace projects and map each command to its transaction and refresh entry points.
- [x] Select existing material and function assets or fixtures with a material output, function ports, and a call site. Record expression IDs, pin addresses, links, positions, and compile results before migration. Identify a fixture with a missing function port for diagnostic parity.
- [x] Gate: record the fixture selection, baseline results, and graph lifetime owner here and the public API mapping above before adding graph types.

### Stage 1: Build the graph from existing assets

- [x] Using the Stage 0 owner, implement transient graph and node ownership, stable-ID lookup, and rebuild. Represent the existing material output and function input/output expressions using their current identities. Resolve weak or replaced owners before dereferencing expressions.
- [x] Leave the existing inspection path available for parity checks. Compare nodes, pin directions and types, link endpoints, layout, and missing-port diagnostics on both fixtures.
- [x] Gate: opening and rebuilding twice yields equivalent graphs, leaves the asset clean, and creates no undo entry. Closing the editor releases the graph and its nodes.

Stage 1 validation used the implementation based on `ad5170dbe` in the commit carrying this receipt, with preset `Win64-Debug-DurinEditor`. DevTool built and ran the full MaterialEditingPersistenceTests (32/32) and MaterialCompileLifecycleTests (39/39). Exporting the existing canvas destructor resolved the Stage 0 persistence target link failure. Selection covers the two consumers that also compile the private session sources.

The object graph reuses the document projection for expression metadata and pin definitions. The reflected graph tracks its node array for garbage collection and references the asset weakly. The material-output specialization binds the authored output ID. A function uses its authored port expressions. Rebuild replaces transient nodes. Relinking preserves node objects when their ordering, identities and root roles still match. Connections resolve by stable endpoint keys. Stage 2 owns the switch to graph-backed canvas reads and edit-time synchronization.

### Stage 2: Switch the canvas and verify one edit end to end

- [x] Once parity passes, read canvas nodes and pins from the graph. Route drag feedback through the schema query and drop through its execution method. Synchronize via the graph. Reuse the document transaction implementation behind that entry point during migration. Resolve selection and pending gestures by ID across rebuilds, clearing targets that disappeared.
- [x] Gate: for a material and a function, connect, undo, redo, save, and reopen. Check the same endpoints and a single undo step per edit. Verify schema rejection leaves the graph, asset, and undo history intact. Compare preview and compile results with the baseline for equivalent expressions.

Validation used the Stage 2 implementation based on `bb7167ed2` in the commit carrying this receipt, on Windows MSVC x64 with preset `Win64-Debug-DurinEditor`. MaterialEditorInteractionTests passed 76/76, MaterialGraphEditingTests passed 106/106, and MaterialEditingPersistenceTests passed 33/33. Coverage includes native ImGui gestures and preview behavior, endpoint loss during a drag, the unfinished-call compile baseline, and a schema connect, reject, undo, redo, save and reload sequence for both asset types.

#### Stage 2 interface decisions

| Boundary | Implemented contract |
| --- | --- |
| Canvas and read model | Both sessions pass their graph to the canvas. The change-driven read model updates the reflected nodes and copies their pin views into the canvas and Details snapshots. Layout changes patch presentation in place. Selection and link gestures use stable IDs across refreshes. A lost endpoint cancels the gesture. |
| Schema query | `CanCreateConnection(Graph, A, B)` accepts either endpoint order. It returns Make, ReplaceInput or Disallow with a reason. Hover checks the candidate under the pointer. Missing pins, incompatible declared types, and dependency cycles are rejected before writing. Repeating a valid existing link is idempotent. |
| Schema execution | `TryCreateConnection(Graph, A, B, Replace, Mode)` refreshes endpoints and repeats the query on drop. A forward drag requires Shift to replace an occupied input. Reconnecting from that input already expresses replacement intent. Command results retain Durin status and diagnostics. |
| Graph synchronization | The private `LinkMaterialExpressionsFromGraph` takes the two endpoints of one accepted edit. The document commits the expression change in one transaction, then the graph reads back the committed links. Undo and redo feed the normal change-driven refresh. The editing session controls publication to the source asset. |
| Remaining consumers | Context-menu disconnects, creation actions, clipboard and other document commands remain Stage 3 work. Bulk output disconnection needs an atomic multi-input transaction before exposing the full UE-style `BreakPinLinks` operation. |

### Stage 3: Migrate remaining callers and retire obsolete reads

- [ ] After the canvas path passes, migrate creation, deletion, movement, clipboard, details edits, and function-signature refresh. Preserve existing transaction grouping and preview invalidation. Exercise a function port rename and removal with a material call site open.
- [ ] Search every workspace project's source and tests for the previous inspection API. Remove duplicate graph construction after migrating consumers. Keep any document transaction helpers internal. Verify the canvas and graph context actions use the public graph, node, and schema entry points for topology edits. Exercise the material root and a function call with reconstructed pins through the same canvas code, and confirm specialization requires no node-type branch in connection handling. Record the final ownership split in the editor architecture documents linked above.
- [ ] Gate: build MaterialEditor and affected consumers using the [build workflow](../Agents/BuildAndRun.md). Select graph interaction, persistence, editing-session, and function tests using the [test workflow](../Agents/Testing.md). Verify older assets open, edit, compile, and round-trip with stable IDs and equivalent links.
- [ ] Record tested revision, selections, results, and the manual canvas checks here. Complete the plan when evidence covers the final implementation for both asset types.
