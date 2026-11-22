# Material-Owned Root Inputs Plan

Summary: Move base-material inputs and root placement onto the material, project the root in MaterialEditor and convert authored packages to a single terminal-free format.

Last reviewed: 2026-11-22

Status: Active
Completed:

## Current Status

Design selected. Implementation and package conversion are pending. Durin currently constructs one `DMaterialExpressionMaterialOutput` per authored material. Its payload owns the root inputs and its expression GUID also identifies the canvas root. The scene-import fix in `982884ea6` independently retires replaced expressions and detaches them from the package. Preserve that behavior for ordinary expression replacement.

Coordinate changes to graph gestures, layout and replay with [Material Graph Transaction Alignment](MaterialGraphTransactionAlignment.md), especially its stages 3 and 4. Use the transaction implementation present when each stage begins. The [completed graph-object plan](MaterialEditorGraphObjects.md) records why the transient root currently binds an expression. This plan changes that binding and reuses the graph and root node classes.

## Goal

Each base material owns one set of authored inputs and one root position. Its expression collection contains the authored expression objects. A material graph projects exactly one root from its material. Function graphs retain their named input/output expressions and port GUIDs. Material instances continue to inherit their graph.

### UE 5.8 reference and Durin interfaces

Source paths below are relative to the local UE 5.8 `Engine/Source` directory.

| Concern | UE 5.8 reference | Durin target |
| --- | --- | --- |
| Authoring data | `Runtime/Engine/Public/Materials/Material.h`: `UMaterialEditorOnlyData` owns the typed material inputs and `ExpressionCollection`. `UMaterial::GetExpressionInputForProperty(EMaterialProperty)` selects an input. | Store one reflected, editor-only `FMaterialAttributeRoot` on `DMaterial` alongside its editor-only expression collection. Reuse Durin's typed binding/default and packed-attributes representation. Keep `GetExpressionOutputs()` as direct const access and property-keyed input access for editing. Reuse the material's existing editor-only storage boundary. |
| Graph construction and layout | `Editor/UnrealEd/Classes/MaterialGraph/MaterialGraphNode_Root.h` and `Private/MaterialGraph.cpp` in the same module: the root refers to `UMaterial`. Graph synchronization writes its position to `UMaterial::EditorX/EditorY`. | Bind the transient material root to `DMaterial`. Persist root placement separately from GUID-indexed expression layout. Address the root by its graph owner and endpoint kind, and its input by the stable material property or packed-attributes pin. No serialized root GUID, magic GUID or array index. |
| Editing and compilation | `UMaterialGraph::LinkMaterialExpressionsFromGraph` calls `Material->Modify()` before synchronizing root inputs and coordinates. Root pins resolve to material inputs. | Record the material before editing its root. Keep ordinary node edits on their expression owners. Pass the material's root bindings to the existing MIR `FinishSurface` path. Preserve execution semantics and the separation of authoring data from cooked products. |

### Contracts to revise during implementation

| Owning contract | Required change |
| --- | --- |
| [Material graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md) | Replace the exactly-one-output-expression invariant and strict terminal-taking setter with an expression body plus explicit root inputs. Split root endpoint identity from expression and function-port identity. Specify ownership of edits, graph-change events, inactive links and literal defaults. |
| [Material graph canvas](../Editor/Architecture/Materials/MaterialGraphCanvas.md) | Root selection, pins, navigation and layout use the material-root endpoint. Preserve root movement and mixed-selection behavior. Exclude the root from deletion, duplication and clipboard expression payloads. A root layout edit has no shader effect. |
| [Material system](../Runtime/Rendering/Materials/MaterialSystem.md) and [expression building](../Runtime/Rendering/Materials/MaterialExpressionBuilding.md) | Document root storage, fresh-material defaults, closure validation, dependency traversal and compilation from material inputs. Retain Surface/PostProcess rules, packed and per-property forms, function interfaces and shader semantics. |
| [Custom version registry](../Runtime/ObjectSystem/Versioning.md) | Record the authored-format cutover and unsupported legacy version after conversion. Review `FMaterialOutputVersion` use in cook identity. Distinguish a cache-key change from a shader or cooked payload layout change. |

Update implemented contracts with the stage that changes the behavior. Keep historical plans as the record of the earlier design.

## Implementation Stages

### Stage 1: Inventory assets and capture migration evidence

- [ ] Audit Engine, Sandbox and RoadWeaver source, tests, checked-in content including LFS packages, fixtures and generators. Find consumers of the terminal class, its serialized class name, strict graph setter, root GUID, pin addresses, change notifications and layout. Include AssetForge recipes, DurinAssetTool and function-preview material construction.
- [ ] While the old reader can still resolve the terminal export, capture a reproducible inventory and representative pre-conversion results. Include a default material, linked and literal inputs, inactive packed and individual inputs, PostProcess, a custom root position, a function call with a named output, and an instance referencing a base material. Record expression/parameter/port identities, root links and placement, and compiled results for comparison.

### Stage 2: Move the authored root and convert packages

- [ ] Establish the new reflected fields, version and single new-format writer. Switch construction, validation, recipes and expression assignment to body plus root. Use one source-to-copy map for links inside expressions and every root link, including inactive links. Validate against the candidate owner's expressions before publishing. A rejected assignment preserves the previous material.
- [ ] Convert old packages while their legacy exports are still readable, using a temporary migration reader or a pinned old-format exporter and new-format importer. Copy the complete root payload and look up placement with the old terminal GUID. Preserve package paths, asset identity, ordinary node layout and named function outputs. Report malformed packages individually. Stage converted output, reopen and verify it before replacing originals. A repeated run skips already-current packages.
- [ ] Resave affected repository assets and fixtures and update generators. Compare the inventory and pre-conversion evidence. Verify duplicate/copy, save/reopen, reload and prepared scene-import publication. Every root link in a copied material resolves inside the copy; publication contains no retired or terminal export.

### Stage 3: Project the editor root and record changes on the material

- [ ] Migrate graph rebuild, public pin/selection addresses, schema, canvas, Details, clipboard, placement and incremental invalidation. Keep a material root available for an empty body; expose function ports only in function graphs. Rebuild selection from semantic identity after undo, reload or owner replacement.
- [ ] Open the edit scope before mutation. Exercise root connect/disconnect, default values, output-mode/configuration changes, deletion or replacement of a connected source, and grouped moves of the root with ordinary nodes. Verify both edit orders in a shared transaction, undo/redo, cancel, dirty/save checkpoints and publication counts. Verify layout changes preserve compilation state. Check snapshot size on a representative graph before introducing any special-purpose history record.
- [ ] Exercise function previews, import recipes, asset generation and editor tooling through their normal entry points. Preserve inactive inputs and their defaults across mode/domain switches and undo. Verify copied or pasted expression sets never manufacture a second root.

### Stage 4: Remove the legacy terminal and close acceptance

- [ ] After conversion and consumer migration, remove the terminal class and its constructor, serializers, GUID addressing, special-case filters and temporary dual-format code from the shipping tree. A stale package reports its unsupported version through the supported loading/tooling path. Audit all workspace projects for obsolete symbols and old-format exports.
- [ ] Build the workspace with the host DevTool launcher. The reflected class removal, serialized layout and shared editor API change require an incremental `all` build. Run the relevant graph editing, canvas, persistence, material compiler, cook, package reload and complete `SceneImportTests` suites. Verify LFS payloads and reopen converted packages with the final binary. Record the tested revision, preset, selections, conversion counts and any remaining failures in this plan.
- [ ] Compare pre/post root bindings, layout, representative shader results and cooked behavior under the same settings. Confirm graph-stripped cooked loads and instance overrides work. Update the owning contracts above and close the plan after validation.
