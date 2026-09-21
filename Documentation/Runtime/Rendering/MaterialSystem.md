# Material System

Summary: Define material assets, parameters, render proxies, invalidation, passes, and fallback behavior.

Modules: Engine, Renderer, RenderCore

Last reviewed: 2026-09-18

Durin's material architecture keeps declaration ownership, instance resolution,
editor presentation, and renderer consumption at explicit boundaries.

`Materials/MaterialTypes.h` is the authored/reflected parameter and static
property surface. Renderer-facing layout, compatibility, immutable
representation, builder, pipeline identity, and fallback declarations live in
`Materials/MaterialRenderTypes.h`; `MaterialRenderProxy.h` includes that narrow
surface directly. Their implementations are separated into authored schema,
compiled-layout representation/builder, and diagnostics files. The production
render boundary accepts only material-specific layout v4 data. Built-in role knowledge is confined to editor-owned standard function authoring and import binding.

## Results and diagnostics

`FMaterialOperationResult` carries `FMaterialError`; mutations preserve typed
identity/type context and reject invalid changes before publication. Engine
validators and nested adapters preserve causes; `FormatMaterialError` formats
only at UI, log, command, or required string boundaries. Opaque provider text
is bounded and never determines error classification.

Synchronous build, validation, normalization, source-generation, and compiler
results use `bSucceeded` as their sole outcome and default to failure. Check it
before consuming payloads: empty output may succeed, while retained intermediate
data does not establish success. Failed builds/encoders clear emitted output;
property resolution and cooked decoding publish only validated candidates.
Cancellation and admission remain separate lifecycle outcomes. Semantic tests
assert codes, context, and publication behavior, not formatted text.

## Parameter Domain

### Persistent and dynamic instance lifetimes

Ordinary `DMaterialInstance` construction creates an authored instance: parent,
static property overrides and typed parameter values participate in asset saving
and editor transactions. Existing instance packages retain their schema.

`DMaterialInstance::CreateDynamic(Parent, Outer, Name)` creates a transient instance
with a fixed, valid non-dynamic material parent. It returns null for invalid or
over-depth chains. A parent without an accepted program is allowed; parameter
updates require both a matching current declaration and the parent's accepted
active parameter contract. `IsDynamicInstance()` exposes this immutable lifecycle.
The existing typed setters and clear operations update only local runtime values
and render state, without package dirtiness or compilation. Static overrides,
reparenting, import metadata edits and reflected editor edits are rejected.

Dynamic instances reuse the parent's complete accepted rendering generation and
overlay compatible local numeric/texture values. Parent updates reach their stable
render proxies through the ordinary dependency query. Pending compilation retains
the parent's accepted generation; failure or a broken chain displays ErrorMaterial.
Explicit and inherited compile scheduling never submits work for a dynamic owner.
Editor and cooked runtime use the same path; no dynamic cooked payload is created.

Callers retain instances in reflected strong references such as component material
slots, or through the existing explicit GC roots. Outer provides naming context,
not a lifetime root. Reflected parent and texture storage retains those objects
while the instance lives. Package planning may inspect the normal field manifest,
but transient instances are excluded from persisted graphs. Duplication and
snapshot serialization are rejected, and authored instances cannot select a
dynamic parent. Dynamic-parent chains are not supported. This API does not provide
atomic multi-parameter batches or new allocation/upload optimizations.

### Parameter values and declarations

- `DMaterialInterface` is the common asset/component-facing contract. Persistent
  parameter identity is an `FGuid`; public human/API lookup uses
  case-insensitive `FName`.
- `FMaterialParameterValue` is a non-reflected C++ variant of scalar, Vector2,
  Vector3, Vector4, or `FMaterialTextureValue`. `GetType()` derives its type from
  the selected alternative. Checked accessors return that alternative by
  reference; factories explicitly select a new alternative. Equality ignores
  nonexistent inactive payloads. The texture alternative groups its object
  reference, sampler and fallback; `AddReferencedObjects` visits and rewrites
  only that active reference.
- Typed parameter expressions own common identity/presentation metadata and
  their concrete default. Scalar ranges and texture usage belong to their
  respective expression families. `FMaterialParameterDefinition` is a transient
  derived view, with a checked declaration type. Its pointers expire on owner
  revision changes. Parameter GUIDs are distinct from node GUIDs and names are
  unique ignoring case across distinct parameters. Multiple nodes may share one
  parameter GUID when their complete definitions agree. Disconnected references
  remain until explicitly deleted.
- `DMaterial::GetParameterDefinitions()` exposes a read-only, GUID-sorted
  projection of the graph. There is no independently authored root parameter
  table or table mutation API. Declaration derivation reads concrete parameter
  expressions directly, including during Cook; it coalesces identical definitions by
  parameter GUID and rejects conflicting shared definitions or duplicate names,
  node/parameter GUID collisions, oversized text, invalid metadata and non-finite
  active defaults. Concrete expression classes determine declaration types.
- `SetMaterialExpressions` validates and independently duplicates the candidate
  before publishing its owned collection, outputs and derived schema. Shared rename
  keeps node and parameter identities. Rebinding one node to another name uses that
  parameter or creates a fresh parameter GUID. Retyping must leave valid links; GUID/type-mismatched instance
  overrides remain inspectable orphans. Deleting an owner removes its connections
  through the graph command boundary. Rejected edits leave revisions unchanged.
- Parameter defaults and display metadata are excluded from shader identity. Default
  edits update every referencing expression and the derived declaration atomically. A transient 128-bit edit fingerprint
  records typed graph operations, connection selectors, call ports and callee handles;
  it excludes parameter defaults and presentation, and is not a persisted shader key.
  Material owners retain no universal Program or function-call table cache. Structural
  code changes invalidate compilation. Compiler snapshots build detached typed IR with GUID/type parameter
  declarations before worker enqueueing, retaining no authored texture pointers. Graph-stripped Cooked roots
  load generated parameter descriptors/defaults and permit dynamic value updates
  without recreating an authored graph.
- Parameter validation returns `FMaterialParameterValidationResult` with an
  error and offending GUID. Graph replacement and compiler snapshots return
  `FMaterialProgramValidationResult`; output snapshots are assigned only on
  success. Editor commands retain bounded diagnostics and distinguish parameter
  GUIDs from node GUIDs.
- `DMaterialInstance::SetParameterValue(Id, Value)` derives the assignment type from the value
  and rejects mismatched active declarations. Cook stores five typed logical
  parameter arrays, including declaration order and only applicable metadata.
  Render publication uses a separate selected value whose texture alternative
  owns counted RHI references; no texture objects cross into the render thread.
- `DMaterialInstance` references a parent material interface and persists separate
  scalar, vector, and texture parameter-value arrays. Each record owns its GUID
  and concrete value; all vector widths use `FMaterialVectorParameterValue` with
  `FVector4f` storage and a validated declaration-type marker. Reads restore the
  declared width; writes clear unused components and compare at float precision.
  Texture values include sampler/fallback policy.
  `GetLocalParameterValue` reads a selected value directly from those arrays;
  `VisitLocalParameterValues` enumerates GUID/value pairs without an alternate cached
  record array, and `GetLocalParameterValueCount` reports their combined size.
  Reflected storage owns texture references. Duplicate GUIDs across arrays
  fail property-edit admission and package serialization. Missing/unsupported
  `ParameterStorageVersion` (current version 2) fails loading and requires rebuilding the instance.
  Explicit equal-to-parent values remain overrides. The separately reflected
  `FMaterialPropertyOverrides` has five flags that independently select blend,
  shading, cutoff, two-sided and depth-write values. `SetPropertyOverrides`
  validates the entire edit before applying it; clearing a flag retains its
  inactive value. Dynamic resolution walks the current
  instance, its parent instances, and the root material, and reports the object
  that supplied the value. Only matching GUID/type overrides resolve; valid
  type-mismatched overrides survive load as inspectable orphans. Parent cycles
  are rejected. `ResolveMaterialProperties` iteratively resolves at most 64
  owners on GameThread and returns effective authored values, canonical shader
  values and supplying-owner handles. Missing roots, cycles and depth overflow
  fail with a bounded diagnostic.
- Parent or root-graph changes preserve overrides which are no longer reachable
  as orphans for explicit editor removal. Orphans are never resolved into
  render data, while reconnecting the same parameter GUID restores the retained
  base value and override eligibility.
- `DMaterial` owns one reflected static-property set: blend mode, shading model,
  two-sided state, depth-write policy, and masked-opacity threshold. Instances
  inherit each field through the canonical parent chain unless its local flag
  is enabled. `SetParentAndPropertyOverrides` applies a related parent/configuration
  edit atomically with one request. Authored DAST v9 packages migrate the old
  enabled snapshot through deprecated-field routes to five enabled flags, even
  for equal values; disabled snapshots leave all flags off. Normal resave emits
  only the current schema. Reflected proposals validate before mutation, and
  Undo/Redo restores both values and independent intent.
- Resolved static properties form a versioned shader-map identity (blend mode,
  shading model, and mask threshold) nested in a pipeline identity (shader map,
  two-sided state, and depth-write policy). The renderer lazily caches shader
  maps by shader identity and PSOs by the complete effective pipeline identity,
  so a pipeline-only change does not rebuild the shader map.
  `CanonicalizeMaterialShaderProperties` uses cutoff `0.333f` outside Masked,
  normalizes Masked signed zero, and excludes culling/depth from compiler input
  identity. Authored inactive cutoffs remain unchanged. IR version 4/compiler
  envelope 9 invalidate old keys; generated cutoff macros, accepted renderer
  shader keys and Cook metadata comparison use the same canonical semantics.
- Opaque sections disable blending; Masked sections additionally discard the
  saturated OpacityMask constant/texture product only when it is strictly below
  the static threshold; Translucent sections use straight-alpha blending and
  deterministic per-view back-to-front center sorting. Automatic depth writes
  are enabled for Opaque/Masked and disabled for Translucent, while explicit
  depth policy overrides that default. One-sided materials cull back faces with
  mirrored-transform winding correction; two-sided materials disable culling.

The surface contract remains metallic/roughness PBR, but shader inputs are
material-owned. The built-in template declares familiar BaseColor,
tangent-space Normal, Metallic, Roughness, Ambient Occlusion, Emissive, Opacity,
and OpacityMask values and Texture2D inputs. UV-channel, scale, offset, and
rotation are ordinary numeric declarations and expressions. Sampler and fallback
policy are typed data on each Texture2D value and preserve glTF minification,
magnification, mip filtering, independent U/V addressing, and explicit white,
black, or flat-RG-normal recovery.
Their GUIDs are permanent because serialized overrides must survive renames.
The built-in identities are maintained by one explicit role-to-parameter-group
registry. Callers use its shared role/kind lookup rather than duplicating GUID
switches, individual aliases, or parallel-array knowledge.
Engine compiles reachable declarations in GUID order into layout v4. Numeric
fields occupy zero-padded 16-byte slots after the reserved view-control slot;
Texture2D fields receive compact resource/sampler indices. Layout identity,
counts, field types, offsets, and shader reflection are accepted as one schema.
The view-control slot stores material time in x, reserves y, and stores lighting
and specular-AA flags in z/w. Forward, GBuffer and masked-shadow draws supply the
view's material time directly through this uniform; Time does not consume a
vertex-to-fragment interpolator.

`DMaterial` persists an owned `FMaterialExpressionCollection` and typed Surface
outputs. Concrete reflected expression classes own only their applicable values,
inputs and defaults; there is no authored Program, universal node, or separate call
table. GUID connections preserve node, parameter and function-port identities.
The material owns eight typed Surface outputs and an optional aggregate Surface
connection. `DMaterialInstance` owns no expressions and resolves its root through
the parent chain, independently of expression ordering.

Numeric inputs retain component arrays of length zero (absent) or one through four,
validated against the concrete pin type. Connected defaults remain stored;
disconnection restores them. TextureCoordinates retains a scalar channel default. Exposing a default creates a parameter expression and explicit
connection. Functions own their signatures and cannot own root material parameters.
Call expressions own the callee and GUID-keyed port bindings; absent optional call
defaults select signature defaults. Required inputs without a value reject.
Signature InputId aliases remain supported. Retired opcode values 3 and 30 reject.

TextureParameter owns a resource and exposes Texture2D output 0.
TextureSampleParameter2D owns a resource and optionally samples it. Both sampling
forms expose slots 0 RGBA, 1 RGB, 2 R, 3 G, 4 B, and 5 A; slot 6 is retired. Only the combined
owner also exposes slot 7 Texture2D. Node building registers all sample outputs;
normalization removes unreachable sampling and UV work when only the resource is
consumed. Multiple consumers retain independent samples
and UVs. Both sample forms accept a Float2 UV expression; a missing UV link reads
mesh UV0 directly, without local transform settings. TextureCoordinates selects
one channel through its scalar Channel input/default and outputs Float2.
UV channel rounding/clamping remains floor(channel + 0.5), clamped to 0..3;
missing mesh channels remain zero. Scaling, origin rotation in radians and offset
are explicit upstream math expressions. Import and PBR recipes preserve their UV
parameters using these operations; multiple samples can share the resulting Float2.
Synthetic expression source records retain authored node, input and call path.
These authoring forms lower before normalization and do
not change layout v4. Fresh materials own one material-output expression node
and eight unconnected property defaults:
BaseColor `(0.5, 0.5, 0.5)`, Normal `(0, 0, 1)`, Metallic `0`, Roughness
`0.5`, AmbientOcclusion `1`, Emissive `(0, 0, 0)`, Opacity `1`, and
OpacityMask `1`. Aggregate mode accepts one Surface source and requires all
eight property links to be disconnected; per-property mode requires the
aggregate source to be disconnected. Retained fallbacks survive either mode.
Material and function owners serialize the current reflected graph directly, without
a historical graph-version marker or UV compatibility branch.
`MIR::FGraphBuilder::ValidateSurface` and `ValidateFunction` check
concrete expressions directly before publication: identifiers, typed links,
cycles, bounds, defaults, parameter metadata and function terminals. Local validation
uses declared call-port types without inspecting a callee body, so a missing
dependency remains editable. Those private validation values cannot escape as a
compiler snapshot. Snapshot construction separately validates and expands the loaded
function closure. Expressions register all outputs through a call-local emitter;
the builder owns traversal and caches values separately for each function invocation.
See [Material expression building](MaterialExpressionBuilding.md) for the emission,
invocation, and result-publication contracts. Unconnected outputs use finite typed fallbacks. Duplication
creates independently owned expression children while preserving their GUIDs;
presentation names round trip without affecting rendering semantics.

Base materials also persist bounded `EditorOnly` graph presentation containing
one integral position per live node GUID and an optional integral position for
the derived Surface terminal. Presentation schema 2 sanitizes both
domains against live typed-expression GUIDs without constructing a program projection,
and never enters semantic validation, normalized
IR, compile snapshots, shader identity, derived data, or Cook. MaterialEditor's
shared inspection, command, canvas, clipboard, transaction, and
diagnostic-navigation boundary is defined by
[Material Graph Operations](../../Editor/Architecture/MaterialGraphOperations.md)
and [Material Graph Canvas](../../Editor/Architecture/MaterialGraphCanvas.md).

The persisted program is authored state, not a render artifact. GameThread can
snapshot it, parameter declarations, code-affecting static properties, target,
compiler identity, and virtual dependency fingerprints into a detached value
request. Normalization starts only from connected Surface inputs,
removes dead and presentation-only state, canonicalizes
commutative inputs and numeric bytes, and produces versioned typed IR plus a
stable digest independent of authored node order, node GUIDs, and dynamic
parameter/resource values. Two-sided state and depth policy remain pipeline-
only and do not enter this program digest. Normalized IR owns one special
Surface Root outside its ordinary node vector. Per-property inputs contain an
earlier exact-typed expression or an inline finite literal; aggregate mode names
one earlier Surface expression. A default material therefore has zero ordinary
IR nodes. Function calls expand into ordinary expressions before normalization; the editor
retains the compact authored calls and their stable port identities.

The default material compiler environment represents the dedicated
`/Engine/MaterialCompilerEnvironment` dependency graph with one
aggregate source-tree fingerprint. That root mirrors the generated source's
reachable Material and Lighting modules and deliberately excludes BasePass
vertex factories and pass implementation. RenderCore obtains it from the
ordinary persisted shader manifest, so a warm startup validates file metadata
without asking Slang to parse the graph or rereading source contents. Any
reachable source change rebuilds the aggregate fingerprint and therefore
selects a new material-program identity; the root virtual path is an identity
anchor, not an extra shader compilation. RenderCore memoizes that aggregate for
the current Shader reload generation, so every later material in the process
reuses it without additional file-status queries. Applying an explicit Shader
reload advances the generation and makes the next material compilation
revalidate the manifest.

The synchronous compiler lowers that IR to bounded deterministic Slang using
stable IR-index symbols and exact floating-point bit expressions. RenderCore
accepts the generated root as owned memory, resolves only allowlisted virtual
imports, retains cache/artifact ownership, compiles forward, GBuffer, and
masked-shadow fragments, and accepts only correctly typed reflected bindings
from each pass's closed allowlist. Unused material textures, samplers, and
uniform fields may therefore be optimized out; a default material contains no
material texture-sample expressions or texture-role bindings.
The complete value-owned result includes identity, IR, source, dependencies,
three compiled stages, phase timings, and bounded diagnostics; any failure
retains no publishable partial stage set.

### Final surface evaluation

The generated root calls `EvaluateMaterialSurface(FMaterialSurface)` in
`Material/SurfaceMaterial.slang` once, for both per-property and aggregate inputs.
Ordinary Surface expressions remain composable data until this final boundary.
Forward, material previews, GBuffer/depth and masked-shadow fragments use the same
generated evaluation; Cook retains its compiled stages. Opaque shadows do not need
surface evaluation. The evaluator performs no texture lookup, normal decoding,
factor composition, vertex-color multiplication or hidden parameter lookup.

Base Color is linear reflectance clamped to 0..1, matching RGBA8 GBuffer transport
for Lit and Unlit authoring. Metallic, AO, opacity and mask also clamp to 0..1;
perceptual roughness clamps to 0.045..1 before specular AA. Emissive preserves
nonnegative HDR up to the finite R11G11B10 channel maxima (65024,65024,64512).
Use Emissive for HDR output, including Unlit materials. This evaluates values
without rewriting retained literals or graph expressions.

Nonfinite expression components use root defaults: 0.5 for Base Color and
roughness, zero for metallic/emissive, and one for AO/opacity/mask. Decoded tangent
normals normalize safely; nonfinite vectors, nonfinite squared lengths and squared
lengths at most 1e-8 recover to (0,0,1). Authored nonfinite literals/defaults still
fail validation. Mask rejection remains strictly below cutoff. BRDF and specular-AA
helpers retain their independent numerical safeguards; AA bounds its newly
computed roughness and deferred lighting does not apply AA again.

### Decoded normal sample output

Both sampling forms use RGB output index 1 for Float3 values. For a Normal texture
resource, lowering decodes its RG channels into a tangent-space normal. RGBA and
scalar channels retain encoded data. The internal DecodeNormalRG IR operation and
shader helper remain compiler details; no authored decode expression exists.
Indices 6 and 8 are invalid, while the sampled parameter's Texture resource stays
at index 7. The canvas has no aliases for retired output pins.

SampleNormal interpolates from the flat normal `(0,0,1)` to the decoded RGB value
using Strength, then composes it with its Normal input through RNM's safe
normalization. Strength zero is flat and strength one uses the sampled direction.
Intermediate strengths operate in decoded vector space, replacing the former
encoded-RG scaling rule. Structural imports already bake strength into their
source images and consume RGB directly. Import recipe keys use version 4, so
parents from previous semantics are not silently reused.

## Reusable Functions and Standard Library

`DMaterialFunctionInterface` is an abstract asset contract for typed signatures,
semantic revisions and dependencies. `DMaterialFunction` owns its editable concrete
expression collection and exposes a borrowed body for owning-thread validation and
Build. Root materials own concrete expressions, including function calls with
base-typed asset references and GUID bindings; instances retain no function graph.
Function expression collections and presentation are `EditorOnly`.

Ports have persistent GUIDs, names, types, ordering, required/advanced flags and
typed defaults. Calls bind by port GUID, and multi-output links retain output
GUIDs rather than declaration indices. Reordering or renaming preserves wiring;
deleting or incompatibly changing a used port produces a diagnostic. Functions
accept Float, Float2, Float3, Float4, Texture2D and Surface. `GetSurfaceAttributes`
and `SetSurfaceAttributes` compose existing Surface fields. Functions cannot own
root parameter declarations; callers supply parameter expressions explicitly.
Texture inputs carry resource, sampler and fallback together. Optional unbound
textures use the declared white, black or flat-RG fallback; connected values keep
the caller's complete sampling policy. Surface defaults retain all eight fields.

GameThread traverses typed function bodies and emits detached IR directly. Each
invocation substitutes independent bindings, validates required inputs, rejects
recursion and missing/incompatible ports, and emits the existing opcode domain
before normalization. Preview, editor admission and reload preparation validate
typed dependency bodies and return owner/path/revision stamps only on success.
No worker receives function bodies or follows live asset references.
Limits are 256 nodes/1,024 links per authored graph, 64 inputs/16 outputs per
function, 16 call levels, 64 distinct dependencies, 4,096 expanded nodes/16,384
links, expression depth 64, 1 MiB per graph and 8 MiB per closure. Diagnostics
retain the originating function, node and call stack for editor navigation.

Semantic function edits, reload, relocation and dependency replacement invalidate
loaded callers through the existing compilation manager. Publication rechecks
the captured closure's live owner stamps; stale completion cannot publish after a
nested edit. Presentation and authoring provenance do not change program identity.
Cook fingerprints every source function, including on warm hits. A versioned
`material-function-source` contributor declares the complete source/schema/package
inputs and overrides generic asset contributors so function dependencies remain
incrementally reusable. Explicit function runtime roots are rejected; Cook emits
no runtime function packages. DMAT contains the expanded compiled stages; cooked
loading needs no function graph, function source package or compiler.

AssetForgeBuiltins ships three focused material functions under
`/Engine/Materials/Functions`: `UVTransform`, `SampleNormal`, and `SampleORM`.
UVTransform rotates scaled UVs and adds an offset. SampleNormal consumes decoded
sampling results and provides strength and RNM composition. SampleORM samples once
and exposes R occlusion, G roughness and B metallic. Whole-surface StandardPBR
recipes are not shipped; authors can connect properties directly or create a
project-specific function when shared processing is needed. Scene import does not
depend on the shipped function library.

New scene imports select a structural parent under the destination mount's
`Materials/ImportedParents/Surface_v1_<digest>` directory. The key records sample
groups, channel outputs and required value/UV operations, excluding resource paths
and ordinary parameter values. Parents carry editor-only recipe provenance and
must exactly match the generated program before reuse; authored changes are
preserved and reported as conflicts. Instances publish only declared owners and
retain recipe, source and output identity without parameter-value history. Equal linear ORM resources, UVs and
samplers share one sample; different sampling requirements remain separate.
Identity UVs stay local. Source samplers, transforms and alpha factors are retained;
normal strength, emissive color and nonidentity occlusion strength are baked once.
New scene parents and outputs remain private through compilation and persistence.
Failed saves restore package/bulk bytes and discard candidates; successful saves
publish the complete in-memory set together. Reused parents remain read-only.
Repeating scene import with the same source and destination replaces matching
source/output identities. Meshes, textures, material instance parameters and static
settings are rebuilt from source; edits to generated outputs are discarded. The
import dialog states this behavior. Store custom copies outside the import output
directory. Unrelated occupied paths are rejected, and removed source outputs are
retained for existing references. Shared parents are never overwritten. Successful
replacement refreshes render/physics bindings; failed publication preserves the
previous authored files and live references. Older outputs without source/output
ownership metadata remain conflicts; there is no implicit ownership migration.

Generated structural parents connect final property values directly to Surface.
Engine's shared root evaluates numerical output policy. Reusable functions can
return aggregate Surface values; resource-output sharing never implicitly merges
separate UV operations. Function GUIDs have no special lowering rules.

Material-library maintenance and explicit parent-template creation follow
[Canonical Asset Resave](../../Editor/Guides/CanonicalResave.md#material-recipe-initialization-and-reconstruction)
and [Explicit PBR material template](../../Editor/Guides/CanonicalResave.md#explicit-pbr-material-template).
These commands preserve compatible authored implementations; scene import does
not invoke template creation, and unsupported graphs require deliberate reconstruction.

## Compile Lifecycle and Cooked Programs

Engine registers `DMaterial` and `DMaterialInstance` to the built-in `Durin.Material` typed manager of its
[asset-compilation aggregate](../Assets/AssetCompilation.md) between task-system
startup and shutdown. `DMaterialInterface` owns non-reflected compilation state.
GameThread snapshots root graph/declarations and effective owner properties into a value-owned request,
normalizes it to obtain the M5 program identity, and submits the expensive
compiler call to the `Engine/MaterialCompile` task scope. Workers retain no
`DObject`, editor, Renderer, RHI, registry, or borrowed-container state. The
only synchronous compatibility path is process bootstrap or tooling without an
active compiling manager; construction in a running engine does not compile
before its object handle exists. `PostLoad`, authored edits, reload, and the
explicit editor action use the asynchronous owner. MaterialEditor selects a
transient working-copy root edit policy: Automatic coalesces semantic edits for 400 ms, Manual
records `NeedsCompile` without submitting, and Immediate preserves non-editor
callers' existing behavior. Loaded instances inherit the root policy. Scheduled
edits use `Scheduled` state and the manager's bounded retry scan, without taking
compiler snapshots or advancing request generations until submission. Selected
finish flushes scheduled automatic work; manual work requires an explicit request.
Late results cannot replace a material with unsubmitted edits. See
[Material editor lifecycle](../../Editor/Architecture/MaterialEditorLifecycle.md#compile-apply-and-save)
for toolbar and save behavior.
The editor working copy has no scene dependents. Its compilation and dynamic
edits affect only preview rendering; Apply copies authored state to the source
root and requests its changed dependent variants. Runtime source identity and
publication rules remain unchanged.

Authored revision, nonzero request generation, dependency generation, latest
terminal result, and accepted renderable program are independent state. A new
request removes the same owner from obsolete work, requests cooperative
cancellation when a flight loses its last consumer, and leaves the accepted
last-known-good program visible. GameThread admits a mailbox result only when
the live object-handle generation, authored revision, request generation,
dependency generation, parent-chain revision, target, and program identity all match. Successful
admission atomically replaces the complete three-stage result and proxy state;
current compilation failure or admission rejection retires the complete accepted
program, layout, values and resources and publishes ErrorMaterial through the
ordinary render proxy path. This includes normalization and dependency failures
before worker submission. Diagnostics no longer report last-known-good display.
Cancellation, supersession and stale results do not replace the visible generation.
A material with no accepted result uses ErrorMaterial, including during retries
after failure; successful admission restores ordinary rendering. Root materials
and instance variants apply this rule to their own compile requests. Immutable
compiler cache entries remain reusable after renderable generation retirement.
The owner stores this contract using the same `FMaterialLocalRenderLayer` value
published to its proxy; shader properties derive from that layer's static
properties. There is no separate accepted-generation type, compiled authored
revision or last-known-good display flag. Current readiness requires Ready state
and matching requested/compiled identities; owner and revision checks remain at
result admission.

The Material compiling manager admits at most 64 distinct flights and 256 consumers. Requests are
bounded to 2 MiB, results to 8 MiB, diagnostics to the M5 64-record/512-byte
limits, and in-process retained results to 128 identities and 256 MiB FIFO.
Equal identities share one flight and retained immutable result while keeping
asset-local generations and diagnostics. Aggregate counters retain no asset or
terminal-request history. Aggregate selected finish and advisory cancellation
filter both material asset classes; aggregate shutdown closes admission, publishes accepted
terminal results, empties the mailbox, and releases flights and retained
results before task-system teardown. Capacity overflow records `Deferred` state
without retaining a request or another owner. A rotating GameThread scan retries
at most 256 owners per pump; finish operations include deferred owners and
cancellation/stop-admission clears their intent.

ShaderBuild owns compiled-output DDC and local dependency manifests as defined
by [Shader Cache](ShaderCache.md). Materials add no second persistent cache for
IR/source or compiled artifacts. The material manager reports retained-result
hits, shared in-flight work, compilation, or forced compilation; corrupt Shader
artifacts follow ShaderBuild's cache-miss/repair contract.

Cook requires a current successful Win64 Game result and never substitutes
ErrorMaterial. Authored expression collections and their owned descendants are stripped from cooked packages. One
DMAT v7 value per material or instance in the `DMaterialInterface::ProgramData`
BulkData field stores the exact
compiler/target/pass/version envelope, program identity, canonical shader properties and separate pipeline metadata,
active declaration contract, compiled layout, and complete shader
code/reflection set. It is uncompressed, 16-byte aligned, bounded to 8 MiB, and
protected by an internal checksum plus the DAST field range and raw-segment extent/hash
contract. Metadata load is range-free; first render-layer construction locks
and decodes the field. Loading rejects missing, truncated, corrupt, trailing, wrong-target,
wrong-profile, wrong-version, invalid-stage, or package/payload static-property
mismatches before publishing an immutable result. Runtime loading therefore
requires neither authored IR/generated source, Shader source files, editor DDC,
nor live compilation.

Both material asset kinds publish a complete `FMaterialLocalRenderLayer`:
shared immutable compiler result, accepted static properties, and native
parameter/resource values. Canonical shader properties derive from that layer. Admission validates the entire
candidate before swapping. Values resolve through the bounded authored chain by
stable GUID/type against the variant's active layout; no packed parent bytes or
parent compilation success are required. Removed declarations retain accepted
native values/resources until replacement. Dynamic edits refresh compatible
values and dependent publications without compiling. A shader/pipeline edit
retains the complete old configuration while pending; current failure clears
the layer and uses normal ErrorMaterial fallback. Pipeline-only
edits apply immediately when compatible with accepted shader properties.

Cook checks current owner/dependency state and rejects stale/error results.
Each instance owns its payload and retains ordinary parent package dependencies;
children never append data to the root package. Equal payload bytes may repeat
across packages; this is separate from in-process immutable result sharing.
Older DMAT outputs require recook; material recipe versions invalidate incremental
outputs. Cooked runtime never compiles. Existing transient owners can select an
exact compatible parent program without compiling; this does not provide the
separate M8 runtime-instance API. Missing/broken parent chains retire live child
state with a dependency diagnostic and ErrorMaterial.

`InspectMaterialParameterDependencies` is the UI-independent dependency
authority. It traverses connected surface branches in fixed surface/input order,
de-duplicates shared declarations by first use. Texture sampling contributes its
explicit Texture2D and UV-expression dependencies; sampler/fallback data travels
with the texture value rather than through a packed scalar role. Instance
eligibility/orphans and local render layers consume this same
snapshot. Serialized but unreachable values remain intact and are excluded from
instance controls and active local bindings; base Details can still edit a selected disconnected owner.

Production Renderer resource slots key generated shader maps, PSOs, diagnostics,
and deterministic draw ordering by the material-program digest plus the exact
pass and geometry-domain contract. On the rendering thread they combine the
shared fixed geometry vertex stage with the accepted generated `FragmentMain`,
`GeometryFragmentMain`, or `ShadowFragmentMain` artifact and create a complete
typed shader map transactionally. Opaque shadow retains the fixed material-
resource-free fragment. StaticMesh, SplineMesh, Material
Preview, and thumbnails therefore consume the same accepted surface program;
none reads the authored graph or IR.

RenderCore represents those sets with `FMaterialShaderMap` and strongly owned
`TMaterialShaderRef` values. Intrinsic generated fragments derive from
`FMaterialShader`; Local, Spline, GBuffer, and opaque-shadow
mesh stages derive from `FMeshMaterialShader`. Mesh compatibility adds only a
registered stable Vertex Factory descriptor, mesh-pass key, frequency, and
local permutation to the existing Material identity. Runtime factory pointers,
vertex declarations, streams, and PSO state remain outside shader identity.
Each geometry family retains independent exact-set caches, bounded to 256 maps
and 512 pipelines per cache, because the lower shader-resource cache already
shares identical code/RHI resources and a second hierarchy would widen failure
and eviction domains without demonstrated sharing. Maps retain no Material
asset or render proxy.

Generated forward evaluation uses the same world normal frame, specular-AA,
directional/local/environment lighting and shadow evaluation through compiled
layout bindings.
GBuffer uses the same evaluator and publishes the established octahedral
normal, effective roughness, AO, opacity, and emissive encoding. Shader reload
rebuilds shader-dependent slots and device invalidation discards then lazily
recreates device-dependent RHI resources from the retained accepted compiler
result; neither operation reinterprets the authored program.

## Renderer Boundary

- `DStaticMesh` owns the ordered material-slot definitions. Each slot has a
  mesh-local user-facing `Name`, exact imported `SourceName`, original
  `SourceMaterialIndex`, and an optional default material. The vector index is
  the stable slot identity; sections store only that compact index.
- Reimport matches unique non-empty exact source names first and unique source
  indices second. Matched entries keep their existing index, user name, and
  default while source evidence is refreshed. Removed entries remain reserved,
  new entries append, and section construction consumes the explicit imported-
  to-stable map rather than searching historical source indices.
- `DMeshComponent` persists a positional `OverrideMaterials` array shared by
  StaticMesh and SplineMesh components. Geometry owners supply slot count,
  name-to-index lookup, and default-material queries; the base owns indexed and
  named mutation, reset, validation, property replay, and binding updates.
  Binding updates are submitted immediately on the game thread through the scene.
  Proxy creation, slot updates, and removal share the ordered render command queue;
  updates carry material references and slot indices, not component revisions.
  Asynchronous work must resolve current component state before submitting a binding.
  Resolution for each current index is non-null component override, mesh
  default, then the Engine-owned `/Engine/Materials/DefaultMaterial` proxy.
  Empty assignments remain null in serialized component and mesh state; the
  service binding is transient. Mesh assignment preserves the complete
  array: shared indices apply immediately, entries beyond a smaller mesh are
  dormant, and a later larger mesh reactivates them. Dormant entries bind no
  dependency and never reach the scene proxy; Clear All removes them too.
- Material-side code resolves accepted declaration GUIDs into one immutable
  `FMaterialRenderRepresentation` carried by `FMaterialRenderData` alongside
  the static shader/pipeline identity. `FMaterialRenderRepresentationBuilder`
  is the only GUID-to-layout compilation seam. The renderer consumes the
  validated compiled binding contract and never performs GUID or `FName` lookup or
  reads reflected material objects.
- StaticMesh and SplineMesh use one Renderer-private material
  binding resolver. It accepts validated compiled layouts through
  `TryGetMaterialRenderBinding`; an unsupported layout records the shared
  fallback reason, emits a renderer-specific `ShaderBinding` diagnostic, and
  selects the code-constructed ErrorMaterial before shader-map or pipeline
  selection. An incompatible terminal is a checked invariant and skips the
  affected production draw.
- Scene-proxy construction walks the current mesh slots in order and retains
  one stable `FMaterialRenderProxyRef` binding per slot. A mesh assignment or
  rebuilt mesh render layout replaces the proxy; dynamic parameter, static
  identity, and parent changes publish through the existing proxy in place.
- Dirty flags classify the publication that a material mutation produces:
  dynamic parameters and compatible static properties publish complete immutable
  layers. GameThread uses the loaded-dependent query to refresh affected owners;
  the render thread builds only that owner's layout and caches by local version.
  It retains no parent proxy and performs no material-object traversal.
- A material publication owns one pending wave per proxy. Repeated edits before
  the render command is consumed replace that pending wave, so only the newest
  immutable state is applied. The command stream preserves publication order
  before later rendering commands consume the proxy. If a material is edited
  while render-command admission is stopped, the retained wave is replayed when
  the rendering thread starts; the first preview or scene-proxy consumer does
  not observe an uninitialized snapshot.
- Component material assignment is a binding update, not material-content
  invalidation. A component-wide revision orders rapid changes across
  independent slots and rejects stale render commands.
- The static mesh shader implements Cook-Torrance GGX direct lighting and
  split-sum image-based environment lighting, plus an unlit viewport mode.
  Direct evaluation clamps perceptual roughness to `[0.045, 1]`; that positive
  lower bound makes the GGX distribution denominator finite without an
  independent epsilon floor. The distribution term uses the stable equivalent
  form `(1 - NoH^2) + NoH^2 * alpha^2`, preserving the supported smooth-surface
  peak. Standard-Lit surfaces apply renderer-owned specular antialiasing after
  the final world-space shading normal is built: bounded screen derivatives of
  that normal increase an effective perceptual roughness before both direct and
  split-sum environment evaluation. The authored roughness, material render
  representation, shader-map identity, and BRDF denominator remain unchanged.
  `FSceneViewModeSettings::bEnableSpecularAA` defaults on and exists only as a
  per-view development A/B seam; there is no material parameter or normal-map
  payload change.
  Missing role textures retain the selected material and use Renderer-owned
  white, black, or flat-normal resources. Missing environment resources retain
  the selected material and use the black environment set.

### Default and Error Surfaces

`FDefaultMaterialService` is initialized on the game thread from
`DEngine::Init()` after Engine Content mounts and render-command admission are
ready, before world or scene-proxy creation. It synchronously loads the exact
base `DMaterial` at `/Engine/Materials/DefaultMaterial`, roots that asset, and
retains one counted proxy for level, preview, and thumbnail consumers. Engine
destruction detaches world, scene, viewport, preview, and thumbnail consumers,
then shuts the service down before Engine and rendering shutdown. Loading is
never lazy and never occurs on the render thread.

The authored default is opaque, lit, one-sided, automatic-depth-write neutral
gray with BaseColor `(0.5, 0.5, 0.5)`, Normal `(0, 0, 1)`, Metallic `0`,
Roughness `0.5`, AmbientOcclusion `1`, zero Emissive, unit Opacity and
OpacityMask, and no textures. It is valid authored data and is not classified
as an error.

Invalid representation construction, material compilation, structural missing
proxies, Renderer layout rejection, and unavailable default content converge
on `GetErrorMaterialRenderData()`. This asset-independent v4 terminal uses the empty compiled layout and
a zeroed view-control slot. Fixed fragment shaders emit opaque magenta without
material descriptors; sampler and environment resolution are bypassed. It is
unlit, two-sided and depth-writing with no texture, package, DDC,
Cook, Engine, or RHI dependency. Whole-material diagnostics use the distinct
reasons `UnassignedDefault`, `DefaultAssetUnavailable`, `MaterialDataInvalid`,
`UnsupportedLayout`, and `MissingProxy`; normal default selection increments a
counter without per-component logging, while default-asset failure logs once
per service lifecycle. Texture and environment recovery do not increment these
whole-material counters.

## Versioned Render Representation

`FMaterialRenderRepresentation` is an Engine-owned immutable snapshot with a
layout identity, validated uniform payload, counted RHI texture references,
typed sampler states, and explicit texture fallbacks. Compiled layout v4 is
material-specific: it deterministically packs only reachable declarations and
is validated against the accepted program and reflected pass bindings. Material
assets persist authored values rather than this transient representation. DMAT
persists the accepted layout and code needed by Game loading; current payload
and generator versions are listed in [Compatibility Boundary](#compatibility-boundary).
Old render layouts require recompilation. The independent error terminal uses
the same layout-v4 boundary as custom materials.

Construction validates the version and identity, field counts, compact-index
contiguity, types, sizes, alignment, non-overlapping ranges, finite values,
zero padding, and resource counts before publication. Invalid construction
returns the complete deterministic ErrorMaterial representation and a diagnostic;
it never publishes a partially filled payload. The representation retains no
reflected object or raw texture pointer.

`FMaterialRenderProxy` resolves parent and local layers into a copied builder,
publishes only a complete representation, and keeps the existing cache,
coalescing, stale-update, startup-replay, and counted-resource contracts.

## Renderer Surface Execution

`FSceneRenderer` owns one Renderer-private surface-material resource service.
StaticMesh, SplineMesh and registered geometry factories consume compiled
material layouts through common mesh-pass execution. The service copies the
accepted uniform payload and resolves compact texture/sampler indices with
explicit White, Black or FlatRGNormal fallbacks. Missing or unready textures
select the declared fallback without retaining a raw RHI pointer beyond the
current command submission. The resource-free error terminal bypasses texture,
sampler and environment resolution and uses fixed magenta fragment shaders.
The Renderer consumes compiled expressions without a fixed-role binding adapter;
authored-format admission follows [Compatibility Boundary](#compatibility-boundary).

The service owns generation-aware sampler slots keyed by the complete
`FMaterialSamplerState`; identical states across registered geometry factories
therefore share one device-generation sampler. Device invalidation and
renderer shutdown release this owner once, while shader maps, pipelines,
geometry resources remain family-owned. A failed
sampler or incomplete resolved packet rejects the smallest owning draw or
batch, preserving feature-local attempt/result accounting.

Resolution is pass-aware. Opaque shadow draws resolve no material uniform,
texture, sampler, environment, or receiver-shadow resource. Masked shadow, Forward and GBuffer
draws resolve the accepted layout and bind only resources present in validated
pass reflection; only forward adds lighting, environment, and
directional-shadow inputs. Irradiance, prefilter, BRDF LUT, and environment
sampler are accepted only as a complete set and otherwise fall back together.
Directional-shadow texture and sampler each retain their deterministic array
and material-sampler fallback.

## Dependency And Invalidation Model

Material mutation publishes explicit render-thread commands. Replacing a
component material assignment rebuilds its scene proxy; parameter changes
publish through the stable proxy and dynamic-only changes reuse shader identity.

- Material dependencies are forward-only. `DMaterialInstance::Parent` is the
  canonical relationship, and dependency tests walk that chain iteratively with
  a cycle guard. A material depends on itself; a base material has no other
  material dependency.
- Loaded child/dependent discovery, GC-protected result lifetime, shared batch
  contexts, and reentrant notifications follow [Scoped Material Queries](MaterialQueries.md).
  Render publication refreshes retained local layers and stable proxies;
  parent and descendant proxies resolve inherited values on the render thread.
- Relationships are canonical forward assignments, with no reverse component
  collections or registered-value mirrors to reconcile after edits or loading.
  Ordinary parameter mutation does not enumerate components or flush rendering.
- Structural work uses the initiating subsystem's primitive and scene lifecycle
  APIs. `FlushRenderingCommands()` is the explicit visibility boundary for tests,
  import, preview, and save workflows.
- Editor hierarchy queries that must include unloaded assets belong to the asset
  registry and package systems, not the runtime material object relationship.
- Static-mesh render-data changes use a separate on-demand loaded-component
  scan and select components whose current mesh assignment equals the changed
  mesh. Rebuilding render state then resolves current slot definitions,
  defaults, and positional overrides directly from canonical storage.
- Proxy diagnostics expose publication, coalescing, resolution-cache hit/miss,
  stale-publication, and binding-update counts; these are not a dependency index.
  Query counters are defined by [Material query diagnostics](MaterialQueries.md#diagnostics-and-validation).

## Compatibility Boundary

Retained Engine material/function content is rebuilt directly through typed recipes.
Material and function owners validate their current expression ownership and typed
connections directly. Instances retain their typed-override storage marker; missing
or unsupported instance markers reject before publication. Compiler capture emits
detached typed IR through `Build()` and owns all worker data without live
expression or callee pointers. Legacy universal graph records are unsupported.

Current versions are compiler envelope 9, DMAT 7, IR 4, layout 4, generator 7,
pass contract 3, and material Cook contributor 5. Time uses the material uniform;
fragments using the old time interpolator and prior Cook hits require rebuilding.
DMAT has no authored Program version word. Materials use ordinary DAST v10
default-relative owned-object serialization, with no material-specific serializer
or old-asset conversion path. Unrelated property/package migrations remain intact.

Mesh components persist the positional `DMeshComponent::OverrideMaterials`
collection. StaticMesh and SplineMesh components serialize only the base
declaration; the former subclass declarations have no compatibility fields or
PostLoad migration. Authored packages must already use the base declaration,
and old Cook outputs must be rebuilt. Shared PostLoad validation preserves
positional and dormant entries without marking the package dirty.

StaticMesh slots persist no GUID or slot-schema version. The
former GUID-keyed override records and slot fields have no loader alias,
upgrade branch, or migration path. Authored packages using those schemas are
incompatible.

Cooked material decoding uses saved target, schema, layout/stage contracts and
code hashes. Compiler identity remains production provenance in the Cooked
domain; standalone Game never requires ShaderBuild to validate its bytecode.

## Environment Lighting

[Sky Lighting](SkyLighting.md) owns independent Sky Light components, ordinary
HDR cube sources, procedural capture, GPU filtering, and generation lifetime.
Forward and deferred materials sample the same complete irradiance/prefilter
set and shared BRDF LUT. Intensity and specified-cube rotation are sample-time
controls; missing eligible sources contribute black without affecting direct
lighting or Emissive. Studio lighting is ordinary scene/component content.

## Geometry and Editor Consumers

[Static Mesh Rendering](StaticMeshRendering.md) owns imported vertex semantics,
source provenance, derived data, and Cook. Material consumers use its vertex
factory and positional slot contract rather than defining another payload.
[Material Editor Lifecycle](../../Editor/Architecture/MaterialEditorLifecycle.md#preview-resources)
owns editor preview lifetime; [Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md)
owns thumbnail fixtures, dependency keys, scheduling, and recovery.

Long-term sequencing and current editor/rendering limitations are tracked in
the [Material System Roadmap](../../Roadmaps/MaterialSystem.md); executable work
uses the bounded plans linked from that roadmap.
