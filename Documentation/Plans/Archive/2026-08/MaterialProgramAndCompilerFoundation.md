# Material Program and Compiler Foundation Plan

Summary: Add a bounded persisted material-program schema, deterministic compiler foundation, and synchronous compiled-surface vertical slice.

Last reviewed: 2026-08-25

Status: Archived
Completed: 2026-08-25

## Current Status

This plan completed Material System roadmap milestone 5. Its entry gate was
satisfied by the landed v3 material representation, shared surface execution
across StaticMesh, SkeletalMesh, and Terrain, RenderCore Shader Cache, and typed
Shader Parameters contracts. Stages 0 and 1 are complete: the bounded
version-1 contract is frozen, the reflected program schema and deterministic
validator are landed, legacy packages transition explicitly, and malformed
present programs fail admission. Stage 2 is also complete: compiler requests
are detached value snapshots, typed IR normalization and canonical bytes are
deterministic, RenderCore exposes virtual dependency fingerprints, and program
identity includes every frozen code-affecting input. Stage 3 is also complete:
normalized IR generates bounded deterministic Slang, RenderCore compiles the
owned in-memory root through its cache/reflection authority, and the synchronous
compiler accepts only a complete exact-binding three-stage result. Stage 4 is
also complete: base materials compile synchronously at construction/load and
code-affecting edits, immutable render data and stable proxy publications carry
the accepted program identity/result, instances share the base handle, and
dynamic or pipeline-only edits reuse it. Stage 5 is also complete: program
identity participates in Renderer shader, pipeline, diagnostic, and draw-sort
keys; transactional StaticMesh, SplineMesh, SkeletalMesh, Terrain, GBuffer,
preview, thumbnail, and shadow paths create typed RHI resources from the
accepted generated fragment set. Stage 6 is selected for full qualification
and M6 handoff. Stage 6 completed the focused, aggregate, Vulkan, full-build,
documentation, and runtime-smoke gates recorded below.

`DMaterial` now persists the canonical fixed parameter definitions plus one
versioned material program; instances resolve the base program by pointer and
do not duplicate it. Production rendering combines the shared fixed geometry
vertex program with the accepted generated forward, GBuffer, or masked-shadow
fragment, while opaque shadow remains material-resource-free.

This plan deliberately remains synchronous. It must leave one immutable,
value-owned compiler input/result and measured compile/artifact baseline for
[Material Compile Lifecycle and Derived Data](MaterialCompileLifecycleAndDerivedData.md),
which owns asynchronous requests, cancellation, generations, last-known-good
asset state, material-level derived artifacts, Cook, and shutdown orchestration.

## Goal

Let a base material persist a bounded typed surface program, validate and
normalize it deterministically, compile it synchronously through RenderCore,
and render materially distinct authored programs through the existing v3 PBR
surface and pass boundary while preserving explicit fixed-schema compatibility
and deterministic failure behavior.

## Scope

- A versioned Engine-owned material-program schema with stable node, parameter,
  input, and surface-output identities and hard count, byte, and depth bounds.
- A minimal acyclic surface-expression domain covering constants, parameter
  reads, texture sampling, selected arithmetic/composition operations, and the
  existing BaseColor, Normal, Metallic, Roughness, AmbientOcclusion, Emissive,
  Opacity, and OpacityMask outputs.
- Deterministic structural validation, type checking, dependency enumeration,
  normalized material IR, canonical encoding, and program identity.
- An immutable value-owned authored-program snapshot and one synchronous
  material compiler entry point/result with bounded node/output diagnostics.
- Deterministic generated Slang input through an explicit RenderCore-owned
  compilation seam; authored material assets never contain arbitrary shader
  source.
- Program-aware shader-map identity and Renderer binding through the existing
  exact-v3 material uniform/resource and shared surface-pass contracts.
- A fixed-schema transition that preserves or explicitly rejects existing
  canonical materials without silently changing their rendered output.
- MaterialInstance program sharing, dynamic override behavior, and any
  M5-defined static override identity behavior.
- Focused schema, compiler, identity, serialization, rendering, reload, device-
  recovery, and Vulkan qualification plus the measurements required by M6.

## Non-Goals

- Asynchronous compilation, task scheduling, cancellation, supersession,
  request generations, debounce, single flight at the material layer, or
  shutdown draining; M6 owns those behaviors.
- Material-level persistent DDC, retention, Cook payloads, cooked-runtime
  admission, or Shipping without live compilation; M6 owns them after this
  plan measures and fixes the synchronous contract.
- A graph canvas, node and pin interaction, copy/paste, diagnostic navigation,
  graph Undo/Redo, or broader MaterialEditor workflow; M7 owns them.
- User-authored Slang snippets, custom functions, material-function assets,
  recursive program composition, loops, dynamic control flow, custom render
  passes, compute materials, decals, post-process materials, or ray tracing.
- Replacing the v3 PBR render layout, the 256-byte surface uniform, the eight
  texture roles, material proxies, geometry-family vertex programs, or the
  shared forward/GBuffer/shadow executor.
- A second generic shader compiler, shader cache, reflection system, descriptor
  binder, or Renderer resource-recovery framework.
- Runtime-only dynamic material instances, bindless resources, descriptor
  virtualization, uniform deduplication, or high-frequency update batching.
- Treating display names, graph presentation coordinates, insertion order, or
  transient editor selection as compiler or parameter identity.

## Design Decisions and Invariants

### Authored program and type domain

- Engine owns the serialized graph, validation, normalization, material IR,
  generated-module contract, and material-program identity. RenderCore owns
  generic Slang compilation, reflection, dependency manifests, shader artifact
  storage, and shader resource code. Renderer owns pass integration, resource
  creation, and fallback selection.
- The first program is a bounded acyclic expression DAG with one explicit
  surface-output record. It is not a general shader language. Stage 0 freezes
  the exact opcode/type table and numeric limits before serialization lands;
  unsupported nodes, types, implicit conversions, missing inputs, and multiple
  writers fail explicitly.
- The initial value system is limited to the scalar/vector/texture values
  necessary to feed the existing eight PBR outputs and texture roles. Numeric
  conversions are explicit IR operations. No operation depends on locale,
  pointer identity, container iteration order, or host floating-point text
  formatting.
- Every node and authored parameter uses a persistent nonzero `FGuid`. Links
  name a source node and output slot; surface outputs name one source value.
  Node display names and future canvas positions are authoring metadata only.
- Parameter GUID remains the instance override and dependency identity.
  Renaming or reordering a parameter does not invalidate its meaning. A
  parameter type/default or reachable expression change does affect the
  normalized program when it can change generated code or binding layout.
- Dynamic parameter and texture values feed the existing v3 uniform/resource
  representation and never enter program identity. Static properties and any
  static program selection do enter identity.

### Validation, normalization, and identity

- Validation is side-effect free and bounded before allocation proportional to
  untrusted counts. It rejects invalid enums and GUIDs, duplicate identities,
  dangling or duplicate links, cycles, excessive depth/fan-in/count/bytes,
  type mismatches, invalid constants, absent required outputs, and output types
  incompatible with the fixed surface contract.
- Normalization operates on a detached value-owned snapshot. It assigns a
  deterministic topological order, canonicalizes commutative inputs where the
  selected opcode permits it, removes presentation-only data, encodes numeric
  values canonically, and emits one versioned IR independent of serialized
  vector order or validation traversal schedule.
- Program identity is a stable digest over the normalized IR, reachable
  generated-module and shader dependency fingerprints, static material
  properties, compiler/generator schema and environment, target, and exact
  surface/pass contract. It never includes `DObject` addresses, package load
  order, process-local IDs, diagnostics, or dynamic values.
- Equivalent normalized programs produce identical canonical bytes, generated
  input, and identity. Any code-affecting node, static property, compiler
  environment, target, exact pass contract, or reachable source dependency
  change produces a different identity.
- Diagnostics distinguish schema, bounds, graph structure, type checking,
  normalization, generation, Slang compilation, reflection, and Renderer
  binding failures. Each record is bounded and carries an asset-qualified
  context plus stable node/input/output location when available.

### Compilation and thread ownership

- GameThread snapshots all reflected material and referenced asset state into
  immutable, value-owned input before calling the compiler. The compiler never
  resolves or mutates a `DObject`, asset registry, editor model, material proxy,
  Renderer, RHI object, or live package state.
- M5 exposes a synchronous compiler entry point and complete result. The result
  owns normalized identity, generated-input metadata, compiled stage artifacts
  or retained RenderCore resource code, reflection/binding evidence, bounded
  diagnostics, timings, dependency fingerprints, and byte/count measurements.
- Generated Slang is a deterministic compiler input under an engine-controlled
  virtual identity. It may import only the allowlisted shared surface modules
  selected in Stage 0. No arbitrary physical include, absolute path, or
  asset-supplied source text crosses the seam.
- RenderCore remains the sole compilation/cache authority. M5 may extend its
  request API for a value-owned generated root module, but it does not publish
  a second cache hierarchy or copy SPIR-V/reflection into a material-owned
  artifact.
- M5 remains synchronous even if RenderCore internally coalesces identical
  shader requests. The M5 API must not capture owner-thread state so M6 can
  submit the same request envelope to Worker execution without redesign.

### Renderer publication and compatibility

- The material-program digest extends the shader-map identity; pass-planning
  state remains separate. Geometry vertex shaders and the exact v3 material
  binding stay shared, while generated fragment evaluation is selected by the
  program and requested surface pass.
- One successful compiler result is complete for every M5-required fragment
  entry point and binding contract. No partial pass set, SPIR-V set, reflection
  set, or Renderer candidate may publish.
- StaticMesh, SkeletalMesh, Terrain, Material Preview, and thumbnails consume
  the program through their existing material render data and Renderer-private
  surface seam. They do not inspect the authored graph or normalized IR.
- Opaque shadow remains material-resource-free. Masked shadow evaluates only
  the required opacity-mask contract. Forward and GBuffer use the same
  generated surface evaluation and exact-v3 resource table.
- Fixed canonical materials follow one explicit versioned transition selected
  in Stage 0: either a persisted/synthesized canonical program with byte-for-
  byte qualified output or a deliberate package compatibility failure with a
  repository content migration. Absence, corruption, and invalid authored
  program state are never silently interpreted as a different user program.
- A synchronous compile, reflection, or binding failure publishes no partial
  candidate. Before M6 adds asset-local last-known-good orchestration, a
  material without an accepted compiled candidate selects the existing asset-
  independent ErrorMaterial and exposes the bounded compile diagnostic.
- Material instances share their parent's program identity and compiled result.
  Dynamic overrides change only v3 values/resources. An instance may select a
  different program only through an explicitly validated M5 static override;
  otherwise no instance-local graph is introduced.

## Stage 0 Frozen Contract

This section is the implementation authority for M5 until Stage 6 moves the
landed behavior into the runtime contracts. Every number and production choice
below is fixed for material-program schema version 1, IR version 1, generator
version 1, and compiler-envelope version 1. Later versions may raise bounds or
add opcodes without changing the interpretation of version 1 bytes.

### Authored schema and bounds

`DMaterial` owns one reflected `FMaterialProgram` value. Instances never own a
program in M5 and always use the root base material's program. The program has
one `uint32 SchemaVersion`, one node array, and one `FMaterialSurfaceOutputs`
record. A node has a nonzero stable GUID, an opcode, a declared result type,
opcode-specific immediate data, and ordered input links. A link names one
source node GUID and its zero-based output slot. Version 1 nodes expose exactly
one output except `TextureSample2D`, whose sole output is `Float4`; swizzling is
always an explicit node.

| Bound | Version 1 value |
| --- | ---: |
| Nodes | 256 |
| Links, including surface-output links | 1,024 |
| Authored parameters referenced by the program | 128 |
| Inputs on one node | 8 |
| Longest dependency path | 64 nodes |
| UTF-8 display-name bytes | 128 per name |
| Aggregate authored string bytes | 16 KiB |
| Canonical encoded program bytes | 1 MiB |
| Diagnostics | 64 records, 512 UTF-8 message bytes each |
| Generated root bytes | 256 KiB |
| Reachable shader dependencies | 64 files |
| Compiled fragment stages | 3 generated plus 1 fixed opaque-shadow stage |
| One SPIR-V stage | 16 MiB |
| Complete compiled stage set | 32 MiB |

The value domain is `Float`, `Float2`, `Float3`, `Float4`, and `Texture2D`.
There is no bool, integer, matrix, sampler, string, struct, or implicit numeric
type in version 1. Finite IEEE-754 binary32 values are the only numeric
constants. Texture values are the existing v3 texture-role parameters and
sampler/UV state remains in that representation rather than graph values.

The opcode table is closed:

| Opcode family | Version 1 opcodes and result rule |
| --- | --- |
| Leaves | `Constant`, `Parameter`, `TextureParameter`, and `TextureCoordinate`; constants and numeric parameters declare one numeric result, texture parameters return `Texture2D`, and texture coordinates return `Float2` while referencing one canonical texture-role GUID whose dynamic channel/transform companions stay in exact-v3 data |
| Sampling | `TextureSample2D(Texture2D, Float2) -> Float4`; role, sampler, scale, offset, rotation, and channel resolve through the referenced canonical texture parameter's exact-v3 companion fields |
| Arithmetic | `Add`, `Subtract`, `Multiply`, `Divide`, `Minimum`, and `Maximum`; both inputs and result have the same numeric type |
| Unary | `Negate`, `OneMinus`, `Absolute`, `Saturate`, and `Normalize`; input and result have the same numeric type, while `Normalize` accepts only `Float2` through `Float4` |
| Ternary | `Clamp(value, minimum, maximum)` uses one numeric type; `Lerp(a, b, alpha)` requires `a`, `b`, and result to match and requires scalar `alpha` |
| Composition | `MakeFloat2`, `MakeFloat3`, and `MakeFloat4` take exactly 2, 3, or 4 scalar inputs; `Swizzle` takes one numeric vector and a 1-4 component immediate mask and declares the corresponding numeric result |
| Explicit conversion | `Splat2`, `Splat3`, and `Splat4` convert one `Float`; `TruncateToFloat`, `TruncateToFloat2`, and `TruncateToFloat3` select the leading components of a larger vector |
| Normal helpers | `DecodeNormalRG(Float2) -> Float3` and `BlendNormalsRNM(Float3, Float3) -> Float3` preserve the existing safe tangent-normal decode and RNM contract |

No other conversion occurs. Division is generated as ordinary IEEE-754
division; validation rejects non-finite authored constants but does not attempt
value-range analysis. Normalization uses the existing finite safe-normal helper
for normals and a generated epsilon-guarded zero result for other vectors.
Commutative canonicalization applies only to `Add`, `Multiply`, `Minimum`, and
`Maximum`.

The surface-output record contains exactly one required link for each output:

| Output | Required type | Canonical fixed-program source |
| --- | --- | --- |
| BaseColor | `Float3` | `BaseColor` parameter multiplied by BaseColor texture RGB |
| Normal | `Float3` | canonical normal parameter combined with decoded sampled normal RG through the existing RNM contract |
| Metallic | `Float` | `Metallic` parameter multiplied by Metallic texture B |
| Roughness | `Float` | `Roughness` parameter multiplied by Roughness texture G |
| AmbientOcclusion | `Float` | AmbientOcclusion parameter multiplied by AO texture R |
| Emissive | `Float3` | nonnegative `Emissive` parameter plus nonnegative Emissive texture RGB |
| Opacity | `Float` | `Opacity` parameter multiplied by Opacity texture A |
| OpacityMask | `Float` | `OpacityMask` parameter multiplied by OpacityMask texture R |

All eight links must exist even when a pass does not consume them. There are no
implicit surface defaults in serialized version 1. The canonical fixed program
is a public Engine factory with permanent node GUIDs and is the only program
used for the legacy transition.

### Serialization and compatibility

- `SchemaVersion == 1` is required whenever the `Program` field is present.
  Unknown versions, invalid enum values, zero or duplicate GUIDs, dangling or
  duplicate input writers, cycles, excessive counts/depth/bytes, invalid
  constants, type mismatches, or missing surface outputs reject admission.
- Packages written before the reflected `Program` field existed take one
  explicit legacy transition during load: Engine assigns the canonical fixed
  program before post-load validation. The next save writes version 1. A
  present-but-empty or malformed program never takes this transition and
  selects ErrorMaterial.
- Package round trip preserves node, parameter, and link GUIDs and canonical
  array order. Asset duplication deep-copies the value and preserves its GUIDs;
  package object references are remapped by the package system, while program
  GUIDs are not regenerated because their scope is the duplicated material.
- Display names are optional presentation metadata. They round trip but do not
  enter normalization or identity. Graph positions and editor selection are
  not part of the M5 schema.
- Referenced parameter GUIDs must resolve to the canonical base-material
  definition with the exact declared type. Texture sampling may reference only
  one of the eight canonical texture-role GUIDs. Orphan instance overrides
  remain excluded exactly as on the fixed path.

### Normalized IR and program identity

Validation first copies all required reflected values into an immutable
`FMaterialCompilerInput`. Normalization retains only nodes reachable from the
eight surface outputs, sorts roots by the fixed output-table order, assigns a
stable topological index using canonical node content followed by GUID as the
tie breaker, canonicalizes the four commutative opcodes, and rewrites links to
IR indices. Node GUIDs remain in diagnostic locations but do not distinguish
otherwise equivalent normalized programs.

Canonical encoding is an explicitly little-endian byte stream. It begins with
ASCII domain `DurinMaterialProgramIR`, a zero terminator, IR version, node and
output counts, then length-prefixed records. Enums are encoded as fixed `uint8`,
counts and indices as `uint32`, GUIDs as their four `uint32` words, strings as
UTF-8 byte count plus bytes, and floats as canonical binary32 bits. Negative
zero becomes positive zero; NaNs and infinities were already rejected. No
native struct bytes, padding, locale text, pointer, or container iteration order
enters the stream.

`FMaterialProgramIdentity` is an `FXxHash128` digest over the following
length-prefixed domains in order:

1. canonical IR bytes;
2. sorted reachable dependency records containing normalized virtual path and
   content `FXxHash128`;
3. blend mode, shading model, and canonical opacity-mask-threshold bits;
4. schema, IR, generator, and compiler-envelope versions;
5. RenderCore compiler-environment identity and target triple/profile;
6. exact v3 render-layout version and GUID;
7. ordered required entry-point names, frequencies, and pass-contract version.

Dynamic numeric/texture values, sampler values, UV transforms, two-sided state,
depth-write policy, display metadata, node GUIDs, diagnostics, asset path, and
package/load identity are excluded. Parameter GUID and type enter IR; changing
only a dynamic default value does not. A static parameter facility is not added
in M5.

### Generated source and synchronous compiler seam

The generated virtual root is
`/Generated/Materials/<32-lowercase-hex-program-identity>` and is supplied as
owned UTF-8 bytes, never a physical path. RenderCore accepts the virtual root,
source bytes, ordered entry points/frequencies, macros, compiler environment,
and the allowlisted import roots as one value-owned request. The generated
source may import only `Material.SurfaceMaterial`,
`Material.SpecularAntialiasing`, `Lighting.SurfaceLighting`, and the dedicated
versioned material-fragment ABI module introduced by Stage 3. Transitive imports
are fingerprinted by RenderCore. Relative physical includes, absolute paths,
parent traversal, and authored source text are rejected.

Generated fragment entry points are exactly `FragmentMain` for forward,
`GeometryFragmentMain` for GBuffer, and `ShadowFragmentMain` for masked shadow.
The fixed `/Engine/StaticMeshBasePass::OpaqueShadowFragmentMain` remains the
resource-free opaque-shadow fragment. Geometry-family vertex shaders remain
fixed and shared. The generated root exposes no additional entry point and
must reflect the exact existing bindings for its pass: 24 for forward, 17 for
GBuffer, and Material plus OpacityMask texture/sampler for masked shadow. There
are no push constants. Interface locations and GBuffer target count must match
the fixed root characterization tests.

`CompileMaterialProgram(const FMaterialCompilerInput&)` is the sole M5 entry
point. Its result owns identity, canonical-IR and generated-source byte counts,
dependency fingerprints, the complete three-stage RenderCore output/resource
code, phase timings, and diagnostics. It never retains a `DObject`, borrowed
span, asset registry, editor model, Renderer/RHI object, or cache path. Success
requires all three stages, valid SPIR-V, exact source/binary entry points,
expected reflection, no unexpected resources or push constants, and all bounds.
Failure returns no stage usable for publication.

Diagnostics use the closed categories `Schema`, `Bounds`, `Graph`, `Type`,
`Normalization`, `Generation`, `Dependency`, `Compile`, `Reflection`, and
`Binding`. A record carries asset context, category, bounded message, and an
optional stable node GUID plus input index or surface-output enum. Records sort
by category, node GUID, location kind/index, then message bytes.

### Engine and Renderer publication seam

- `FMaterialRenderData` gains only immutable program identity and a shared
  accepted compiled-program handle. `FMaterialRenderRepresentation`, its
  416-byte/48-uniform/eight-resource exact-v3 layout, and the 256-byte Renderer
  surface uniform do not change.
- `FMaterialShaderMapIdentity` gains program identity. Blend mode, shading
  model, and mask threshold remain shader-map inputs; two-sided state and
  depth-write policy remain only in `FMaterialPlanningPassIdentity`.
- A base material snapshots and compiles synchronously on load or a
  code-affecting edit. It publishes one complete accepted program plus v3 data,
  or the asset-independent ErrorMaterial. Dynamic value/resource edits rebuild
  only v3 data. Instances share the root program handle and identity and apply
  only existing dynamic and complete static-property overrides.
- Renderer-private resource slots key generated fragment shader maps by program
  identity plus existing shader identity, create all required fragment
  resources transactionally, and retain the last valid slot candidate on
  reload failure. No partial pass set reaches a proxy or draw.
- StaticMesh, SkeletalMesh, Terrain, preview, and thumbnail paths consume only
  `FMaterialRenderData`. Forward and GBuffer use the same generated evaluator;
  masked shadow consumes only OpacityMask; opaque shadow remains material-
  resource-free. Exact-v3 role fallbacks and ErrorMaterial remain unchanged.
- Shader reload advances shader-dependent slots; device invalidation drops
  device-dependent resources and lazily rebuilds them from the accepted
  compiled program. Neither path recompiles or reinterprets the authored graph
  unless the existing RenderCore dependency contract requires compilation.

### Characterization and measurement authority

The fixed-path guard set is distributed by ownership: material layout/error
and dynamic/static identity live in `MaterialTests`; surface uniform bytes and
pass role masks live in `RendererSceneContractTests`; shader entry points,
reflection, vertex-factory domains, and GBuffer locations live in
`RenderShaderContractTests`; shader-change and device-generation recovery live
in `RendererResourceTests` and `RendererResourceReloadVulkanTests`; rendered
preview/thumbnail parity lives in `MaterialVulkanTests`.

`FShaderCompileServiceTests.M5FixedMaterialPathRecordsCompleteColdAndWarmBaseline`
is the repeatable M6 input. It force-compiles the four fixed fragment stages,
then takes an in-process warm hit and records cold/warm microseconds, dependency
count, fixed root bytes, aggregate SPIR-V bytes, and reflection-sidecar bytes.
Stage 0 records the observed Debug profile and hardware result below; timing is
diagnostic because the current lane is not an exclusive performance lane.

| Measurement | Stage 0 observed value |
| --- | --- |
| Build profile / host | `Win64-Debug-DurinEditor`, windows-msvc-x64; NVIDIA GeForce RTX 3090, Vulkan 1.4.325 |
| Cold fixed fragment compile | 683,284 us forced compile |
| Warm in-process lookup | 1,852 us memory hit |
| Reachable dependencies | 8 |
| Fixed root input bytes | 17,327 B |
| Aggregate SPIR-V bytes | 140,408 B across four fixed fragment stages |
| Reflection sidecar bytes | 9,965 B across four sidecars |
| First Renderer shader/pipeline resource resolution | 68,239 us through the Vulkan reload resource slot |
| Synchronous failure categories | Schema/bounds/graph/type/normalization/generation/dependency/compile/reflection/binding; injected coverage recorded by the owning focused tests |

## Current Foundations and Gaps

### Foundations

- `DMaterial` owns canonical parameter definitions and static properties;
  `DMaterialInstance` owns GUID-based overrides and parent inheritance.
- `DMaterial` also owns one reflected bounded version-1 program with stable
  node/link/output identities. The validator rejects schema, graph, type, and
  bound failures deterministically; instances share the base program.
- Engine snapshots that authored state into a detached value-owned compiler
  input, lowers only reachable semantics into versioned typed IR, and hashes
  canonical bytes with code-affecting static, dependency, compiler, target,
  layout, and pass inputs. Authored order, node GUIDs, display labels, dead
  nodes, dynamic values, two-sided state, and depth policy are excluded.
- RenderCore exposes the Slang environment identity and a sorted virtual-path,
  content-hash dependency manifest without leaking physical cache paths.
- The synchronous material compiler generates stable `n<IR-index>` Slang,
  compiles the forward/GBuffer/masked-shadow entry set from an owned in-memory
  root, validates exact binding/stage/reflection contracts, and returns no
  compiled stages on any normalization, generation, compile, or reflection
  failure. RenderCore retains artifact/cache ownership and enforces import
  virtual roots.
- Base materials publish the accepted result as a shared immutable handle on
  `FMaterialRenderData`; the program digest extends shader-map identity while
  two-sided/depth state stays pipeline-only. Base proxy layers carry the same
  handle, instances inherit it, and missing/failed required compilation makes
  direct or proxy resolution select the complete ErrorMaterial terminal.
- `FMaterialRenderRepresentation` provides a completely validated immutable v3
  payload with 48 uniform fields, eight counted texture roles, deterministic
  defaults, and an asset-independent ErrorMaterial terminal.
- Stable material render proxies coalesce complete publications, reject stale
  local versions, resolve inheritance lazily, replay after admission restarts,
  and preserve counted resource lifetime.
- StaticMesh, SkeletalMesh, and Terrain share Renderer-private forward,
  GBuffer, opaque-shadow, and masked-shadow surface execution while retaining
  geometry-specific vertex programs and pipelines.
- RenderCore already provides synchronous Slang compilation, dependency
  manifests, deterministic macro/environment identity, in-process single
  flight, content-addressed SPIR-V/reflection artifacts, strict validation,
  bounded retention, and corruption recovery.
- Typed Shader Parameters resolve declared fields against reflection once and
  keep descriptor indices out of Renderer material call sites.
- Renderer resource slots already create complete candidates transactionally,
  preserve valid resources on refresh failure, and rebuild after shader reload
  or device invalidation.
- Asset packages provide bounded reflected serialization, stable references,
  duplication, strict compatibility validation, and authored-versus-derived
  ownership boundaries.

### Gaps this plan closes

- `FMaterialShaderMapIdentity` contains only render-layout and static surface
  properties, so materially different authored programs cannot select distinct
  fragment shader maps.
- `/Engine/StaticMeshBasePass` owns one fixed fragment implementation; there is
  no generated-root-module request or constrained material evaluation seam.
- Existing tests now round-trip programs, reject malformed graphs, perturb
  semantic-equivalent graph representations, pin identity inputs, and record
  the fixed compile/artifact baseline; two distinct compiled/rendered programs
  remain open.

## Implementation Stages

### Stage 0: Characterize and lock the compiler contract

Dependencies: Landed Material System roadmap M4, Shader Cache, and Shader
Parameters contracts.

- [x] Characterize the canonical `DMaterial` package schema, instance override
  behavior, current shader-map keys, fixed fragment entry points, reflection,
  Renderer resource creation, fallback, reload, and device-recovery paths. The
  frozen contract maps each behavior to its owning focused guard.
- [x] Measure fixed-path cold/warm compile latency, dependency count, generated
  input size, SPIR-V/reflection bytes, Renderer resource-creation time, and
  synchronous failure categories on the selected build profile and
  representative hardware. The Stage 0 table records the diagnostic Debug/RTX
  3090 observations and the repeatable test properties.
- [x] Freeze the initial expression types, opcodes, output table, explicit
  conversion rules, defaults, and hard graph/node/link/depth/string/byte bounds.
- [x] Freeze the serialized program/version ownership, stable identities,
  package round-trip and duplication rules, malformed-data behavior, and exact
  fixed-schema transition or content migration.
- [x] Freeze the normalized IR schema, canonical encoding, digest algorithm,
  dependency closure, compiler/generator versions, target/pass inputs, and the
  distinction between dynamic values and compile identity.
- [x] Freeze the generated Slang root/module ABI, allowlisted imports, required
  entry points, synchronous RenderCore request/result, reflection checks,
  diagnostic locations, and output bounds.
- [x] Freeze the `FMaterialRenderData`, proxy, shader-map, pass-planning, and
  Renderer resource-slot seam without changing the exact-v3 material layout.
- [x] Add characterization tests that pin canonical fixed-material output,
  identity, fallback, all geometry/pass consumers, shader reload, and device
  recovery before changing the production path. New identity and measured
  compile/resource guards complement the existing ABI, consumer, reload, and
  recovery suites.

#### Acceptance Gate

- One bounded, non-conflicting schema/type/IR/identity/compiler/Renderer and
  compatibility contract is recorded with no unresolved production choice.
- Baseline timings, artifact sizes, dependencies, and synchronous failure
  categories are recorded in forms directly consumable by M6 Stage 0.
- Existing tests can detect a changed canonical surface, incompatible v3
  binding, partial pass publication, or lost ErrorMaterial fallback.

### Stage 1: Add the persisted material-program schema and validator

Dependencies: Stage 0.

- [x] Add reflected, versioned Engine value types for program nodes, typed
  inputs/links, literals, parameter references, texture samples, and the single
  surface-output record using the Stage 0 identities and bounds.
- [x] Persist the program on base `DMaterial` while keeping instances parent-
  program-sharing by default and retaining GUID-based dynamic overrides.
- [x] Implement bounded validation for schema/version, enums, identities,
  counts, bytes, links, fan-in, cycles, depth, types, constants, parameter
  references, required outputs, and output compatibility.
- [x] Produce bounded deterministic diagnostics with stable node/input/output
  source locations and no pointer or container-order dependence.
- [x] Implement the selected canonical fixed-content transition and reject
  ambiguous, missing-required, or malformed present program data explicitly.
- [x] Add package round-trip, deterministic bytes, duplicate/remap, reference
  enumeration, default construction, strict compatibility, malformed archive,
  cycle, depth, count, type, and source-location tests.

#### Acceptance Gate

- Valid programs and canonical-transition materials round-trip and duplicate
  with stable semantic identities; invalid untrusted records fail before live
  render publication.
- Every count, allocation, traversal, diagnostic string, and graph depth is
  bounded by a named Stage 0 limit.
- Instances retain correct overrides and dependency references without owning
  or duplicating the parent graph.

### Stage 2: Implement normalized IR and deterministic identity

Dependencies: Stage 1.

- [x] Snapshot the validated authored program, parameter declarations, static
  properties, target, and dependency inputs into an immutable value-owned
  compiler request on GameThread.
- [x] Lower the snapshot into the versioned typed IR with deterministic
  topological ordering, explicit conversions, canonical constants, and the
  selected dead-node and commutative-operation policy.
- [x] Canonically encode the IR and every identity input with explicit enum,
  width, byte-order, string, collection-order, and numeric rules.
- [x] Enumerate and fingerprint the complete reachable generated-module and
  shader dependency closure without including dynamic parameter/resource
  values or presentation-only graph metadata.
- [x] Build the stable program identity from normalized IR, dependencies,
  generator/compiler environment, target, static properties, and exact
  surface/pass contract.
- [x] Add schedule/order perturbation, equivalent-graph, code-affecting edit,
  dynamic-value edit, dependency edit, static-property, target, compiler-
  environment, canonical-byte, and collision-fixture tests.

#### Acceptance Gate

- Equivalent programs produce byte-identical IR and identity independently of
  serialized node order, traversal schedule, display names, and dynamic values.
- Every code-affecting program, dependency, static-property, target, compiler,
  or pass-contract change invalidates identity deterministically.
- The immutable request contains no `DObject`, editor, Renderer, RHI, physical
  cache path, process-local ID, or borrowed live-container state.

### Stage 3: Add deterministic source generation and synchronous compilation

Dependencies: Stage 2.

- [x] Generate bounded deterministic Slang from normalized IR through the
  Stage 0 module ABI, stable symbol scheme, numeric encoding, and allowlisted
  shared-surface imports.
- [x] Add the minimal RenderCore generated-root request seam while preserving
  its ownership of dependency resolution, compiler environment, reflection,
  cache identity, artifact validation, and retention.
- [x] Implement one synchronous material compiler entry point and complete
  value-owned result for validation, lowering, generation, compile, reflection,
  and binding outcomes.
- [x] Compile and validate the complete M5 fragment entry-point/pass set as one
  candidate; reject missing, extra, wrong-type, wrong-array, or incompatible
  reflected bindings before publication.
- [x] Bound generated bytes, entry points, dependencies, compiled stages,
  reflection records, artifacts, diagnostics, and retained compiler output.
- [x] Add golden generation, determinism, allowlist/path rejection, malformed
  IR, compiler failure, reflection mismatch, artifact corruption-repair, warm/
  miss, force-recompile, and complete-or-no-result tests.
- [x] Record cold/warm phase timings, generated bytes, dependency counts,
  artifact/reflection sizes, cache outcomes, and failure categories for M6.
  The final repeatable Debug characterization records 10,825 generated bytes,
  eight dependency fingerprints, 134,580 aggregate SPIR-V bytes, 909 us
  normalization, 460 us generation, 592,801 us forced compilation, and 65,460
  us warm generated-root resolution/cache lookup on the Stage 0 host.

#### Acceptance Gate

- A valid immutable request synchronously returns one deterministic identity
  and complete compiled pass set; any phase failure returns no publishable
  partial candidate and an actionable bounded diagnostic.
- Generated material input cannot escape the selected virtual shader roots or
  import arbitrary authored/physical source.
- RenderCore remains the only owner of SPIR-V/reflection cache artifacts and
  existing warm, corruption, retention, and force-recompile behavior remains
  valid.

### Stage 4: Publish compiled programs through the Engine material boundary

Dependencies: Stage 3.

- [x] Integrate synchronous snapshot/compile/admission into base-material
  construction, post-load, and compile-affecting mutation without introducing
  M6 request generations or background tasks.
- [x] Extend immutable material render data and shader-map identity with the
  accepted program identity and complete compiler result/reference while
  keeping pass-planning fields separate.
- [x] Publish only an accepted complete result through the stable material
  proxy; invalid schema, compile, reflection, or binding state selects the
  ErrorMaterial terminal with the compile diagnostic.
- [x] Keep dynamic parameter/texture edits on the v3 representation path and
  reuse program identity; recompile only code-affecting program, dependency,
  static-property, target, or contract changes.
- [x] Preserve parent program sharing and dynamic instance overrides; implement
  only the Stage 0-selected static override program behavior.
- [x] Add load/edit/save/reload, proxy coalescing, instance inheritance,
  dynamic-versus-static mutation, failure/recovery, object destruction, and
  exact canonical-transition tests.

#### Acceptance Gate

- Every compiled base material publishes one immutable program identity and
  exact-v3 value/resource representation; no Renderer consumer reads authored
  nodes or reflected objects.
- Dynamic-only edits reuse compiled identity, while every selected compile-
  affecting edit synchronously produces and publishes the matching complete
  result or deterministic ErrorMaterial.
- Canonical fixed content follows the recorded transition with qualified
  output and no silent semantic change.

### Stage 5: Integrate the compiled surface across Renderer consumers

Dependencies: Stage 4.

- [x] Extend shared material shader/resource keys with program identity and
  exact pass contract without folding two-sided/depth-only planning policy into
  generated program identity unnecessarily.
- [x] Consume the accepted compiled fragment pass set through transactional
  Renderer resource slots and existing typed Shader Parameters; create RHI
  shaders and pipelines only on their rendering-thread owners.
- [x] Bind one generated surface evaluation contract for StaticMesh,
  SkeletalMesh, Terrain, Material Preview, and thumbnail consumers across
  forward, GBuffer, and applicable opaque/masked shadow paths.
- [x] Preserve exact v3 uniform/resource resolution, role fallbacks,
  environment lighting, ErrorMaterial selection, pass culling/depth/blend
  policy, reload generation, and device-recovery ownership.
- [x] Add focused parity and failure tests proving two materially distinct
  programs select different identities and output, while identical programs
  reuse code/resource candidates where the existing caches permit.
- [x] Add rendered-output tests for constant/parameter/arithmetic/texture
  programs, each geometry family, preview/thumbnail, forward/GBuffer, masked
  shadow, reload, RHI/PSO failure, and device invalidation.

#### Acceptance Gate

- Two authored programs with distinct normalized identities render distinct
  qualified results through every applicable production geometry/pass family.
- No consumer observes a partial pass set, incompatible reflection/binding, or
  graph-owned state; fixed canonical and ErrorMaterial output remain qualified.
- Shader reload and device invalidation rebuild from the accepted compiled
  result through existing transactional Renderer ownership.

### Stage 6: Qualify M5 and hand off the synchronous contract

Dependencies: Stages 1 through 5.

- [x] Run focused schema/serialization/IR/identity/compiler/reflection/Engine/
  Renderer tests in the selected Agent Build Profile.
- [x] Run StaticMesh, SkeletalMesh, Terrain, preview, thumbnail, forward,
  GBuffer, shadow, reload, device-recovery, and Vulkan rendered-output tests
  selected by the testing guide.
- [x] Run cold/warm, graph-bound, malformed-input, dependency-fan-out,
  deterministic-repeat, cache-corruption, and resource-failure workloads
  against the Stage 0 limits and measurements.
- [x] Run owning module targets, required aggregate tests, complete build, and
  editor runtime smoke selected by repository guidance.
- [x] Move lasting program/schema/IR/identity/compiler/Renderer/compatibility
  contracts into their authoritative Runtime documents.
- [x] Update the Material System roadmap and M6 Stage 0 with the exact immutable
  compiler input/result, source locations, Renderer seam, fixed-schema
  transition, timing, artifact size, dependency, and failure evidence.
- [x] Record exact validation commands, profile, target, hardware, cache state,
  measurements, limitations, and handoff before completing the plan.

#### Acceptance Gate

- The Material System M5 exit gate and every validation-matrix row pass with
  recorded evidence.
- Authored program round trips, invalid programs fail deterministically, two
  distinct programs compile and render, dependency edits invalidate identity,
  and fixed-schema content follows its explicit transition policy.
- M6 can move the immutable synchronous request/result to bounded Worker
  execution without changing schema, identity, compiler, diagnostic, or
  Renderer publication contracts.
- Lasting behavior no longer depends on this active plan as its sole authority.

#### Stage 6 evidence

Qualification used `Win64-Debug-DurinEditor` on Windows x64 with an NVIDIA
GeForce RTX 3090 (Vulkan 1.4.325). RenderCore's ordinary process and disk caches
were enabled; the generated and fixed characterization cases explicitly forced
their cold compilation before measuring an in-process warm request.

- `DevTool test MaterialTests` passed 84/84; the focused generated-program case
  recorded 10,825 source bytes, eight dependencies, 134,580 SPIR-V bytes,
  909/460/592,801 us normalize/generate/cold compile, and 65,460 us warm.
- `DevTool test RenderShaderServiceTests
  FShaderCompileServiceTests.M5FixedMaterialPathRecordsCompleteColdAndWarmBaseline`
  recorded 17,327 source bytes, 140,408 SPIR-V bytes, 9,965 reflection bytes,
  637,110 us cold, and 1,594 us warm. `RenderShaderContractTests` passed 39/39.
- `DevTool test "@domain=material"`, `"@domain=shader"`,
  `"@domain=static-mesh"`, `"@domain=skeletal-mesh"`, and
  `"@domain=terrain"` passed their resolved routine selections.
- `MaterialVulkanTests`, `StaticMeshRenderPreparationVulkanTests`,
  `SkeletalMeshRenderResourcesVulkanTests`, `TerrainRenderVulkanTests`,
  `TerrainRenderQualificationTests --mode qualification`,
  `RendererResourceReloadVulkanTests`, and `EditorGridVulkanTests` passed.
  These cover distinct/restored program pixels, preview/thumbnail, all geometry
  domains, forward/GBuffer/shadow, shader reload, injected shader/PSO/image
  failures, and device-generation reconstruction.
- `DevTool test fast-all` passed the complete 60-target contract/feature/
  infrastructure selection on rerun. Its first parallel run exposed one
  transient `CoreConcurrencyTests` fan-in timing failure; the focused case and
  full target then passed (141/141) before the successful aggregate rerun.
- `DevTool build --target all` passed. `DevTool run --project
  Sandbox/Sandbox.dproject --args --hidden-window --exit-after-ticks=30`
  completed initialization, 30 ticks, and normal editor shutdown.
- `DevTool doc validate --scope changed`, `doc plan validate --scope all`, and
  the final plan/roadmap/all-document validators passed after the lasting
  Runtime, roadmap, and M6 handoff updates.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Schema and assets | Deterministic package round trip, defaults, duplication, stable GUIDs, reference enumeration, bounds, malformed/unknown schema rejection, cycle/depth/type failures, and fixed-content transition |
| IR and identity | Schedule/order-independent normalization, canonical bytes, equivalent-graph equality, code-affecting inequality, dynamic-value exclusion, dependency closure, target/compiler/pass isolation, and deterministic diagnostics |
| Compiler | Golden generated input, allowlisted imports, complete pass set, cold/warm/cache paths, compile/reflection/binding failures, corruption repair, force recompile, and bounded phase/byte records |
| Ownership | GameThread value snapshot; no compiler `DObject`, editor, Renderer, RHI, registry, borrowed-container, or physical-cache-path ownership; rendering-thread-only RHI creation |
| Engine publication | Complete-or-ErrorMaterial synchronous publication, program identity on immutable render data, proxy ordering/coalescing, instance parent sharing, and dynamic-versus-static mutation behavior |
| Renderer | Exact-v3 binding, distinct-program output, identical-program reuse, StaticMesh/SkeletalMesh/Terrain parity, preview/thumbnail, forward/GBuffer/shadow, fallback, reload, and device recovery |
| Bounds and diagnostics | Named graph/IR/source/dependency/stage/artifact/string limits; asset and stable node/input/output locations; validation/generation/compiler/reflection/binding categories |
| Compatibility | Canonical material visual parity or explicit migration/rejection, package strictness, instance overrides, shader/pass ABI stability, ErrorMaterial independence, and no silent program substitution |
| Qualification and M6 handoff | Focused and aggregate tests, full build, editor smoke, representative hardware/profile, cold/warm timings, generated/artifact bytes, dependency counts, failure modes, and exact immutable API/result evidence |

Use the repository [build and run](../../../Agents/BuildAndRun.md) and
[testing](../../../Agents/Testing.md) workflows to select profiles and commands.
Stage handoffs record exact targets, filters, timings, hardware, cache state,
and any environment-dependent measurements rather than embedding commands here.

## Definition of Done

- `DMaterial` can persist one versioned bounded typed surface program with
  stable parameter/node identity, strict validation, deterministic round trip,
  and explicit canonical fixed-content transition.
- Validation produces one immutable value-owned compiler input; normalization,
  generated source, dependency closure, and program identity are deterministic
  and exclude presentation metadata and dynamic values.
- One synchronous compiler entry point returns a complete pass-compatible
  compiled result or bounded diagnostic without accessing reflected/live owner
  state and without duplicating RenderCore shader artifacts.
- Program identity selects transactional Renderer shader resources while the
  exact v3 material layout, shared surface ABI, role fallbacks, pass policy,
  ErrorMaterial, reload, and recovery boundaries remain intact.
- Two materially distinct programs compile and render through StaticMesh,
  SkeletalMesh, Terrain, preview, thumbnail, forward, GBuffer, and applicable
  shadow consumers; invalid programs fail deterministically.
- Stage 0 limits and measured compile/artifact/dependency/failure evidence pass,
  lasting contracts are updated, the roadmap records M5 completion, and M6 has
  all required handoff inputs.

## Deferred Follow-ups

- Cancelable asynchronous requests, latest-generation publication, single-
  flight material consumers, last-known-good asset state, material DDC, Cook,
  cooked load, reload qualification, and shutdown belong to M6.
- Graph canvas, node/pin editing, diagnostic navigation, copy/paste, and graph-
  specific Undo/Redo belong to M7.
- Runtime-only dynamic material instances and profiling-selected allocation,
  batching, uniform/resource reuse, and descriptor policy belong to M8.
- Material functions, reusable subgraphs, arbitrary source snippets, new
  surface outputs, custom passes, bindless resources, compute materials,
  decals, post-process materials, and ray tracing require separate gated work.

## Related Documentation

- [Material System Roadmap](../../../Roadmaps/MaterialSystem.md)
- [Material System](../../../Runtime/Rendering/MaterialSystem.md)
- [Shader Cache](../../../Runtime/Rendering/ShaderCache.md)
- [Shader Parameters](../../../Runtime/Rendering/ShaderParameters.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Resource Lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../../../Runtime/Rendering/SkeletalMeshRendering.md)
- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials`
- `Engine/Source/Runtime/Engine/Private/Materials`
- `Engine/Source/Runtime/RenderCore/Public/Shader`
- `Engine/Source/Runtime/RenderCore/Private/Shader`
- `Engine/Source/Runtime/Renderer/Private/Renderers`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/EngineTests/Private/Materials`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
