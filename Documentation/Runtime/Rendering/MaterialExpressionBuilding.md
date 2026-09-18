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

Every authored expression is checked, including disconnected nodes and unused
function calls. Material output terminals are handled by surface finalization and
cannot be consumed as expression sources. Builds run on the owning thread and
must finish before a graph edit or asynchronous dispatch.

## Validation and publication

Local `ValidateSurface` and `ValidateFunction` use the same node emission contract
with private opaque function values, allowing unavailable callee bodies during
authoring. Those values cannot become compiler snapshots. Authoring fingerprints
are transient edit checkpoints, not persisted shader identities.

`Finish` and `FinishSurface` publish detached results and consume the build session.
On failure they clear IR, roots, parameters, source mappings, and dependencies,
while retaining typed diagnostics. Duplicate output registration, missing output
registration, invalid IR references, and invalid connection selectors are errors.
See [Material diagnostics](MaterialDiagnostics.md) for error and location contracts.

The expression tests in `MaterialCompilerTests` cover multi-output sharing,
function invocation isolation, stable output selectors, normal texture handling,
cycle detection, and failed-result publication.
