# Material Expression Building

Summary: Material expressions register outputs through an emitter; graph builders own traversal, invocation caches, validation, and detached result publication.
Modules: Engine

## Header ownership

`MaterialExpressionIO.h` declares graph connections, numeric defaults, attribute
bindings, and material output types. `MaterialExpression.h` declares the expression
base and reflected collection. Small related nodes share focused family headers,
including `MaterialExpressionArithmetic.h`, `MaterialExpressionVector.h`, and
`MaterialExpressionScene.h`. Parameter bases, function calls, and the material
output retain dedicated headers. Each reflected header owns its corresponding
generated files; node-specific auxiliary records remain with their owning family.

`Material.h` and `MaterialFunction.h` include the expression foundation rather
than the concrete node inventory. Sources that create, cast, or access a concrete
node include its declaring header directly. Reflection type names, authored
properties, and graph ownership do not depend on this header partition.

## Expression emission

The material intermediate layer lives in `Durin::MIR`. `FModule`, `FNode`,
`FPayload`, and `FSwizzle` describe detached IR; `FValue` represents an emitted
index or a function texture default. `FEmitter` and `FGraphBuilder` construct it,
and `FCompilerInput` carries the detached compiler contract. IR operations use
`MIR::BuildGraph`, `MIR::Validate`, `MIR::Normalize`, `MIR::EncodeCanonical`, and
`MIR::Compile`. Material assets, expression objects, and runtime material
interfaces remain in `Durin`.

Source generation accepts property-root MIR after aggregate lowering by
`MIR::Normalize`. Make/Get/Set Attributes remain authoring constructs; an
aggregate root or residual Make Attributes node is rejected with an actionable
diagnostic and no partial generated source. There is no separate fixed-eight
aggregate evaluator in the source generator.

`DMaterialExpression::Build(MIR::FEmitter&) const` builds a node and registers
each output with `Output(int32, Value)`. Function calls look up each callee
terminal by port GUID and register its value at the caller's binding index. The virtual interface
has no default arguments or output-selection state. An emitter belongs to one
Build invocation and must not escape it.

Numeric nodes register output zero. Texture sampling nodes emit a single sample
and register its RGBA, RGB, and scalar channels; sample parameters also register
the texture resource at index seven. Retired index six remains invalid. Normal
texture RGB outputs decode RG normal data; RGBA and scalar outputs remain raw.
Texture2D sample expressions select `EMaterialTextureReadMode`: Implicit,
Level, Bias or Grad. MIR records the selection in `FTextureRead`; resource and
Float2 coordinates precede the selected scalar LOD/Bias or Float2 DDX/DDY
operands. Level supports Vertex and Pixel; Implicit, Bias and Grad are Pixel-only.
Every selected operand participates in stage intersection and type validation.
Unused optional operands retain authoring state but are absent from detached MIR
and its resource closure. The generated Shader uses Sample, SampleLevel,
SampleBias or SampleGrad with the existing material-owned sampler. Normal RGB
decoding retains its existing Pixel/Tangent semantics; raw sampled channels may
feed vertex displacement through Level. Editor mode changes use the shared
ReplaceExpression transaction and preserve inactive defaults and wiring.

Surface extraction registers every enabled attribute. Function calls register
all declared GUID outputs after validating their bindings and building the body.
Collection expressions resolve one numeric declaration from a referenced
`DMaterialParameterCollection` and emit its typed collection-read node. Expansion
through a function uses the same invocation boundary and contributes that
collection to the caller's closure.

Every numeric MIR value also carries its legal evaluation-stage mask, spatial
kind, and coordinate space. Existing operations resolve those semantics through
one signature table before a node is published. Constants and parameters are
non-spatial; normal-texture RGB is an explicit Pixel/Tangent/Normal producer;
Surface attributes require Pixel values and Normal requires the exact Tangent/Normal
contract. The WPO attribute requires Vertex values; explicit interpolators
bridge vertex inputs to pixel consumers. See [vertex evaluation](MaterialVertexEvaluation.md). Scalar broadcast is represented explicitly and records which operand
was broadcast so canonical validation cannot confuse it with vector arithmetic.
Position, direction, and normal transform nodes carry canonical source and
destination spaces. Stage 1 validates their supported pairs and canonical bytes;
shader context lowering and authored transform nodes are introduced with the
context-source stage.

The pure numeric language includes `Pow`, `Sqrt`, `Exp`, `Log`, `Floor`, `Ceil`,
`Round`, `Frac`, `Fmod`, `Step`, `SmoothStep`, and `Sign`. Scalar operands may
broadcast to the selected vector width; different vector widths do not coerce.
`Dot`, `Cross`, `Length`, `Distance`, and `Reflect` apply the semantic signature
table: geometric vector operands have exact widths and compatible kinds/spaces,
while ordinary non-spatial vectors retain non-spatial results. `Dot`, `Length`,
and `Distance` produce scalar values; `Cross` produces a direction for compatible
direction/normal inputs, and `Reflect` preserves an incident direction. These
nodes add no renderer requirement or pass binding.

Material World positions use [translated world space](../Scene/CameraRelativeRendering.md#material-positions).
Context expressions carry exact stage and space semantics. `WorldPosition` is a
Both/World/Position value; `CameraPosition` and `ObjectPosition` are
Both/World/Position; `CameraVector` is a normalized Pixel/World/Direction;
`VertexNormal` is Vertex/World/Normal after vertex-factory deformation;
`ScreenPosition` is normalized viewport Float2 and `ViewSize` is the active
viewport Float2. `Time` and `ViewSize` are non-spatial. A Pixel root cannot
consume `VertexNormal` directly, including through a function call. An explicit
`VertexInterpolator` retains its World/Normal meaning and produces a Pixel value.
Vertex WorldPosition is pre-WPO; pixel WorldPosition is displaced and interpolated.

Transform Position accepts exact Float3 Position values between Object, World,
and View. Transform Direction and Transform Normal accept exact Float3 values
for every ordered Object, World, View, and Tangent pair. Source and destination
must differ and must match the input semantics; there is no implicit conversion.
Positions use homogeneous point transforms, directions use homogeneous vector
transforms, and normals use inverse-transpose transforms followed by finite,
zero-safe normalization. Tangent conversions use the post-vertex-factory normal,
orthonormalized tangent, and handed bitangent supplied by the active mesh pass.

Compilation folds finite constant-only math and geometry subgraphs before
normalization. A domain error, zero `Fmod` divisor, overflow, or non-finite folded
component is a source-located compiler error; it never publishes partial IR.
Zero-length `Length` and a zero normal in `Reflect` remain finite and deterministic.
Authoring validation still retains disconnected and domain-invalid literal state
so the editor can present and repair it; compiler snapshot construction owns the
fold diagnostic.

Static selection is resolved by the graph builder before normalized IR and
resource discovery. `DMaterialExpressionStaticBool` names one root declaration
by stable GUID; base defaults and the compiling instance's sorted overrides form
the effective compiler environment. `StaticSwitch` requests only its selected
branch; a disconnected False or True branch resolves through its retained
numeric value. `QualitySwitch` selects Low or High and `FeatureLevelSwitch` selects
ES3_1, SM5, or SM6, with each node's Default input used when its exact input is
absent. Static bool values never become MIR nodes, uniform fields, or dynamic
instance values.

Local authoring validation checks selector payloads and the structure of every
authored input, including inactive branches. Compiler traversal starts from the
active Surface roots and follows only the selected branch, so inactive or
disconnected expressions do not contribute expanded-node limits, parameters,
resources, function dependencies, canonical bytes, or generated code. The
quality, feature level, and sorted effective static-bool set are explicit
snapshot inputs; equivalent selected normalized programs may reuse the same
compiled result.

## Class-associated authoring descriptions

`MaterialExpressionDescription.h` exposes immutable Engine class adapters through
`GetMaterialExpressionDescriptions()` and `FindMaterialExpressionDescription()`.
Each concrete reflected class has one description with its name, aliases,
semantic operation, authored input labels,
supported shapes, and fixed/adaptive/instance output behavior. The adapters and
class lookup are initialized once after reflection initialization, borrow class
pointers, and construct no expression or asset objects. Descriptions are runtime,
nonserialized metadata; they contain no MaterialEditor policy and never enter
compiler worker inputs.

Authored shapes reuse shared value/type rules during initialization, then apply
numeric broadcast and exact-normal authoring constraints. They are separate from
normalized MIR signatures: samples expose multiple indexed outputs, collections
resolve declaration types, functions use live GUID ports, and attribute GUID selections and
bindings control instance pins. Instance-dependent output behavior must be read
from authored state rather than inferred from palette availability.

`MaterialExpressionInputs.h` owns reflected connection traversal and retained
numeric-default lookup. Fixed members retain declaration order; function calls
visit binding order. MIR and editor readers borrow the material's named numeric
fields through `GetNumericInputForProperty`. Make exposes its connection-only
fields through `GetExpressionInputForProperty`. The same accessor on the material
accepts both numeric properties and `MP_MaterialAttributes`. Compiler capture
selects live inputs using the domain, effective instance properties and root mode
before resolving dependencies. Both domains use the stored packed flag. Asset validation checks retained links in both modes.
Whole-graph replacement and cross-owner duplication use owned `FMaterialRootInputs`.
Root commands stage only touched numeric fields, the packed link or the mode bit.
The read-only input description carries the connection, type, name and optional
numeric view for both live and detached root validation. MIR constructs aggregates
and selects properties through the graph builder. Make, Set, Get and Blend use
the emitter interface, which owns the dense representation and default literals.
Historical comparisons store source IDs, selectors, flags and component values.
Dynamic attribute selectors and overrides retain their attribute GUIDs. Visitors borrow live inputs for the
current owning-thread operation. Editor commands, clipboard, and layout share
this contract without putting expression references into detached compilation.

## Numeric input ownership

`FMaterialExpressionInput::Expression` references an upstream expression in the same
asset. Its signed 32-bit `OutputIndex` selects the source output. A disconnected
input has an empty expression reference. Function calls keep port GUIDs in their
own output binding table. A consumer indexes that table, so renaming a port or
changing its display order preserves the connection. The editor resolves named
pin addresses against the source at the editing boundary. This follows the
ordinary connection contract of [UE FExpressionInput](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FExpressionInput?application_version=5.8).
Durin models channel selection through expression outputs and Swizzle nodes.
UE also provides input names and per-input component masks.
Node and function port GUIDs survive graph reconstruction and duplication. Reflected
`FMaterialNumericInput` groups that `Connection` with `UseConstant` and a retained
float-array `Constant`. Numeric expressions and sampling UVs use this storage. Material roots retain their enabled bit and fixed-width scalar, linear RGB or vector components. Resources and
aggregate Surface links remain identity-only; function port defaults and material
parameter defaults retain their separate ownership.

A connection takes precedence over either flag state. At fixed-width inputs
that admit implicit conversions, `CanValidCast` defines the accepted source
types and `FEmitter::ValidCast(Code, DestType)` emits the conversion or reports
a diagnostic. Following [UE ValidCast](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FMaterialCompiler/ValidCast),
the operation preserves matching types, repeats a scalar to fill a vector,
and truncates trailing vector components. For example, a Float4 wired to
Emissive Color becomes an RGB swizzle in MIR. Expanding a Float2 or Float3
requires an explicit expression. Spatial values require an explicit component
mask for truncation, and input-specific stage and coordinate-space contracts
still apply. Width-inferred arithmetic retains its equal-vector-width rule
and scalar broadcasting. Authored connections retain their original source
and output index.

Broken or incompatible connections report diagnostics. A disconnected input
uses its constant when enabled and its definition fallback otherwise.
`GetMaterialNumericInputFallback`
owns numeric node fallbacks. Material and Make read standard property defaults
from `FMaterialAttributeDefinitionMap`. An open individual Normal root always
uses the tangent-space default (0, 0, 1), including when v6 stores an enabled
Normal constant. Make emits a disconnected Normal literal
with tangent-space normal semantics.
Set starts from the incoming aggregate, or from the same defaults as Make when
its prototype is disconnected. Only connected assignments replace attributes.
Sampling UVs inherit mesh UV0. Inputs without a fallback remain required.

Retained constants are validated even behind connections or while disabled.
Adaptive inputs retain their component widths and scalar broadcasting rules.
Canonical inactive storage is finite zero with an appropriate input width; it
does not define the fallback. Authored fingerprints include the flag and retained
components, while shader identity still uses the resolved compiler representation.

Authored graphs use graph version 10 and function-port version 6. Material roots
and Make/Set expressions write output version 6. Make and Set pins store expression connections. Set inherits the incoming attribute when its pin is open. [Material version domains](../../ObjectSystem/Versioning.md)

## Traversal and invocation state

`MIR::FGraphBuilder` exposes only build-session lifecycle and local
validation. Its private implementation owns graph admission, bounded depth-first
traversal, function resolution, diagnostics, and IR construction. The emitter
exposes input resolution and emission operations to expression implementations,
without exposing result publication or mutable traversal state.

An expression builds once per graph invocation. Output lookup happens after the
node completes; partially registered outputs cannot satisfy a recursive request.
Active-expression tracking rejects cycles across different outputs of one node.
Each recursive Build receives its own emitter, preserving the output owner while
resolving upstream dependencies.

Each function invocation has independent input bindings, expression/output caches,
and diagnostic call paths. Invocations share detached IR storage, resource usage,
function-body captures, and expanded graph budgets. Sharing a function body does
not share values computed from different caller bindings.

Every authored expression is checked for locally valid storage, including
disconnected nodes and unused function calls. Compilation semantics and expanded
bounds are evaluated only for the root-reachable, selector-chosen closure.
Surface finalization consumes the owning material’s named inputs and mode. Builds run on the owning thread and must finish
before a graph edit or asynchronous dispatch.

## Validation and publication

Normalized `MIR::Validate` rejects authored-only function, coordinate, sample
parameter, Surface accessor/override, AppendVector, and static-selection
operations before source generation, even when those nodes are unreachable.
Build must expand or prune them before detached input publication. UVChannel and
DecodeNormalRG remain legal internal operations with no authoring class.


Local `ValidateSurface` and `ValidateFunction` use the same node emission contract
with private opaque function values, allowing unavailable callee bodies during
authoring. Those values cannot become compiler snapshots. Authoring fingerprints
are transient edit checkpoints, not persisted shader identities.

`Finish` and `FinishSurface` publish detached results and consume the build session.
They set an explicit completed success flag, including for a valid empty build.
On failure they clear IR, roots, parameters, source mappings, and dependencies,
active collection layouts,
while retaining typed diagnostics. Duplicate output registration, missing output
registration, invalid IR references, and invalid connection selectors are errors.
Missing collection assets/declarations, incompatible types, malformed layouts, the
128-declaration asset limit and the four-distinct-collections program limit produce
source-located diagnostics and publish no partial result.
See [Material diagnostics](MaterialSystem.md#results-and-diagnostics) for error and location contracts.

The expression tests in `MaterialCompilerTests` cover multi-output sharing,
function invocation isolation, stable output selectors, normal texture handling,
pure-math folding and generated intrinsics, geometric semantic mismatches, cycle
detection, and failed-result publication.

## Compiler input capture

`SnapshotMaterialCompilerInput(Material, Environment)` returns one
`FMaterialCompilerSnapshotResult`. Its optional `Snapshot` owns the detached
`MIR::FCompilerInput` and the function-owner stamps used to capture it. Payload
presence is the sole success condition; default and failed results have no
payload and failures retain typed diagnostics. A failed retry cannot publish a
partial input or a mismatched dependency list. A previously captured result
remains independent of later capture attempts or owner edits.

On the owning thread, capture selects shader entries from the material domain,
the requested instance's effective static properties and the compiler environment.
PostProcess reads Emissive Color from the selected root mode. An open packed
input supplies the default attribute set, whose Emissive Color is black.
Only traversed function bodies contribute owner stamps. Each stamp records the
object key, path and revision for freshness checks on the owning thread.
Workers receive detached input data.

Asset validation traverses every stored connection in both root modes.
Authoring parameter reachability uses the owner's stored mode independently of
shader entry selection and verifies function stamps before reusing its cache.
Collection capture stores only detached asset path, collection GUID, schema,
uniform layout and default bytes; no collection object reference reaches worker
execution. Cooking continues through this compilation boundary. Collection GUID,
schema and layout participate in versioned IR, shader/cache identity and DMAT;
authored defaults and per-world values do not, so value changes can reuse the
accepted shader artifact.
