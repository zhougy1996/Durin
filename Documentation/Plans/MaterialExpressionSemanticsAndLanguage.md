# Material Expression Semantics and Language Plan

Summary: Add stage- and space-aware material values, complete the bounded math and context expression set, and introduce compile-time selectors without adding a new material pass or Surface output.

Last reviewed: 2026-09-30

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

Stages 0 through 2 completed on 2026-09-30. The frozen contract below records the semantic
algebra, pass-context ABI, static-selection ownership, compatibility policy,
fixtures, and budgets. Existing material and function content now compiles through
semantic MIR, and the frozen pure-math/geometric operation set is authorable,
persistent, foldable, and deterministic without changing the Surface or renderer
ABI. Stage 3 is current: add context sources and authored spatial transforms.

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

## Stage 0 Frozen Contract

### Inventory and ownership

The existing authored language contains opcodes 0-45. Value shapes are `Float`,
`Float2`, `Float3`, `Float4`, `Texture2D`, and `Surface`; opcode 3 and retired
channel opcodes 25-27 remain unavailable and must not be reused. New opcodes are
appended in compatibility groups. Stage 1 reserves Transform Position, Transform
Direction, and Transform Normal as opcodes 46-48 so semantics can be validated
before context lowering is exposed. Stage 2 then appends Dot, Cross, Length,
Distance, Pow, Sqrt, Exp, Log, Floor, Ceil, Round, Frac, Fmod, Step,
SmoothStep, Sign, and Reflect. Stage 3 appends CameraPosition, CameraVector,
VertexNormal, ObjectPosition, ScreenPosition, and ViewSize. Stage 4 appends
Static Bool, Static Switch, Quality Switch, and Feature Level Switch. Static Bool is an
authoring/compiler-only type and cannot survive into normalized MIR.

The eight ordered Surface inputs remain BaseColor `Float3`, Normal `Float3`,
Metallic `Float`, Roughness `Float`, AmbientOcclusion `Float`, Emissive
`Float3`, Opacity `Float`, and OpacityMask `Float`. All are Pixel roots. Normal
is exact Tangent Normal; root-owned literal defaults are typed by their root,
while an ordinary authored `Float3` constant remains non-spatial.

Existing function inputs support None, Numeric, Texture, Surface, another
Input, and UV0 defaults. Ports are keyed by GUID and shape. Stage 1 adds the
constraint and stage mask to that same port; it does not add an opaque call
node. Existing graph catalog entries, graph commands, clipboard records, and
expression classes cover every old opcode except compiler-only lowering nodes.
Stage 2-4 entries use the same catalog and command path.

Compiler input currently owns detached MIR, numeric/resource declarations,
collections, static render properties, source records, and an environment made
of compiler identity, target, pass-contract version, dependencies, and resource
limits. The selected quality, accepted RHI feature level, and sorted effective
static-bool configuration become environment/request fields, not globals.

Generated material shaders retain `FragmentMain`, `GeometryFragmentMain`,
`ShadowFragmentMain`, and `HitProxyFragmentMain`. StaticMesh and SplineMesh both
use `VertexMain` and the shared `VSOutput` containing clip position, color,
world position, world normal, world tangent plus handedness, and UV0-UV3. The
only current material-view payload is set 0/binding 0, one `float4` carrying
time, a reserved value, the lighting enable, and the Specular AA enable.
Per-primitive set 1/binding 0 currently contains LocalToClip, LocalToWorld,
NormalToWorld, and determinant sign, but is vertex-only. Fragment-visible
camera, viewport, inverse view, inverse object, and bounds-center fields are
missing. Forward uses lighting/environment/shadow resources; GBuffer omits
them; masked shadow admits only material/view resources reachable by the mask;
opaque shadow remains the fixed resource-free shader. Preview and thumbnail
render through ordinary `FSceneView` and StaticMesh preparation.

Tracked authored corpus: `/Engine/Materials/DefaultMaterial`, functions
`SampleNormal`, `SampleORM`, and `UVTransform`, plus
`/Game/Materials/Metal/M_MetalRust`. The four Metal texture assets and their
bulk payloads are dependency fixtures, not expression graphs. `Durin.dworkspace`
declares Engine, Sandbox, and RoadWeaver. Engine owns all C++ API consumers;
Sandbox consumes the tracked material and Cook/runtime path; RoadWeaver has no
direct material API use but remains an affected workspace build/Cook consumer.

### Value and operator rules

`S` below is the intersection of the operation mask and every operand mask.
Empty `S` is an error at the consuming input. `N` means non-spatial. `P`, `D`,
and `M` mean Position, Direction, and Normal in one exact Object, World, View,
or Tangent space. `C` means ScreenCoordinate in Screen space. Unless a row says
otherwise, scalar broadcast follows the existing numeric rule, all numeric
operands must have the selected result width after broadcast, stages intersect,
and any unlisted spatial combination is rejected rather than cleared.

| Operation | Shape and stage | Semantic result and rejection rule |
| --- | --- | --- |
| Constant, numeric Parameter, Collection Parameter | Authored numeric shape; Both | `N`. Constants and parameters never acquire a space implicitly. |
| Texture Parameter | `Texture2D`; Both | Resource, no spatial kind/space. |
| Time | `Float`; Both | `N`, material-view time. |
| Texture Coordinates, UV Channel | `Float2`; Both | `N`; UV channel selector is scalar `N`. |
| Texture Sample | texture + `Float2 N`; Pixel | Sample channels are `N`, except the decoded RGB output of a Normal-usage sample is Tangent `M`. Resource/sample outputs share one fetch. |
| Decode Normal RG | `Float2 N` to `Float3`; Pixel | Tangent `M`; clamp/reconstruct/normalize exactly once. |
| Blend Normals RNM | two `Float3` Tangent `M`; Pixel | Tangent `M`; any other kind/space is rejected. |
| Add | equal numeric width; Both | `N+N -> N`; `P+D` or `D+P -> P`; same-space `D+D -> D`; `C+N2` or `N2+C -> C`. Other spatial pairs reject. |
| Subtract | equal numeric width; Both | `N-N -> N`; same-space `P-P -> D`; `P-D -> P`; same-space `D-D -> D`; `C-C -> Float2 N`; `C-N2 -> C`. Other spatial pairs reject. |
| Multiply, Divide | numeric; Both | `N` combinations use component rules. A spatial value may combine only with one scalar `N`, preserving its kind/space; division requires spatial/scalar order. Spatial-spatial and scalar/spatial division reject. |
| Minimum, Maximum, OneMinus, Absolute, Saturate, Clamp | numeric; Both | `N` only. Component-wise spatial use is ambiguous and rejected. |
| Negate | numeric; Both | `N -> N`, `D -> D`, `M -> M`; Position and ScreenCoordinate reject. |
| Normalize | vector; Both | `N -> N`, `D -> D`, `M -> M`; Position and ScreenCoordinate reject. Zero length deterministically returns the zero vector and never produces NaN. |
| Lerp | equal A/B shape plus scalar `N` alpha; Both | `N/N -> N`; equal kind and exact space for `P`, `D`, `M`, or `C` preserves that semantic. Mixed semantics reject; no implicit post-normalization. |
| Make Float, Append, Splat | existing widths; Both | Inputs must be `N`; output is `N`. These nodes never manufacture spatial meaning. |
| Swizzle | numeric to selected width; Both | `N -> N`. Exact identity `xyz` on `P/D/M` and `xy` on `C` preserves semantics; every other spatial mask clears to `N` only when its result is consumed as non-spatial, and cannot feed an exact spatial port without an explicit semantic producer. |
| Sine, Cosine, Pow, Sqrt, Exp, Log, Floor, Ceil, Round, Frac, Fmod, Step, SmoothStep, Sign | numeric; Both | `N` only. Pow/Fmod/Step/SmoothStep use scalar broadcast. Constant folding uses IEEE float operations; invalid domain or non-finite folded output is a source error, while dynamic shader evaluation follows Slang/IEEE behavior. Fmod with folded zero divisor is an error. |
| Dot | equal vector width 2-4; Both | `N/N -> Float N`; equal-space combinations of `D` and `M` are allowed and return `Float N`. Position and ScreenCoordinate reject. |
| Cross | `Float3`; Both | `N/N -> Float3 N`; equal-space `D/M` operands return `D` in that space. Position rejects. |
| Length | vector width 2-4; Both | `N`, `D`, or `M` to `Float N`; Position and ScreenCoordinate reject. |
| Distance | equal vector width; Both | `N/N -> Float N`; same-space `P/P -> Float N`; `C/C -> Float N`. Other spatial pairs reject. |
| Reflect | equal vector width; Both | `N/N -> N`; incident `D` and normal `M` in the same space return `D`. Other spatial pairs reject. Zero normal follows the intrinsic result and remains finite when inputs are finite. |
| Make/Get/Set Surface | fixed attributes; Pixel | BaseColor/Emissive and scalar attributes are `N`; Normal is Tangent `M`. Get preserves the selected attribute semantic; Set validates it. Surface itself has no kind/space. |
| Function Input/Output/Call | declared shape and stage constraint | Unconstrained numeric input adopts the caller semantic per invocation; non-spatial and exact constraints validate; output is inferred then checked. Defaults are checked as if authored at the call site. |

WorldPosition is Pixel World `P`; CameraPosition is Both World `P`;
CameraVector is Pixel World `D`; ObjectPosition is Both World `P`;
VertexNormal is Vertex World `M`; ScreenPosition is Pixel Screen `C`; and
ViewSize is Both `Float2 N`. Surface roots request Pixel. A `Both` result is
evaluated in the requested root stage and never implies hoisting or a varying.

### Transform matrix and geometry contract

Transform Position, Direction, and Normal require `Float3`, an exact input kind,
and an authored source space equal to the input semantic. Source and destination
must differ and are canonical payload bytes. Screen is never accepted. All 12
ordered pairs among Object, World, View, and Tangent are supported for Direction
and Normal. Position supports Object/World/View pairs; Tangent Position is
rejected because a tangent frame has no stable origin.

Object/World uses the primitive LocalToWorld and its validated inverse;
World/View uses the active view matrices; Object/View composes them. Position
uses homogeneous `w=1` and translation. Direction uses the linear `w=0` map.
Normal uses the inverse transpose of the corresponding direction map and is
normalized with the zero-vector rule above. Tangent conversions use the
post-vertex-factory world normal and tangent, reconstruct bitangent as
`handedness * cross(normal, tangent)`, orthonormalize the frame, and use its
transpose for the inverse. A non-finite or singular required object/view matrix,
or a degenerate required tangent frame, rejects preparation transactionally and
keeps the current last-known-good/error fallback; it never substitutes identity.

For StaticMesh, Object means the vertex-factory local result before the primitive
LocalToWorld. For SplineMesh it means the spline-deformed local result and
deformed normal/tangent before LocalToWorld. Thus both factories publish the
same post-factory world basis; non-uniform spline scale and primitive scale use
their existing inverse-scale normal path and determinant/tangent handedness.

### Pass-context ABI

Set 0/binding 0 becomes a versioned `MaterialView` uniform with time/flags,
camera world position, viewport origin/size and reciprocal size, WorldToView,
and ViewToWorld. Set 1/binding 0 becomes one cross-stage `MaterialPrimitive`
uniform extending the existing transform data with WorldToLocal and world-space
bounds center. Reflection admits either buffer only when reachable. Vertex
shaders and fragment shaders use identical declarations; no duplicate binding
path is introduced.

Forward and GBuffer use their active `FSceneView`. Preview and thumbnail use the
view created by their own viewport. Masked shadow uses each shadow caster
`FSceneView`, including its camera/view matrices and shadow-map viewport; opaque
shadow remains resource-free because it never evaluates the material. Hit proxy
uses its active editor view. ScreenPosition is
`(SV_Position.xy - viewportOrigin) * reciprocalViewportSize`; ViewSize is the
active viewport width/height, not the backing texture extent. CameraVector is
the safe-normalized vector from WorldPosition to the active view camera.

The current time and material lighting/Specular-AA flags are reused. Existing
VSOutput world position, world normal, world tangent/handedness, and UVs are
reused. Camera position, viewport facts, view transforms, inverse object
transform, and bounds center are new fields. Pass-contract validation requires
exact names, bindings, stage visibility, sizes, and reachability; mismatch
rejects the candidate program without partially publishing resources.

### Static selection and variant policy

A base material owns at most 32 static-bool declarations `{Guid, Name,
DefaultValue}` with unique valid GUIDs and names. Authored instances own sorted
`{Guid, Value}` overrides; orphan overrides persist for repair but are rejected
from compilation. Dynamic instances expose no setter and cannot carry local
static overrides. Inheritance resolves root declaration defaults followed by
parent-to-child overrides. The effective sorted GUID/value list is the canonical
configuration and compile-request key.

Static Bool is an authoring-only value referencing one root declaration. Static
Switch has False and True value inputs of one shape; Quality Switch has Default,
Low, and High; Feature Level Switch has Default, ES3_1, SM5, and SM6. All
authored branches must be structurally valid and type-compatible, but only the
selected reachable branch is expanded, normalized, dependency-scanned, limited,
generated, and encoded. A missing exact Quality/Feature branch selects Default;
a missing selected Static Switch branch is an error. Static Bool never enters a
uniform layout, parameter collection, dynamic parameter API, or generated code.

Editor preview explicitly requests Low or High and one feature level no greater
than the initialized RHI capability. Game quality is renderer/scalability state;
feature level is derived from immutable `FRHICapabilities::FeatureLevel` and
cannot be authored by an instance. An unsupported requested tier is rejected.
Game performs exact lookup only and never compiles or falls back to another
configuration when the program is missing. Default branches are compile-time
expression fallbacks, not runtime variant fallbacks.

Cook requests the root default and every authored instance's effective static
configuration for both Low and High and for every feature tier supported by the
target profile. It deduplicates identical normalized programs and never creates
unreferenced static-bool combinations. Each DMAT record stores the exact
quality, feature level, effective static configuration, versions, identity, and
compiled stages. Missing or incompatible Game configurations fail Cook/load.

### Compatibility and version selection

| Contract | Current | M12 | Policy |
| --- | ---: | ---: | --- |
| Material graph custom version | 4 | 5 | Bounded v4-to-v5 load migration, then canonical resave; older/missing versions reject. |
| Material output custom version | 3 | 3 | Unchanged eight-output representation. |
| Function-port custom version | 1 | 2 | Bounded v1-to-v2 constraints derived from the frozen legacy table; then resave. |
| Material-instance custom version | 1 | 2 | Adds static-bool overrides; v1 migrates to an empty override set. |
| Graph/function presentation schemas | 2 / 1 | 2 / 1 | Presentation is unchanged. |
| MIR | 5 | 6 | Semantic value and transform/selection payload encoding; no old MIR reinterpretation. |
| Generator | 8 | 9 | New math/context/transform source and exact reflection. |
| Compiler envelope | 9 | 10 | Adds quality, feature, static configuration, and requirements. |
| Pass contract | 5 | 6 | Versioned MaterialView/MaterialPrimitive bindings. |
| Material render layout | 4 | 4 | Parameter packing is unchanged; context is pass-owned and not a material field. |
| DMAT payload | 8 | 9 | Exact configuration and requirements; old payloads reject. |

Legacy opcodes migrate to the table above. Existing normal-usage texture RGB
lowering remains the explicit Tangent Normal producer; tracked function ports
are resaved with inferred exact constraints where their body requires them and
otherwise remain unconstrained. Any legacy generic numeric link that would
acquire spatial meaning only from its destination is rejected with a resave
diagnostic and must be replaced by a semantic source/transform. No loader
silently labels a generic `Float3` as spatial.

### Baselines, fixtures, and budgets

The 2026-09-30 `MacOS-arm64-Debug-DurinEditor` CPU qualification passed with no
failures. The synthetic PBR fixture normalized 260 authored IR nodes to 167,
including six samples: canonical 13,529 bytes, generated source 14,075 bytes,
source hash `4d1085e8a903ab942bb06ac7ca663f2e`, program identity
`ec6ea07b78072a22596b13c36489bd46`, one compiler dependency, 138,244 compiled
SPIR-V bytes, and 142,260 DMAT bytes. Diagnostic timings were normalize 2,155
us, generate 875 us, cold compile 225,680 us, and warm compile 408 us. The
variant fixture produced eight compatible owners, four effective identities,
13 instance requests, four retained programs/731,509 bytes, 1,173,978 aggregate
instance DMAT bytes, and 142,260 root DMAT bytes. Timing came from a shared CPU
lane and is diagnostic, not a regression claim.

Deterministic coverage is owned by `MaterialCompilerTests` (identity/source/code
and reflection), `MaterialFunctionTests` (expansion/defaults/nesting),
`MaterialRuntimeTests` (variants, renderer publication, reload/recovery),
`MaterialEditorInteractionTests` (preview), `MaterialThumbnailTests`,
`MaterialCookTests`, `MaterialCompileLifecycleTests`, and
`MaterialQualificationTests`. The qualification command is
`./DevTool test MaterialQualificationTests --mode qualification --report`; all
other gates use their registered target or `./DevTool test affected` as changes
select them. GPU evidence remains a Stage 6 gate and follows the repository GPU
qualification workflow.

Existing hard bounds remain: 256 authored nodes, 1,024 authored links, 4,096
expanded nodes, 16,384 expanded links, 64 graph depth, 16 function-call depth,
128 parameters, 64 function dependencies, 32 static-bool declarations, 1 MiB
canonical IR/generated source, 2 MiB compile request, 8 MiB compile result, 8
MiB per DMAT, 128 retained programs, and 256 MiB retained-program storage. The
fixed synthetic M12 qualification fixture must stay at or below 256 KiB
canonical bytes, 256 KiB generated source, 2 MiB total compiled shader bytes,
and 2 MiB root DMAT. Its warm normalization and generation are each bounded to
25 ms in the Debug qualification profile; cold external compiler time is
recorded but has no cross-host threshold. Selector fixtures for 1/4/8
declarations may compile only the explicitly requested owner x quality x
supported-feature configurations, must deduplicate identical normalized
results, and must remain within the existing resident count/byte and per-DMAT
bounds.

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

- [x] Inventory every opcode, Surface input, function port/default, compiler
  environment, generated shader entry, vertex-factory input, pass context,
  tracked asset, editor node, Cook version, and workspace consumer.
- [x] Publish the complete shape/stage/kind/space operator table for every old and
  new operation, including preservation/clearing and explicit rejection rules.
- [x] Freeze supported transform pairs, translation, inverse-transpose normal,
  tangent handedness, non-uniform scale, spline semantics, and singular failure.
- [x] Freeze active-view ABI and exact Forward/GBuffer/masked-shadow/preview/
  thumbnail meanings. Record reused and missing shader/binding fields.
- [x] Freeze static-bool persistence/overrides, quality ownership and runtime
  selection, feature derivation, keys, fallback, Cook inventory, missing-variant
  behavior, and the 32-declaration bound.
- [x] Choose graph/function migration and exact graph, function, MIR, generator,
  compiler-envelope, pass-contract, layout-if-needed, and DMAT versions. Inventory
  the tracked asset corpus.
- [x] Record deterministic baselines for graph identity/source/code, function
  expansion, variants, existing passes, preview/thumbnail, Cook, reload, and
  recovery. Freeze compile, artifact, variant, and Cook budgets before Stage 2;
  timing claims require a valid qualification lane.

Completion: one reviewed table determines every result/error; pass and variant
owners, migration, fixtures, commands, and budgets are recorded in this plan.

### Stage 1: Establish Stage- and Space-Aware Values

Dependency: Stage 0. Outcome: existing materials compile through semantic MIR
before the language grows.

- [x] Introduce the detached value contract and migrate emitter values, MIR,
  signatures, validation, normalization, encoding, sources, and diagnostics.
- [x] Migrate every existing opcode and eight Pixel Surface roots. Preserve valid
  tracked output and explicitly migrate formerly ambiguous links.
- [x] Add canonical position/direction/normal transform IR and deterministic
  invalid-pair diagnostics without exposing screen reconstruction/interpolation.
- [x] Extend function port contracts and invocation substitution; prove nested
  calls, defaults, independent calls, dependencies, and call-path diagnostics.
- [x] Apply selected version/migration policy; incompatible authored/cooked data
  must fail or migrate explicitly, never reinterpret silently.
- [x] Add editor pin/tooltips and diagnostics for stage, kind, space, transforms,
  and call paths without editor-owned semantic logic.

Completion: all existing accepted content uses semantic MIR; invalid stage/space
graphs fail before shader compilation; no Surface ABI or rendered result changes.

Evidence (2026-09-30): graph/function writers advanced to 5/2 with bounded 4/1
read migration, MIR/generator/envelope advanced to 6/9/10, and all five tracked
graph assets were canonically resaved. Focused compiler, function, graph editing,
editor interaction, persistence, package, Cook, runtime, and lifecycle suites pass.
The transform IR is canonical and validated but remains unavailable in the
authoring catalog until Stage 3 supplies its pass-context lowering.

### Stage 2: Add Pure Math and Geometric Operations

Dependency: Stage 1 and frozen budgets. Outcome: general expression language
grows without new renderer inputs.

- [x] Add Dot, Cross, Length, Distance, and Reflect with exact width, same-space,
  kind, zero-length, and result-semantics rules.
- [x] Add Pow, Sqrt, Exp, Log, Floor, Ceil, Round, Frac, Fmod, Step, SmoothStep,
  and Sign with exact broadcast, width, folding, and non-finite/domain behavior.
- [x] Expose nodes through catalog, command API, canvas, copy/paste, Undo/Redo,
  persistence, automation, and diagnostic navigation.
- [x] Add root/nested-function fixtures for valid inference, rejected semantic
  mismatches, disconnected validation, identity, source location, and generated code.
- [x] Compare frozen compiler/artifact fixtures and resolve regressions before
  renderer context; do not infer GPU cost from source lines.

Completion: selected math works identically in materials/functions with
deterministic identity and no renderer/pass ABI change.

Evidence (2026-09-30): all 17 selected operations have reflected expressions,
catalog entries, semantic signatures, deterministic Slang emission, constant
folding, and persistence coverage. Root and nested-function tests exercise valid
and rejected stage/kind/space/width combinations, source locations, and generated
intrinsics. `MaterialCompilerTests` (58), `MaterialFunctionTests` (29),
`MaterialGraphEditingTests` (99), and `MaterialPackageTests` (6) pass. The frozen
`MaterialQualificationTests` compiler/artifact/variant/Cook workload also passes
in 99.70 seconds on the shared Debug lane; that time is diagnostic only. No pass
contract, render layout, material output, or renderer binding changed.

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
