# Material Expression Semantics and Language Plan

Summary: Add stage- and space-aware material values, complete the bounded math and context expression set, and introduce compile-time selectors without adding a new material pass or Surface output.

Last reviewed: 2026-09-29

Status: Active
Completed:

## Current Status

The Material System roadmap has selected M12. M8 observability and runtime
parameters, M10 compiled layouts, M11 reusable functions, and M13 shader variants
are complete enough to admit this work. There is no other active material plan.

The current expression model carries only `EMaterialProgramValueType`; MIR nodes,
function ports, links, normalization, canonical encoding, and generated code do
not carry evaluation-stage or coordinate-space semantics. Existing Surface roots
are fragment-only, while `WorldPosition`, `Time`, texture coordinates, texture
samples, and numeric operations are admitted through shape-only signatures.

Stage 0 is current. It freezes the semantic algebra, pass-context ABI,
static-selection ownership, compatibility policy, fixtures, and budgets before
the shared compiler representation changes.

## Goal

Make every material value carry enough meaning for the compiler to determine its
base shape, legal evaluation stage, spatial kind and coordinate space. Use that
model to add the selected math, geometry, view, static-bool, quality, and
feature-level expressions through the existing material/function compiler,
variant, editor, Cook, and renderer contracts.

The completed work preserves the existing eight-output Surface ABI and the
existing Forward, GBuffer, and shadow passes.

## Scope

- One detached value-semantics contract shared by authored validation, MIR,
  normalization, canonical identity, generated code, diagnostics, and functions.
- Evaluation-stage legality for `Vertex`, `Pixel`, and `Both`.
- Coordinate spaces `Object`, `World`, `View`, `Tangent`, and `Screen`, plus a
  non-spatial state for ordinary numeric values.
- Spatial kinds for position, direction, normal, and screen coordinate. Shape
  remains separate; a generic `Float3` is not silently a world-space value.
- Dot, Cross, Length, Distance, Pow, Sqrt, Exp, Log, Floor, Ceil, Round, Frac,
  Fmod, Step, SmoothStep, Sign, Reflect, and explicit spatial transforms.
- CameraPosition, CameraVector, VertexNormal, ObjectPosition, ScreenPosition,
  ViewSize, and reconciliation of existing WorldPosition and Time semantics.
- Authored static bool declarations and instance overrides, Static Bool and
  Static Switch, plus compiler-environment Quality and Feature Level switches.
- Shared Material and Material Function authoring, diagnostics, preview,
  persistence, variant identity, Cook, source-free load, reload, and recovery.
- CPU and GPU qualification across StaticMesh and SplineMesh in Forward,
  GBuffer, reachable masked shadow, preview, and thumbnail paths.

## Non-Goals

- World Position Offset, Customized UV outputs, vertex-to-pixel varying
  allocation, automatic vertex hoisting, tessellation, mesh shaders, or a new
  shader stage.
- SceneDepth, PixelDepth, SceneColor, derivatives, refraction, decals,
  post-process materials, compute materials, or another material domain.
- New Surface attributes, shading models, blend modes, geometry passes, or a
  replacement Surface ABI.
- Implicit coordinate conversion, implicit position/direction/normal
  reinterpretation, or automatic cross-stage interpolation.
- Runtime mutation of static bools, quality, or feature level through dynamic
  material instances. These select accepted compiled variants.
- Compiling every Cartesian product of static declarations, arbitrary shader
  source, or a second checker in MaterialEditor or Material Functions.

## UE Reference and Selected Adaptation

The following Epic public documentation was checked on 2026-09-29. It identifies
public responsibilities and observable semantics, not private implementation.

| UE reference | Useful responsibility | Durin adaptation |
| --- | --- | --- |
| [Material attribute shader frequency](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialAttributeDefinitionMap) | Outputs identify their shader frequency | Current Surface outputs remain Pixel roots. Stage legality becomes a value/compiler contract so future vertex roots do not require retrofitting every opcode. |
| [Customized UVs](https://dev.epicgames.com/documentation/en-us/unreal-engine/customized-uvs-in-unreal-engine-materials) | Vertex and pixel evaluation differ, and non-linear vertex work changes interpolation | Never move work between stages automatically. `Both` means legal in either requested root, not “evaluate once and interpolate”. |
| [Vector operation expressions](https://dev.epicgames.com/documentation/en-us/unreal-engine/vector-operation-material-expressions-in-unreal-engine) and [`TransformVector`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialCompiler/TransformVector) | Transform declares source/destination bases; vector operators have defined widths | Carry explicit source/destination payloads and separate position, direction, and normal rules. Do not copy implicit default-space coercions. |
| [Coordinate expressions](https://dev.epicgames.com/documentation/en-us/unreal-engine/coordinates-material-expressions-in-unreal-engine) | Camera, object, vertex-normal, screen, view-size, and world-position expressions have distinct meanings; VertexNormalWS is vertex-only | Freeze exact Durin meanings below and diagnose unavailable stages/passes. |
| [Quality Switch](https://dev.epicgames.com/documentation/en-us/unreal-engine/utility-material-expressions-in-unreal-engine), [scalability reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-reference-for-unreal-engine), and [`UMaterialExpressionQualitySwitch`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterialExpressionQualitySwitch) | Quality selects expression networks at compilation, increases permutations, has a default branch, and requests inputs on demand | Select one branch before normalized IR/resource discovery, compile only requested configurations, and expose variant cost through existing diagnostics. |
| [Material Functions](https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-and-using-material-functions-in-unreal-engine) | Reusable graphs obey material compiler restrictions | Expand through the existing invocation boundary and apply the same semantic engine; add no function-only rules. |

Durin does not adopt UE's full domain breadth, custom code, hidden coercions, or
Customized UV interface in this milestone.

## Selected Architecture

### Value semantics

Introduce one detached value contract used by builder values and MIR nodes:

- base type: the existing scalar/vector/resource/Surface shape;
- legal-stage mask: `Vertex`, `Pixel`, or their union `Both`;
- spatial kind: `None`, `Position`, `Direction`, `Normal`, or `ScreenCoordinate`;
- coordinate space: `None`, `Object`, `World`, `View`, `Tangent`, or `Screen`.

Resources and Surface have no spatial space. Position, direction, and normal are
three-component values in Object, World, View, or Tangent space. Screen
coordinates use Screen space. Constants and ordinary numeric parameters remain
non-spatial until an explicit expression gives them spatial meaning.

Stage propagation intersects the operation's legal mask with all operands. A
root requests one stage; an empty intersection is a source-located error. No edge
creates an interpolator. Existing Surface roots request Pixel. The compiler may
represent Vertex-only values, but they cannot reach a current Surface output.

Space compatibility is exact unless an explicit transform appears. Stage 0 owns
the complete operator table, including scalar scaling, position subtraction,
same-space Dot/Cross/Distance, swizzle/append/splat, normal decode/blend, Surface
attributes, texture coordinates/sampling, and intentionally rejected ambiguous
component-wise spatial operations. Normal transformation uses inverse transpose,
not direction transformation.

### Context semantics

| Expression | Type and semantics | Legal stage |
| --- | --- | --- |
| Time | Float, non-spatial, current material-view time | Both |
| WorldPosition | Float3 World Position at the current fragment | Pixel |
| CameraPosition | Float3 World Position of the active pass view | Both |
| CameraVector | normalized Float3 World Direction from fragment to active pass camera | Pixel |
| ObjectPosition | Float3 World Position at the render primitive's bounds center | Both |
| VertexNormal | Float3 World Normal from the vertex factory before material displacement | Vertex |
| ScreenPosition | Float2 ScreenCoordinate normalized to the active viewport | Pixel |
| ViewSize | Float2 non-spatial active viewport size in pixels | Both |

“Camera” means the active pass view. Shadow materials observe the shadow view,
not a main-camera singleton; preview and thumbnail paths supply their own view.
Unavailable context fails compile/reflection admission rather than filling zero.
Texture sampling remains Pixel-only in this plan.

Explicit transform operations carry source, destination, and
position/direction/normal mode in canonical payload. Supported Object/World/View/
Tangent pairs use existing object, normal, view, and tangent bases. Unsupported
pairs fail normalization. Arbitrary Screen-to-world reconstruction is excluded.

### Functions

Function ports retain base shape and gain a semantic constraint: unconstrained,
non-spatial, or exact spatial kind/space, plus a legal-stage mask. An
unconstrained numeric input adopts caller semantics per invocation. Exact inputs
validate them. Outputs are inferred from the expanded body and checked against
their declared constraint.

Expanded function values use the same operator table, stage intersection, space
validation, normalization, diagnostics, and encoding as root material values.
Call paths remain attached to errors. No opaque function node or second checker
may bypass these rules.

### Static selection and variants

Static bools are a separate authored domain keyed by stable GUID. They do not
enter uniform layouts, dynamic-instance APIs, or collection buffers. Base
materials provide defaults; authored instances override them through the existing
variant lifecycle. Changing an effective value requests or reuses a matching
accepted variant.

Static Switch selects before normalized IR, active resource discovery,
dependency capture, and code generation. Both authored inputs must be structurally
well-formed, but only the selected reachable branch contributes IR, layout,
dependencies, source, code, and limits. Static bool declarations are bounded to
32 per root. Compilation and Cook enumerate only requested root/default and
authored-instance configurations, never all `2^N` combinations.

Add Low and High material quality with a Default branch. Feature Level Switch
uses existing RHI `ES3_1`, `SM5`, and `SM6` tiers plus Default. Quality and the
accepted RHI feature level become explicit compiler/variant request inputs shared
by renderer, preview, Cook, and source-free runtime. Game selects compatible
precompiled programs and never compiles a missing variant.

Selection affects identity through exact configuration and selected normalized
program. Identical normalized selections may reuse a program. Existing
diagnostics expose requested selection and loaded-family variant cost.

### Versions and ownership

Engine owns expressions, graph/function schemas, validation, MIR, variants, and
Cook admission. Renderer owns exact pass context and transform bindings.
RenderCore/RHI retain shader and capability primitives. MaterialEditor owns
presentation, not semantic truth.

Node payload, function-port, MIR, compiler-environment, generated/reflected
shader, and source-free payload changes require coordinated version increments.
Stage 0 selects canonical resave or bounded migration for tracked assets;
unsupported schemas fail explicitly.

## Current Code Boundaries

| Current owner | Planned change |
| --- | --- |
| `MaterialProgramTypes.h` / `MaterialProgramSignatures.cpp` | Replace shape-only signatures with shared semantics and append selected opcodes/payloads without reusing retired values. |
| `MaterialExpressionBuild` and expression `Build` methods | Carry semantic values, centralize admission, preserve sources, and select branches lazily. |
| `MaterialProgramCompiler` | Normalize semantic MIR, canonicalize selections, derive exact active resources, and version identity. |
| `MaterialProgramGenerator` | Generate math/context/transforms and reflection-check exact context while retaining Surface ABI. |
| Function types/expansion | Add port constraints and reuse common semantics across nested calls. |
| Material parameter/instance ownership | Add static bool declarations/overrides without dynamic setters or uniform slots. |
| Material Cook/cooked program | Persist exact versions/configurations and reject missing or incompatible programs. |
| MaterialEditor | Author nodes/payloads, display semantic contracts/errors, and select preview quality/feature. |
| Renderer surface/vertex factories | Supply only reachable view, object, normal, tangent, and transform requirements; add no pass. |

All shared API consumers in `Durin.dworkspace` must migrate together. Lasting
rules move to owning Runtime and Editor documents as stages close.

## Implementation Stages

### Stage 0: Freeze Semantics, ABI, Compatibility, and Baselines

Dependency: completed M8/M10/M11/M13. Outcome: no semantic/configuration decision
remains implicit before representation changes.

- [ ] Inventory every opcode, Surface input, function port/default, compiler
  environment, generated shader entry, vertex-factory input, pass context,
  tracked asset, editor node, Cook version, and workspace consumer.
- [ ] Publish the complete shape/stage/kind/space operator table for every old and
  new operation, including preservation/clearing and explicit rejection rules.
- [ ] Freeze supported transform pairs, translation, inverse-transpose normal,
  tangent handedness, non-uniform scale, spline semantics, and singular failure.
- [ ] Freeze active-view ABI and exact Forward/GBuffer/masked-shadow/preview/
  thumbnail meanings. Record reused and missing shader/binding fields.
- [ ] Freeze static-bool persistence/overrides, quality ownership and runtime
  selection, feature derivation, keys, fallback, Cook inventory, missing-variant
  behavior, and the 32-declaration bound.
- [ ] Choose graph/function migration and exact graph, function, MIR, generator,
  compiler-envelope, pass-contract, layout-if-needed, and DMAT versions. Inventory
  the tracked asset corpus.
- [ ] Record deterministic baselines for graph identity/source/code, function
  expansion, variants, existing passes, preview/thumbnail, Cook, reload, and
  recovery. Freeze compile, artifact, variant, and Cook budgets before Stage 2;
  timing claims require a valid qualification lane.

Completion: one reviewed table determines every result/error; pass and variant
owners, migration, fixtures, commands, and budgets are recorded in this plan.

### Stage 1: Establish Stage- and Space-Aware Values

Dependency: Stage 0. Outcome: existing materials compile through semantic MIR
before the language grows.

- [ ] Introduce the detached value contract and migrate emitter values, MIR,
  signatures, validation, normalization, encoding, sources, and diagnostics.
- [ ] Migrate every existing opcode and eight Pixel Surface roots. Preserve valid
  tracked output and explicitly migrate formerly ambiguous links.
- [ ] Add canonical position/direction/normal transform IR and deterministic
  invalid-pair diagnostics without exposing screen reconstruction/interpolation.
- [ ] Extend function port contracts and invocation substitution; prove nested
  calls, defaults, independent calls, dependencies, and call-path diagnostics.
- [ ] Apply selected version/migration policy; incompatible authored/cooked data
  must fail or migrate explicitly, never reinterpret silently.
- [ ] Add editor pin/tooltips and diagnostics for stage, kind, space, transforms,
  and call paths without editor-owned semantic logic.

Completion: all existing accepted content uses semantic MIR; invalid stage/space
graphs fail before shader compilation; no Surface ABI or rendered result changes.

### Stage 2: Add Pure Math and Geometric Operations

Dependency: Stage 1 and frozen budgets. Outcome: general expression language
grows without new renderer inputs.

- [ ] Add Dot, Cross, Length, Distance, and Reflect with exact width, same-space,
  kind, zero-length, and result-semantics rules.
- [ ] Add Pow, Sqrt, Exp, Log, Floor, Ceil, Round, Frac, Fmod, Step, SmoothStep,
  and Sign with exact broadcast, width, folding, and non-finite/domain behavior.
- [ ] Expose nodes through catalog, command API, canvas, copy/paste, Undo/Redo,
  persistence, automation, and diagnostic navigation.
- [ ] Add root/nested-function fixtures for valid inference, every rejected
  mismatch, disconnected validation, identity, source location, and generated code.
- [ ] Compare frozen compiler/artifact fixtures and resolve regressions before
  renderer context; do not infer GPU cost from source lines.

Completion: selected math works identically in materials/functions with
deterministic identity and no renderer/pass ABI change.

### Stage 3: Add Context Sources and Spatial Transforms

Dependency: Stage 2. Outcome: spatial/view graphs render through existing passes.

- [ ] Add CameraPosition, CameraVector, ObjectPosition, VertexNormal,
  ScreenPosition, and ViewSize; reconcile WorldPosition, Time, coordinates,
  sampling, and normals with the same contract.
- [ ] Expose Transform Position, Direction, and Normal over the supported
  Object/World/View/Tangent matrix with exact non-uniform/tangent behavior.
- [ ] Extend compiler requirements and renderer bindings only for reachable
  context. Reflection/layout mismatch fails transactionally with current fallback.
- [ ] Integrate StaticMesh/SplineMesh vertex factories and pass-local context in
  Forward, GBuffer, and reachable masked shadow; preserve opaque-shadow freedom.
- [ ] Prove preview/thumbnail, two views, resize, object movement, non-uniform
  scale, spline deformation, reload, recovery, and shadow-view semantics.
- [ ] Retain VertexNormal as Vertex-only. With no vertex Surface root, prove
  detached/function semantics and deterministic rejection from Pixel outputs;
  activation belongs to later WPO/vertex-output work.

Completion: Pixel graphs consume exact context in existing passes; Vertex-only
data cannot leak into Pixel roots; no new pass or output exists.

### Stage 4: Add Static Bool, Quality, and Feature Selection

Dependency: Stage 3. Outcome: compile-time selection reuses bounded variants.

- [ ] Add static bool declarations/defaults and instance overrides with GUIDs,
  inheritance, editing, persistence, invalidation, variant resolution, and no
  dynamic/uniform representation.
- [ ] Add Static Bool and lazy Static Switch. Validate authored inputs but admit
  only the selected branch into IR, resources, dependencies, limits, and code.
- [ ] Add Low/High Quality and ES3_1/SM5/SM6 Feature Level switches with Default;
  route selections through requests, preview, lookup, diagnostics, and identity.
- [ ] Extend Cook/DMAT for exact required root/instance/quality/feature variants;
  reject incompatible feature tiers or missing Game variants without compilation.
- [ ] Prove inactive branches retain no resources/dependencies, identical results
  reuse programs, changes request one variant, and declaration/cache bounds hold.
- [ ] Record variant/code/Cook growth for fixed 1/4/8 switches and Low/High times
  supported feature tiers; never enumerate an unbounded Cartesian product.

Completion: selectors prune before resource/layout generation, reuse safely, and
load source-free with explicit configuration.

### Stage 5: Complete Product Integration

Dependency: Stages 1-4. Outcome: the language works through all workflows.

- [ ] Complete reflected properties, catalog/search, details, graph commands,
  canvas, Undo/Redo, copy/paste, preview selection, statistics, and diagnostics.
- [ ] Migrate/canonical-resave the frozen corpus. Verify duplicate, move, rename,
  reload, delete, replacement, round trip, and imported-material behavior.
- [ ] Complete source-free Cook/runtime coverage for missing, corrupt, incompatible,
  and compiler-provider-absent cases.
- [ ] Exercise async supersession, cancellation, last-known-good, failure/retry,
  cache hit/miss, unload, world teardown, recovery, and shutdown.
- [ ] Verify editor, preview, thumbnail, StaticMesh, and SplineMesh share the same
  accepted program and requirements without local rules or shader forks.

Completion: every selected expression/configuration can be authored, diagnosed,
saved, cooked, reloaded, and rendered through existing workflows.

### Stage 6: Qualify and Publish the Contract

Dependency: Stages 1-5. Outcome: M12 has durable evidence/documentation.

- [ ] Follow repository build/test workflows to cover material expression,
  compiler, function, graph, editor, persistence, variant, lifecycle, Cook,
  runtime, StaticMesh, and SplineMesh owners selected by affected analysis.
- [ ] Run available GPU qualification for Forward, GBuffer, masked shadow,
  preview, and thumbnail. Record backend/device, spatial fixtures, pass-local view,
  resize, scale, spline parity, reload, recovery, and unavailable gates honestly.
- [ ] Complete the shared Engine/Renderer `all` build and affected Sandbox and
  RoadWeaver consumers in `Durin.dworkspace`.
- [ ] Rerun frozen compiler/artifact/variant/Cook workloads. Record timings only
  on a valid quiet lane plus bytes/resources/variants; close evidence-backed gates.
- [ ] Publish lasting contracts in owning docs, reconcile the roadmap, and run
  changed-document, all-plan, and all-roadmap validation.

Completion: the language is deterministic, bounded, diagnosable, source-free in
Game, and qualified in existing passes; WPO, depth, new outputs, and passes remain
future work.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Semantics | Exact rule per opcode; no implicit stage/space conversion; deterministic source errors |
| Math | Width/broadcast, folding, domain/non-finite behavior, semantics and identity |
| Context | Active view, bounds center, vertex normal, scale, tangent handedness and transforms |
| Functions | Constraints, polymorphic inputs, inferred outputs, nesting, defaults and common checker |
| Static selection | GUID/default/override, lazy pruning, quality/feature fallback, identity and bounded inventory |
| Renderer | StaticMesh/SplineMesh, Forward/GBuffer/masked shadow, opaque-shadow freedom, preview/thumbnail and recovery |
| Persistence/Cook | Version/migration, package round trip, DMAT, source-free load and incompatible rejection |
| Editor | Catalog/commands, pin semantics, payload editing, Undo/Redo, preview and diagnostics |
| Scalability | Compiler/artifact sizes, resources, loaded variants and Cook growth for bounded fixtures |

## Coordination and References

- Follow [Build and Run](../Agents/BuildAndRun.md), [Testing](../Agents/Testing.md),
  and [Documentation](../Agents/Documentation.md) workflows.
- [Material System](../Runtime/Rendering/MaterialSystem.md) owns lasting asset,
  compiler, variant, Cook, and renderer contracts.
- [Material Expression Building](../Runtime/Rendering/MaterialExpressionBuilding.md)
  owns lasting emitter, MIR, function, and semantic rules.
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
  and [Material Editor Lifecycle](../Editor/Architecture/MaterialEditorLifecycle.md)
  own editor behavior.
- Coordinate vertex-factory/pass bindings with
  [Geometry Submission Refactor](GeometrySubmissionRefactor.md); do not add a
  second geometry submission or shader-binding path.
