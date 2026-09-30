# Material Expression Building

Summary: Material expressions register outputs through an emitter; graph builders own traversal, invocation caches, validation, and detached result publication.
Modules: Engine

## Expression emission

The material intermediate layer lives in `Durin::MIR`. `FModule`, `FNode`,
`FPayload`, and `FSwizzle` describe detached IR; `FValue` represents an emitted
index or a function texture default. `FEmitter` and `FGraphBuilder` construct it,
and `FCompilerInput` carries the detached compiler contract. IR operations use
`MIR::BuildGraph`, `MIR::Validate`, `MIR::Normalize`, `MIR::EncodeCanonical`, and
`MIR::Compile`. Material assets, expression objects, and runtime material
interfaces remain in `Durin`.

`DMaterialExpression::Build(MIR::FEmitter&) const` builds a node,
not a requested output. It registers indexed outputs with `Output(uint8, Value)`
or stable function-port outputs with `Output(FGuid, Value)`. The virtual interface
has no default arguments or output-selection state. An emitter belongs to one
Build invocation and must not escape it.

Numeric nodes register output zero. Texture sampling nodes emit a single sample
and register its RGBA, RGB, and scalar channels; sample parameters also register
the texture resource at index seven. Retired index six remains invalid. Normal
texture RGB outputs decode RG normal data; RGBA and scalar outputs remain raw.
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
Surface roots require Pixel values and Normal requires the exact Tangent/Normal
contract. Scalar broadcast is represented explicitly and records which operand
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

Context expressions carry exact stage and space semantics. `WorldPosition` is a
Pixel/World/Position value; `CameraPosition` and `ObjectPosition` are
Both/World/Position; `CameraVector` is a normalized Pixel/World/Direction;
`VertexNormal` is Vertex/World/Normal after vertex-factory deformation;
`ScreenPosition` is normalized viewport Float2 and `ViewSize` is the active
viewport Float2. `Time` and `ViewSize` are non-spatial. A Pixel root cannot
consume `VertexNormal`, including through a function call.

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
branch. `QualitySwitch` selects Low or High and `FeatureLevelSwitch` selects
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

## Numeric input ownership

`FMaterialExpressionInput` contains only upstream identity. Reflected
`FMaterialNumericInput` groups that `Connection` with `UseConstant` and a retained
float-array `Constant`. Numeric expressions, terminal properties, sampling UVs
and individual Surface attribute overrides use this storage. Resources and
aggregate Surface links remain identity-only; function port defaults and material
parameter defaults retain their separate ownership.

A connection takes precedence over either flag state. Broken or incompatible
connections report diagnostics. A disconnected input uses its constant when
enabled, otherwise its definition fallback. `GetMaterialNumericInputFallback`
owns numeric node fallbacks; standard Surface values come from
`FMaterialSurfaceOutputs`. Sampling UVs inherit mesh UV0 and attribute overrides
inherit their base Surface value. Inputs without a fallback remain required.

Retained constants are validated even behind connections or while disabled.
Adaptive inputs retain their component widths and scalar broadcasting rules.
Canonical inactive storage is finite zero with an appropriate input width; it
does not define the fallback. Authored fingerprints include the flag and retained
components, while shader identity still uses the resolved compiler representation.

Authored packages require graph custom version 5 and function-port version 2;
terminal output version 3 is unchanged. Older, missing, and future versions
reject. The repository materials and standard functions were canonically resaved
before the former graph-4/function-1 migration window was retired, so loading
never guesses spatial semantics for legacy generic numeric values or ports.

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
Material output terminals are handled by surface finalization and cannot be
consumed as expression sources. Builds run on the owning thread and must finish
before a graph edit or asynchronous dispatch.

## Validation and publication

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

Capture runs on the owning thread, resolves instances to the material root, and
uses the requested material's effective static properties. Input data contains no
object references. Function stamps retain object keys, paths and revisions rather
than object ownership; freshness checks remain on the owning thread. Compilation
moves the captured input and stamps into its lifecycle request; reachability
uses the same capture and validates stamps before reusing its cached result.
Collection capture stores only detached asset path, collection GUID, schema,
uniform layout and default bytes; no collection object reference reaches worker
execution. Cooking continues through this compilation boundary. Collection GUID,
schema and layout participate in versioned IR, shader/cache identity and DMAT;
authored defaults and per-world values do not, so value changes can reuse the
accepted shader artifact.
