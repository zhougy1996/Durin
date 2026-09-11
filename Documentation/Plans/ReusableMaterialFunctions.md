# Reusable Material Functions Plan

Summary: Add typed material function assets and replace expanded imported PBR graphs with texture-driven StandardPBR calls, including editor workflows and explicit asset migration.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

Selected by the user on 2026-09-11 after the RenderProxy parameter pass-through
fix (`5b1ea1344`). This commit records the implementation proposal only; no
implementation stage is complete. Stage 0 is next. The user permits aggressive
schema changes and upgrades or modification of the small existing asset set.
This authorization permits a coordinated content migration; it does not require
maintaining a permanent legacy compiler or silently discarding authored edits.
The user subsequently selected an abstract `DMaterialFunctionInterface` asset
base, with `DMaterialFunction` as its first concrete implementation. Function
instances remain deferred; this is a design update, not completed implementation.

This is M11 of the [Material System roadmap](../Roadmaps/MaterialSystem.md).
[Material Instance Shader Variants](MaterialInstanceShaderVariants.md) has
landed its implementation and remains active for final qualification. Consume its
landed property, variant publication, Cook and import contracts. M11 can proceed
independently; reconcile compiler snapshot, dependency, Cook and import changes
before editing shared seams. There is one
source/build writer per checkout.

## Goal

ImportedSurface exposes texture parameters, scalar/vector parameters and UV inputs
feeding one ordinary StandardPBR function call and a Surface output. Sampling,
channel extraction, normal processing and PBR composition live in reusable
function assets. Authors can open functions, reuse them with independent inputs,
and modify selected Surface attributes without expanding the entire function.

The runtime proxy remains parameter-identity agnostic. Functions disappear into
compiled material programs; Renderer never loads function assets or graph nodes.

## Existing Foundations and Change Sites

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramTypes.h` has
  program schema 4, typed Texture2D and Surface values, MakeSurface, and links
  identified by source node GUID plus positional output index. Function calls
  need stable interface identities beyond those positional indices.
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramTypes.cpp`
  constructs the expanded MakePBRMaterialProgram graph. Preserve its intentional
  shading behavior while moving composition into standard function assets.
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramCompiler.h` and
  the private compiler/lifecycle files own detached snapshots, deterministic IR,
  shared compile work and generation-safe publication. Extend these boundaries.
- `Engine/Source/Editor/AssetForgeBuiltins/Private/ImportedSurfaceMaterial.cpp`
  creates the expanded parent and compares its whole program with the built-in
  graph. `SceneDirectImport.cpp` consumes the shared parent. Replace exact graph
  equality with an explicit standard-asset revision and required interface check.
- MaterialEditor already has shared semantic commands, reflected presentation,
  transactions, clipboard and diagnostics. Generalize this graph ownership seam
  for function documents instead of adding a separate canvas implementation.

## Selected Design

### Function assets and stable interfaces

Introduce an Engine-owned abstract `DMaterialFunctionInterface` asset base and
derive `DMaterialFunction` from it. The base is specific to material functions,
not a general script interface. It provides the read-only function signature,
dependency queries and owning-thread entry point for a detached compilation
snapshot. It does not require an editable graph or own graph presentation.

`DMaterialFunction` owns the bounded reflected function graph, ordered typed
input/output declarations and editor presentation, and implements the base
contract. Graph mutations and transactions target this concrete graph owner;
callers and dependency resolution consume the abstract contract. Keep snapshot
creation on the owning thread and reuse the existing compile lifecycle rather
than introducing per-subclass scheduling or runtime virtual shader execution.

FunctionCall references `DMaterialFunctionInterface` through the existing asset
reference machinery, not a concrete `DMaterialFunction` reference. Validation,
reference enumeration, asset picking and snapshot closure use that declared base
type, while creation offers only supported concrete types. Do not require callers
to downcast to obtain the signature or effective compilation snapshot.

The first implementation contains only `DMaterialFunction`. Reserve the base
boundary for a future `DMaterialFunctionInstance`, but do not add instance assets,
override storage or inherited-function semantics in this plan. This class-level
interface is separate from the stable input/output port identities below.

Reuse common node/link values, but give functions their own terminals rather than
pretending that every function has a material Surface root.

FunctionInput, FunctionOutput and FunctionCall are generic graph concepts. Calls
hold an asset reference and input bindings keyed by stable interface GUIDs;
output connections identify the stable function output GUID. Built-in opcode pins
may remain positional. Reordering or renaming function interfaces preserves
connections; deleting/retyping an interface yields a located diagnostic rather
than reconnecting by index. Removed bindings remain recoverable through Undo and
must not be silently reassigned to another interface.

Support the existing numeric types, Texture2D and Surface, multiple outputs and
nested calls. Declare exact types; no generics, overload resolution or arbitrary
shader code. Functions do not implicitly inject material parameters: their
internal graph uses input ports and constants. Exposed parameter declarations
remain owned by the root material, preserving instance override identity and
preventing same-name parameter collisions between calls.

Unconnected optional inputs use declared defaults; required inputs diagnose a
missing binding. Keep preview fixtures separate from production defaults.
Numeric literals, texture fallback/sampling policy and default Surface attributes
have explicit typed representations. A static fallback expression may use other
function inputs without introducing cycles: per-map UV defaults to the common UV,
which defaults to UV0. This needs no general static-switch feature.

### Texture-driven standard library

TextureParameter remains one generic texture-object node. FunctionCall accepts
that object; TextureSample2D performs sampling inside the function. Preserve the
existing sampler and fallback association when passing texture inputs through
nested calls. Define the precedence of input defaults and caller sampling state
before implementation; never infer it from PBR GUIDs.

Create ordinary standard-library assets for UVTransform, normal sampling/strength,
packed ORM sampling, and StandardPBR. Lower-level functions may be called directly.
StandardPBR accepts textures plus factors and UVs and outputs Surface. Common
inputs are shown first; independent per-map UVs and less common settings are
advanced inputs. Do not expose the entire expanded implementation as call pins.

Preserve all currently imported channels, opacity/mask behavior, emissive factors,
UV channels/transforms, sampler states and texture fallbacks. No-texture inputs
have explicit neutral behavior: flat normal, white multiplicative samples and
appropriate emissive defaults. A missing authored connection and a temporarily
unavailable texture resource are distinct states; resource fallback must still
work without recompilation.

Stage 0 freezes the importer channel matrix. Provide a bounded StandardPBR entry
for the current independent-map layout and a packed ORM entry where warranted by
actual imports; both reuse composition helpers. Do not pack existing images or
change their import settings just to fit the function interface. Packed ORM uses
one sample with three outputs when texture, UV and sampling requirements match.
Different UVs or sampling requirements must not be merged. Shared function code
alone is not evidence of shared texture fetches.

Color, normal and data sampling expectations belong to explicit graph/resource
contracts. Validation may use actual texture declarations and sample requirements;
it must not restore GUID-based usage rejection, CPU clamps or normalization.
Retain the current intentional PBR clamps and safe normal behavior inside the
library graph/shared shading primitives. StandardPBR is not a new hardcoded PBR
opcode. Parameter names and function names do not select compiler behavior.

### Surface composition

Reuse Surface and MakeSurface. Add typed GetSurfaceAttributes and
SetSurfaceAttributes operations with explicitly selected attribute pins.
Unmodified fields pass through, defaults remain deterministic, and arithmetic
cannot consume Surface values. A StandardPBR result can have Emissive or Normal
replaced without duplicating its graph. Surface blending and Material Layers are
outside this plan.

### Compilation and dependencies

Resolve the bounded transitive function closure on the owning thread into an
immutable value snapshot. Workers receive no reflected objects or asset loads.
Validate missing references, recursive calls, incompatible ports and limits on
call depth, dependency count, total expanded nodes, texture resources and payload.
Freeze numerical limits in Stage 0 against existing compiler/resource bounds.

Expand calls before existing normalization and layout derivation. Namespace
internal node identities by call path so separate invocations cannot collide.
Lower multi-output calls and Surface access deterministically; prune unreachable
outputs and reuse shared expressions where valid. Keep a separate diagnostic
source map from lowered expressions to root call path, function asset and node.
Authored GUID/order/presentation changes must not disturb equivalent shader keys.

Dependency revision and compile identity are distinct. Track every referenced
function/interface for invalidation; hash the effective compiled semantics and
compiler versions for artifact identity. Edits to code, interfaces or defaults
invalidate affected callers, while presentation-only edits do not compile.
Dynamic root parameter values and bound texture identities stay out of shader
keys. Test warm/cold compilation and two calls with different bindings.

Extend existing reference enumeration, reload/relocation/deletion handling and
compile lifecycle. Pending or failed function edits retain the complete accepted
program/layout/value contract or select the existing error terminal. Stale closure
snapshots cannot publish. Cook resolves function dependencies ahead of compilation;
cooked runtime loads accepted artifacts without authored functions or a compiler.
Do not add another scheduler, DDC or render publication path.

### Editor and asset migration

Support function creation/opening, interface editing, function-call insertion,
enter-function navigation and previews through the shared command surface.
Canvas and structured operations use the same validation, transaction and
clipboard contracts. Commands bind function pins by stable identity. Diagnostics
navigate through nested calls to the originating node. Surface outputs can preview
as a material; numeric/texture outputs use an explicit preview wrapper.

Bump affected authored schemas and compiler versions once the new formats are
locked. Keep any reader for the prior format bounded to an explicit upgrade path;
normal authoring, compilation and Cook use the new representation. Do not retain
parallel legacy and function-based PBR production implementations after migration.

Inventory actual packages and source-controlled storage in Stage 0, including
external LFS-backed content. Record old identity, path, references and disposition.
Migrate the shared ImportedSurface parent to a small call graph, retaining root
parameter GUIDs and material/instance paths where possible. Preserve override
values, parent chains, material slots, property flags and sampler/UV settings.
Known expanded templates can be converted automatically. Modified graphs remain
valid independent graphs after schema upgrade; use reviewed per-asset edits when
replacing their subgraphs, never overwrite them based only on a familiar GUID.

Standard assets have explicit revision/provenance. Bootstrap creates missing
assets; upgrade recognizes supported revisions, and reports incompatible edited
interfaces without repeatedly restoring a built-in graph. Use existing package
save/catalog operations, persist function dependencies first, and make retries
idempotent. Record saved/failed packages and a recoverable checkpoint for interrupted
multi-package migration. Rebuild derived artifacts instead of treating them as
source assets. Re-run migration to verify no additional changes.

## Implementation Stages

### Stage 0: Freeze interfaces and migration inventory

- [ ] Enumerate material/function change sites, import channel layouts, existing
  packages and M13 overlaps; record the actual storage and upgrade execution path.
- [ ] Freeze StandardPBR ports/defaults, sampler precedence, stable pin schema,
  bounded expansion limits and schema/version changes in this plan.
- [ ] Capture current representative imported-material render results and resource
  counts; include independent maps, packed maps, missing textures and custom UVs.

Exit: concrete interface tables, per-asset dispositions and baseline fixtures;
no unresolved schema/default decisions block Stage 1.

### Stage 1: Function assets and compiler expansion

Depends on Stage 0.

- [ ] Implement the abstract DMaterialFunctionInterface asset contract and concrete
  DMaterialFunction graph owner; call references and snapshot/dependency queries
  use the base without concrete downcasts. Verify abstract asset creation is
  rejected and base-typed references survive serialization and asset operations.
- [ ] Implement typed stable interfaces, call bindings,
  snapshot closure, bounded expansion, multi-output lowering and source maps.
- [ ] Add Surface Get/Set and validate nested numeric, texture and Surface calls.
- [ ] Test round trips, reordered/renamed/deleted ports, recursion, missing assets,
  expansion limits, deterministic output and independent invocation inputs.

Exit: a programmatic texture-to-Surface function compiles through existing layouts;
malformed calls diagnose correctly and no worker reads live assets.

### Stage 2: Dependency lifecycle and cooked execution

Depends on Stage 1.

- [ ] Integrate function references with invalidation, asset operations, shared
  compile work, accepted generations, cancellation and shutdown.
- [ ] Extend Cook dependency admission and cache identity/versioning.
- [ ] Test nested edits, stale completion, compile failure/recovery, relocation,
  deletion, cold/warm cache and cooked loading without authored functions.

Exit: dependency edits safely update all callers and Cook artifacts are sufficient
for runtime. Reconcile shared seams with M13 before either plan edits them again.

### Stage 3: Function authoring workflow

Depends on Stages 1 and 2.

- [ ] Generalize MaterialEditor graph documents and semantic commands for functions.
- [ ] Implement port editing, call insertion/navigation, default visibility,
  preview wrappers, Surface editing and nested diagnostic navigation.
- [ ] Verify Undo/Redo, clipboard, save/reload and multi-document dependency edits
  through both structured commands and human canvas workflows.

Exit: authors can build and reuse a function without source edits or raw asset
patches; interface changes retain stable connections and actionable diagnostics.

### Stage 4: Standard PBR library and content upgrade

Depends on Stages 1-3.

- [ ] Author standard functions, replace Import graph construction/validation,
  and generate intentional compact graph presentation.
- [ ] Upgrade the inventoried assets and preserve instance bindings and slots;
  remove retired expanded PBR production builders and migrate their fixtures.
- [ ] Verify fresh import, reimport, missing resource fallback, save/reload,
  modified assets and interrupted/repeated migration.

Exit: ImportedSurface has one standard function call with inputs and Surface
output, no expanded sampling/math chain; every inventoried asset is accounted for.

### Stage 5: End-to-end qualification and handoff

Depends on Stage 4.

- [ ] Run bounded CPU/editor/import/Cook suites selected from the native registry.
- [ ] Qualify migrated and newly imported materials on supported forward, GBuffer
  and shadow paths, including StaticMesh/SplineMesh and masked materials.
- [ ] Compare Stage 0 renders and texture/resource counts; inspect generated code
  to confirm packed-map reuse and no unexpected sampling expansion.
- [ ] Build the editor; document implemented contracts in MaterialSystem and
  MaterialGraphOperations, record receipts, and complete M11 in the roadmap.

Exit: all gates pass, migration is repeatable, edited function assets propagate
correctly, and no runtime dependency on function graphs or PBR GUID rules remains.

## Validation and Handoff

Follow [Testing](../Agents/Testing.md) and [Build And Run](../Agents/BuildAndRun.md).
Discover exact target ownership through the registry during Stage 0. MaterialTests,
scene-import tests and material Vulkan/Cook coverage are candidate lanes, not a
substitute for registry selection. GPU visual qualification is a required Stage 5
gate because template execution and asset content change. Unavailable GPU access
must be reported as outstanding, not counted as a pass. This proposal-only commit
requires documentation validation, not compilation.

Each implementation commit updates the stage checklist and evidence and uses the
repository Plan/Stage trailers. Persist lasting contracts in the owning Runtime
and Editor documents only after implementation.

## References

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [UE Material Functions](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-functions-overview): typed texture inputs and reusable function assets.
- [UE Material Attributes](https://dev.epicgames.com/documentation/unreal-engine/material-attributes-expressions-in-unreal-engine): grouped Surface output and selective attribute access.

UE references inform the authoring model; this plan owns Durin-specific schema,
compiler, dependency, parameter-scope and migration decisions.
