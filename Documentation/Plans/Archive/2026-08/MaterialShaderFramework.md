# Material Shader Framework Plan

Summary: Introduce typed Material and mesh-Material shader categories while preserving existing Material identities, cache keys, and lifecycle behavior.

Last reviewed: 2026-08-30

Status: Archived
Completed: 2026-08-30

## Current Status

All stages are complete. The migration baseline is the 2026-08-30
`Win64-Debug-DurinEditor` profile: `RenderShaderContractTests` passed 43/43,
`MaterialTests` passed 98/98, `RendererResourceReloadVulkanTests` passed 1/1,
and `StaticMeshRenderPreparationVulkanTests` passed 1/1. The selected identity
and ownership decomposition is recorded below. RenderCore now owns the narrow
Material identity header, typed Material shader registration, exact-map build
and validation, deterministic compatibility identity, and strongly retained
typed refs. `RenderShaderContractTests` now pass 47/47 and the unchanged
`MaterialTests` pass 98/98. Generated Surface fragments and all production
mesh stages now use the typed Material map API; renderer-local caches are
bounded and retain no authored asset.

Final focused evidence is `RenderShaderContractTests` 47/47,
`MaterialTests` 98/98, `EditorRenderingTests` 75/75, and passing Material,
StaticMesh, SkeletalMesh, Terrain, renderer-resource reload, and Vulkan RHI
targets. The skeletal visual fixture now installs and awaits a canonical
Material program before asserting its authored color. The first aggregate
build attempt hit an unrelated 30-second CMake discovery timeout while linking
`CoreObjectTests`; that target then passed 85/85 in isolation and the incremental
`test all` gate passed all 84 registered non-characterization/non-qualification
targets. The default `all` build and changed/all/all-plan documentation
validators passed. A concurrent `GBufferQualificationTests` diagnostic run
completed its correctness work but exceeded host GPU timing thresholds; under
the repository testing contract that non-exclusive timing sample is not an
authoritative regression or acceptance measurement.

This plan selects the semantic hierarchy:

```text
FShader
|-- FGlobalShader
`-- FMaterialShader
    `-- FMeshMaterialShader
```

It does not select one process-wide atomic Material map. The completed
evidence gate keeps the existing outer cache keys and exact demanded shader
sets in bounded renderer-local slot caches; RenderCore's lower resource cache
provides code/RHI sharing without a second Material ownership hierarchy.

## Goal

Provide typed Material shader registration, compilation identity, lookup,
strong lifetime, generation compatibility, and failure recovery so consumers
do not allocate, initialize, cast, or manually pair ordinary shader maps for
Material programs. Extend that foundation with a mesh-specific category for
the exact `Material x VertexFactory x MeshPass x permutation` domain without
moving PSO, mesh draw, or pass-processing responsibilities into shader maps.

## Scope

- RenderCore-owned `FMaterialShader`, `FMaterialShaderType`,
  `FMaterialShaderMap`, and `TMaterialShaderRef<T>` vocabulary, plus a narrow
  value-only Material shader identity boundary built on the existing compiler,
  reflection, parameter metadata, resource code, and RHI shader implementation.
- Renderer adapters that translate Engine's accepted immutable Material
  compiler results and render snapshots into exact RenderCore map candidates,
  then own cache partitioning, generation fan-out, diagnostics, and lifecycle.
- Explicit Material permutation identity derived from the existing
  `FMaterialShaderMapIdentity`, shader type, Material pass/entry point, target,
  and local permutation identifier without changing the existing persisted
  shader-cache key during the initial migration.
- Strongly retained exact shader-map sets and pipeline layouts, plus checks
  that the compiled program, shader map, pipeline, and immutable Material
  render snapshot describe the same accepted identity.
- Migration of Surface Material generated fragments before mesh/vertex-factory
  types.
- `FMeshMaterialShader` and mesh permutation identity for Local, Spline,
  Skeletal, Terrain, GBuffer, base-pass, and shadow combinations.
- Integration with asynchronous Material compilation, shader reload, manual
  retry, last-known-good behavior, ErrorMaterial fallback, device invalidation,
  render-proxy publication, asset unload, and Renderer shutdown.
- Focused type, identity, cache, lifecycle, recovery, and Vulkan parity tests.

## Non-Goals

- Fixed fullscreen, debug, editor-assistance, lighting, post-process, or other
  non-Material programs; they continue to use `FGlobalShader`.
- TextureEditor preview and Mona ImGui backend shader ownership.
- A renderer-wide graphics or compute PSO cache, PSO precaching, or PSO
  persistence.
- Mesh draw-command construction, sorting, pass processing, visibility,
  batching, vertex-stream binding, or Render Graph pass ownership.
- Compiling every Material, Vertex Factory, mesh pass, or permutation as one
  atomic map or eagerly at startup.
- Changing authored Material graphs, parameter schemas, cooked Material
  payloads, or the current Material-program compiler pass contract unless an
  explicit compatibility failure proves a schema change is required.
- Replacing the existing asynchronous Material compilation domain or creating
  a second Material DDC owner.

## Design Decisions and Invariants

### Ownership and dependency direction

- `FShader`, ordinary compilation, shader resource code, RHI shader creation,
  `FMaterialShader`, `FMeshMaterialShader`, their type metadata, typed exact-map
  payload, and the narrow Material shader identity are RenderCore-owned.
- `FMaterialShaderMapIdentity` is not inherently an Engine type. Move or
  extract it, `FMaterialProgramIdentity`, `FMaterialRenderLayoutIdentity`, and
  only the minimum non-reflected render-facing value vocabulary into RenderCore.
  Do not move authored objects, reflected Material schemas, compiler lifecycle,
  render proxies, or parameter values with it.
- Engine's reflected `EMaterialBlendMode` and `EMaterialShadingModel` remain
  authored Material vocabulary unless reflection tooling proves they can move
  without adding a CoreDObject dependency to RenderCore. An explicit, exhaustive
  conversion produces the RenderCore shader-facing enum/key values and is
  covered by compatibility tests.
- Engine continues to own authored Material assets, asynchronous compilation,
  accepted `FMaterialCompilerResult` values, ErrorMaterial data, and counted
  render proxies. Engine depends downward on the RenderCore identity header.
- Renderer owns the adapter from Engine compiler/render snapshots to generic
  RenderCore compiled-stage inputs, exact-set cache storage, generation fan-out,
  failure diagnostics, and pipeline consumption. RenderCore does not include an
  Engine header, and Engine does not depend on Renderer.
- `FMaterialShader` binds Material identity and parameter-layout semantics;
  Renderer map storage and the existing Engine/Renderer lifecycle owners
  perform publication, retry, invalidation, and release. The base shader
  instance and RenderCore payload do not become service locators or lifecycle
  coordinators.

### Shader taxonomy

| Category | Compile identity | Initial examples |
| --- | --- | --- |
| `FGlobalShader` | Fixed shader type and exact global set | Fullscreen, debug, lighting, editor assistance |
| `FMaterialShader` | Material program/layout, Material pass entry point, shader type, and local permutation | Surface forward, generated GBuffer, masked-shadow fragments |
| `FMeshMaterialShader` | Material shader identity plus Vertex Factory type, mesh pass, mesh-stage permutation, and shader frequency | Static/Spline/Skeletal/Terrain and GBuffer mesh-stage types |
| Plain `FShader` | Lower-level or intentionally feature-local identity | `FShaderMapBase` foundation, TextureEditor preview, Mona ImGui |

Shader frequency remains `FShaderType` metadata. A Material pass entry point
such as `FragmentMain`, `GeometryFragmentMain`, or `ShadowFragmentMain` is part
of Material permutation compatibility. A mesh pass such as base, GBuffer, or
shadow and its Vertex Factory identity enter only at the
`FMeshMaterialShader` layer.

### Identity and cache compatibility

- The relocated RenderCore `FMaterialShaderMapIdentity` remains the
  authoritative Material code/layout identity: render layout, compiled program
  digest, shader-facing blend mode, shader-facing shading model, and opacity-
  mask threshold. Relocation changes header ownership, not equality, hashing,
  serialized/cooked meaning, or cache-key bytes.
- `FMaterialProgramIdentity` and `FMaterialRenderLayoutIdentity` are immutable
  shader-facing value identifiers and move with the identity boundary. Engine
  keeps the compiler, concrete render layout table, authored enums, and
  validation/build logic that produce and consume those identifiers.
- The framework adds a non-persisted exact-set compatibility identity from the
  Material identity, requested shader types, Material pass/entry points,
  platform/target, and permutation identifiers. It validates publication and
  retained references; it does not silently revise the lower-level shader DDC
  schema.
- Mesh compatibility extends that exact identity with a stable Vertex Factory
  type, mesh pass, and mesh permutation. Runtime Vertex Factory object pointers,
  vertex buffers, vertex declarations, and stream layouts are draw/pipeline
  resources and do not enter Material shader-map identity.
- RenderCore represents mesh pass/permutation with a stable shader-facing key;
  Renderer exhaustively maps private pass enums such as `EMeshBasePass` and
  GBuffer/shadow policy into that key. RenderCore does not include Renderer pass
  declarations.
- Existing `FMaterialShaderMapIdentity`, `FMeshShaderMapKey`, and
  `FGBufferShaderMapKey` outer cache behavior remains unchanged through the
  initial migrations. Any later canonical-key replacement requires measured
  equivalence, collision tests, and an explicit decision recorded in this
  plan.
- `FMaterialPlanningPassIdentity`, rasterizer/depth/blend state, render-target
  layout, vertex declaration, and device generation remain pipeline identity,
  not Material shader identity.

### Exact sets, typed lookup, and lifetime

- RenderCore's `FMaterialShaderMap` initially represents one exact demanded compatible set,
  not a global registry containing every Material permutation. Separate
  Material identities, passes, and Vertex Factories may publish independently.
- During the bounded Surface pilot, a map may wrap the existing combined
  vertex/fragment `FShaderMapBase` while typed Material lookup is introduced.
  Transitional plain-vertex access is removed when Stage 3 lands the mesh
  category.
- `TMaterialShaderRef<T>` accepts only `FMaterialShader`-derived types, retains
  the owning map payload strongly, and exposes the exact identity, generation,
  reflection, and RHI shader used by its pipeline. It cannot be constructed by
  casting an arbitrary `FShader*`.
- A pipeline payload retains the exact typed refs and merged layout used during
  creation. Draw-time parameter binding uses those retained refs rather than
  querying a newer map after Material or shader publication.
- A draw retains an immutable `FMaterialRenderData` snapshot or equivalent
  exact binding snapshot. Dynamic proxy publications may reuse a shader and PSO
  when `FMaterialShaderMapIdentity` is unchanged, but a draw may not pair
  bindings from one identity with shader refs from another.

### Compilation and fallback

- A generated Material stage is selected from the accepted immutable
  `FMaterialCompilerResult`; Renderer does not recompile authored IR or own a
  second generated-program cache.
- Renderer verifies that an Engine compiler result succeeded, its identity
  equals `FMaterialShaderMapIdentity::ProgramIdentity`, its target and pass-
  contract envelope are current, and the requested entry point exists before
  producing generic compiled-stage input. RenderCore then verifies type/stage
  membership, reflection, parameter bindings, exact identity, and merged layout
  without knowing `FMaterialCompilerResult`.
- Fixed geometry/mesh stages continue through the ordinary RenderCore compiler
  and combine with generated Material artifacts only as one exact candidate.
  Candidate compilation, binding, merged-layout construction, and optional RHI
  creation are complete-or-null.
- A failed asynchronous Material compile cannot replace Engine's accepted
  last-known-good compiler result. A failed same-device map refresh retains the
  compatible published map through the existing resource-slot policy. A
  Material with no accepted program resolves through the explicit ErrorMaterial
  path rather than publishing a partial generated set.
- The fixed ErrorMaterial shader set has an explicit sentinel/default identity
  and is validated like any other exact set. Error fallback does not masquerade
  as the failed requested Material identity.

### Generations, reload, unload, and shutdown

- Shader reload and manual retry generations remain explicitly supplied by
  `FRendererResourceCoordinator`. Material dependency generations remain owned
  by the asynchronous Engine compilation domain. Neither owner reaches through
  a global service locator into the other.
- The published map/ref generation is the generation used to resolve a
  dependent pipeline. Same-device last-known-good retention keeps its matching
  pipeline; a successfully refreshed map makes one new pipeline attempt
  eligible. Device invalidation retains no RHI shader or PSO fallback across
  the device generation.
- Render-proxy local/resolved versions control immutable binding snapshot
  publication. They do not force shader recompilation when the resolved
  Material shader identity is unchanged.
- Material maps never retain `DMaterial`, `DMaterialInstance`, or a borrowed
  render-proxy pointer. Asset unload transfers the final proxy release through
  the rendering thread; previously recorded draws remain safe through counted
  proxy/snapshot and map refs. Shared compiled artifacts and shader maps may
  remain in bounded caches until eviction, invalidation, or shutdown without
  keeping the asset resident.
- Shutdown closes render-command admission, releases draw/pipeline consumers,
  clears Material/mesh map caches, releases render proxies and RHI resources,
  and only then permits Renderer, Engine compilation, RenderCore, and RHI
  teardown in their existing dependency order.

## Implementation Stages

### Stage 0 Decisions and Baseline

- Production ordinary-map initialization is limited to GBuffer, StaticMesh,
  SkeletalMesh, Terrain, and `InitializeCompiledMaterialShaderMap`. The
  `FGlobalShaderMap` call remains a global implementation detail, while Mona
  ImGui remains intentionally feature-local. TextureEditor has no production
  Material-map initialization call.
- `FSurfaceFragmentShader`, `FGBufferFragmentShader`, and
  `FSurfaceMaskedShadowFragmentShader` are intrinsic Material shaders.
  Local/Spline/Skeletal/Terrain vertex stages are mesh Material shaders.
  `FSurfaceOpaqueShadowFragmentShader` is a fixed, Material-resource-free mesh
  stage: its set compatibility includes mesh pass and Vertex Factory, but it
  does not claim a generated Material-program entry point.
- Initial outer cache partitioning remains exact: GBuffer uses
  `FMaterialShaderMapIdentity x EGBufferVertexDomain`; StaticMesh uses
  `FMaterialShaderMapIdentity x EVertexDeformationDomain`, split again between
  forward and shadow caches; SkeletalMesh uses `FMaterialShaderMapIdentity`,
  split between forward and shadow caches; Terrain uses
  `FMaterialShaderMapIdentity`, also split between forward and shadow caches.
  Pipeline keys continue to add raster/depth/blend state, vertex declaration or
  topology, render-target layout, and device generation.
- Compile options remain byte-compatible. Every Material path supplies
  `DURIN_MATERIAL_BLEND_MODE`, `DURIN_MATERIAL_SHADING_MODEL`, and
  `DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS`; Spline, Skeletal, and Terrain
  add their existing domain macro; opaque shadow adds
  `DURIN_OPAQUE_SHADOW_DEPTH`. Accepted generated stages remain
  `FragmentMain`, `GeometryFragmentMain`, and `ShadowFragmentMain`; fixed
  opaque shadow remains `OpaqueShadowFragmentMain`.
- The narrow RenderCore boundary contains `FMaterialProgramIdentity`,
  `FMaterialRenderLayoutIdentity`, shader-facing blend/shading keys, and exact
  Material/mesh permutation identities. Engine retains reflected authored
  enums, render-layout tables, compiler envelopes/results, and exhaustive
  authored-to-shader conversions. The shader-facing enums use the same `uint8`
  values as their authored counterparts, preserving existing comparison and
  cache-key bytes.
- Vertex Factory identity uses a registered static `FVertexFactoryType`
  descriptor with a stable name/key. Runtime factory addresses, RHI vertex
  declarations, buffers, stream layouts, and topology remain pipeline/draw
  state and never enter the shader identity.
- Exact Material identity is
  `Material map identity x shader type x entry point x target x local
  permutation`; mesh identity adds `Vertex Factory type x mesh-pass key x
  mesh permutation x frequency`. Equality and ordering compare those fields;
  deterministic hash/text serialization follows the same order. These
  compatibility values wrap the existing `FShaderMapBase` cache result and do
  not alter `BuildShaderMapCacheKey` inputs or persisted shader-cache schema.
- Baseline lifecycle behavior is complete-or-null candidate publication,
  same-device last-known-good slot retention, shader-generation fan-out to the
  dependent pipeline generation, no fallback across device generation, and
  cache reset during renderer release. The reload Vulkan baseline proves
  failed refresh retention, changed/all reload, manual retry, device
  invalidation, cache hit without a native pipeline recreation, bounded
  occupancy, and shutdown. Material and StaticMesh baselines cover
  ErrorMaterial selection, proxy republish/unload safety, first-demand
  creation, reuse, Vulkan output, and teardown. Renderer cache cardinality is
  one shader-map slot and one pipeline slot per exact demanded outer key;
  forward and shadow caches remain independently bounded.

### Stage 0: Freeze taxonomy, ownership, identities, and baseline

- [x] Inventory every remaining production `InitializeFromShaderTypes()` call
  and every Surface/GBuffer/base-pass/shadow shader type; classify each as
  global implementation detail, intrinsic Material, mesh Material, or
  intentionally feature-local.
- [x] Record the exact current outer cache keys, compile options/macros,
  generated entry-point selection, merged layouts, shader/pipeline generation
  rewriting, ErrorMaterial behavior, and release order for GBuffer,
  StaticMesh, SkeletalMesh, and Terrain.
- [x] Select the minimal stable Vertex Factory type metadata required by shader
  identity. Prefer a registered/static descriptor layered beside the existing
  runtime `FVertexFactory`; do not use instance pointers or RHI declarations as
  compile identity.
- [x] Freeze the identity extraction boundary: list every dependency of
  `FMaterialShaderMapIdentity`, move only value-only shader identifiers to
  RenderCore, define exhaustive conversions from Engine's reflected authored
  enums, and prove RenderCore gains no Engine/CoreDObject dependency.
- [x] Resolve the opaque-shadow special case explicitly: classify fixed,
  Material-resource-free stages by their actual compile/set compatibility and
  avoid pretending they are intrinsic Material programs.
- [x] Define exact Material and mesh permutation identity structs, hashing,
  diagnostic text, ordering, and equality while proving that initial outer
  cache and RenderCore shader-cache keys remain byte-for-byte unchanged.
- [x] Capture cold/warm compile counts, cache hits, map/pipeline occupancy,
  first-demand behavior, changed/all reload, manual retry, failure fallback,
  device invalidation, proxy republish, asset unload, Vulkan output, and
  shutdown baselines for one Surface and one mesh path.

#### Acceptance Gate

- Every affected shader type and direct initialization call has one selected
  category and owner, and the selected identity decomposition distinguishes
  Material, Vertex Factory, mesh pass, permutation, pipeline, and draw state
  without circular module dependencies or a cache-schema change.
- The pilot baseline is reproducible and includes generation, failure, unload,
  and rendering evidence needed to detect semantic drift.

### Stage 1: Add typed Material shader and exact-map foundations

- [x] Relocate `FMaterialShaderMapIdentity`, `FMaterialProgramIdentity`,
  `FMaterialRenderLayoutIdentity`, and the selected minimal shader-facing enums
  or keys into a narrow RenderCore header; adapt Engine construction and hashing
  without changing identity semantics or cache bytes.
- [x] Add RenderCore-owned `FMaterialShader` and `FMaterialShaderType` on top of
  `FShader`/`FShaderType`, including Material-aware permutation and compilation-
  environment hooks without duplicating RenderCore compiler machinery.
- [x] Add one declaration/implementation registration vocabulary for Material
  shader classes with deterministic type identity, exactly-one definition,
  parameter metadata composition, and safe defining-module lifetime.
- [x] Implement `FMaterialShaderMap` as a strongly owned exact-set wrapper over
  the existing shader-map resource foundation. Store the exact Material
  identity, compatibility identity, published generation, merged pipeline
  layout, and compiled-stage resource ownership needed for validation.
- [x] Implement `TMaterialShaderRef<T>` with compile-time category checks,
  const typed lookup, strong map retention, exact identity/generation access,
  and deterministic missing/wrong-category diagnostics.
- [x] Centralize candidate validation for accepted generated stages, fixed
  ErrorMaterial stages, reflection/parameter layouts, target/pass envelope,
  RHI creation, and atomic publication.
- [x] Prove type registration, duplicate rejection, wrong-category lookup,
  missing entry point, mismatched program identity, invalid reflection,
  strong lifetime, set-order independence, and no accessor side effects with
  focused unit tests.

#### Acceptance Gate

- One exact Material set can be built and queried through typed Material APIs
  without a consumer casting `FShader*`, and a retained ref remains valid for
  the same lifetime and identity as its owning map.
- RenderCore remains independent of Engine, CoreDObject, and Renderer; Engine
  consumes the narrow lower-level identity header, and the initial framework
  changes neither authored/cooked Material formats nor persisted shader-cache
  identity.

### Stage 2: Migrate intrinsic Surface Material shader types

- [x] Convert generated Surface forward, generated GBuffer, and masked-shadow
  fragment classes to `FMaterialShader`/`FMaterialShaderType`; keep shader
  virtual paths, entry points, frequencies, parameter metadata, and compile
  macros unchanged.
- [x] Route generated `FragmentMain`, `GeometryFragmentMain`, and
  `ShadowFragmentMain` selection through `FMaterialShaderMap` validation and
  typed lookup. Preserve the fixed ErrorMaterial fallback path and classify
  the opaque-shadow exception according to Stage 0.
- [x] Add the Renderer adapter that validates accepted
  `FMaterialCompilerResult` envelopes and emits generic compiled-stage inputs
  for RenderCore without exposing Engine compiler types in RenderCore headers.
- [x] Replace fragment casts and manually paired map/ref members in GBuffer,
  StaticMesh, SkeletalMesh, Terrain, and the shared helper with
  `TMaterialShaderRef<T>` while leaving transitional mesh-stage access bounded
  to this stage.
- [x] Make every pipeline payload retain the exact Material map/ref set and
  merged layout used to create it; remove manual shader-generation rewriting
  only where the typed map exposes the equivalent exact generation.
- [x] Verify that existing `FMaterialShaderMapIdentity`,
  `FGBufferShaderMapKey`, and per-renderer resource-slot counts, lookup
  partitioning, cache hits, compile requests, and diagnostics remain unchanged.
- [x] Run focused default/error/custom Material, forward/GBuffer/masked-shadow,
  reload/retry, generated-stage failure, binding failure, proxy republish,
  device invalidation, and Vulkan parity coverage.

#### Acceptance Gate

- Every generated Surface fragment is a typed Material shader and no migrated
  consumer casts it from an ordinary map. Exact accepted program identity,
  parameter layout, map generation, pipeline generation, and draw binding
  snapshot agree at submission.
- The Surface pilot preserves cache keys, compile counts, first-use behavior,
  ErrorMaterial fallback, reload recovery, device recovery, and rendered output.

### Stage 3: Add mesh Material shaders and migrate mesh-stage permutations

- [x] Add `FMeshMaterialShader` derived from `FMaterialShader` and a matching
  `FMeshMaterialShaderType` or equivalent specialized metadata that composes
  Material, stable Vertex Factory type, mesh pass, shader frequency, and local
  permutation identity.
- [x] Add the selected Vertex Factory type descriptor and registration for
  Local, Spline, Skeletal, and Terrain without changing runtime vertex-buffer,
  declaration, stream, or `FRenderResource` ownership.
- [x] Extend typed lookup so `TMaterialShaderRef<T>` safely retains and queries
  `FMeshMaterialShader`-derived stages from an exact compatible mesh set; add a
  narrower alias only if it materially improves diagnostics or API clarity.
- [x] Migrate StaticMesh, SplineMesh, SkeletalMesh, Terrain, and GBuffer vertex
  types and any Stage 0-classified mesh-only shadow stages. Preserve current
  Material/vertex-domain outer keys and compile macros during each slice.
- [x] Replace duplicated `InitializeCompiledMaterialShaderMap()` assembly,
  ordinary `InitializeFromShaderTypes()` calls, raw `GetShader()` lookups, and
  typed casts in the Material/mesh production paths with one exact-set builder.
- [x] Keep base, GBuffer, masked shadow, opaque shadow, Local, Spline, Skeletal,
  and Terrain demands independently publishable; do not make unrelated
  permutations fail or compile atomically together.
- [x] Prove Vertex Factory type/permutation separation, exact vertex/fragment
  compatibility, layout merging, warm reuse, feature-local failure, and visual
  parity across StaticMesh, SkeletalMesh, Terrain, GBuffer, and shadow tests.

#### Acceptance Gate

- The Material/mesh production paths contain no consumer-owned ordinary
  shader-map initialization, raw shader lookup, or shader cast. All stages are
  obtained from one typed exact-set vocabulary and retained with the pipeline
  that consumes them.
- Material identity changes, Vertex Factory changes, mesh-pass changes, and
  pipeline-only state changes invalidate only their documented cache layer.

### Stage 4: Integrate compilation, reload, fallback, unload, and shutdown

- [x] Connect Engine's accepted immutable compiler-result publication through
  the Renderer adapter to typed Material-map demand using identity/artifact
  snapshots; never capture a live `DObject` or borrowed container in render or
  compile work.
- [x] Prove that authored edits and dependency reload keep the accepted
  last-known-good program/render proxy visible until a complete new program and
  compatible map publish, then switch identity atomically for later draws.
- [x] Integrate changed/all shader reload, manual retry, same-generation
  suppression, same-device map fallback, pipeline recreation, and one-layer
  diagnostics without compiling generated Material IR a second time.
- [x] Validate ErrorMaterial selection for missing/failed first compile and
  retain explicit provenance so diagnostics distinguish requested identity,
  attempted map, retained last-known-good identity, and ErrorMaterial fallback.
- [x] Exercise asset unload, instance-parent changes, proxy replacement,
  queued/recorded draw lifetime, cache eviction, device invalidation, Renderer
  shutdown, Engine compilation shutdown, and restart; verify no asset residency
  leak, stale ref, cross-device RHI fallback, or callback leak.
- [x] Bound retained exact maps and immutable compiled results with existing or
  selected cache budgets and expose occupancy/failure counters sufficient to
  diagnose retention without per-asset history.

#### Acceptance Gate

- Compile success, failure, cancellation, supersession, hot reload, retry,
  proxy publication, unload, device loss, shutdown, and restart preserve one
  compatible Material snapshot/map/pipeline generation at every draw boundary.
- No Material shader map owns a reflected asset or render proxy, and no failed
  or stale candidate replaces a complete accepted payload.

### Stage 5: Decide hierarchical Material map storage

- [x] Measure duplicate fixed-vertex compilation, generated-stage/resource
  retention, map count, first-use latency, reload work, failure isolation, and
  eviction behavior across GBuffer, StaticMesh, SkeletalMesh, and Terrain after
  typed migration.
- [x] Compare the retained renderer-local exact-set caches with a Renderer-owned
  UE-style storage hierarchy in which a Material-level owner partitions bounded
  `FMeshMaterialShaderMap` children by Vertex Factory, mesh pass, and
  permutation.
- [x] Introduce the hierarchical storage only if it reduces demonstrated
  duplication or enables required sharing without widening failure domains,
  changing public cache identity, retaining assets, or compiling all
  permutations atomically.
- [x] If local exact-set caches remain the better boundary, record the measured
  decision and keep `FMaterialShaderMap` as the common typed payload/API rather
  than adding an unused hierarchy.
- [x] Keep PSO caches, mesh draw commands, pass processing, and vertex resource
  ownership outside either map design.

#### Storage Decision

Keep renderer-local exact-set caches. The migrated paths retain one map per
existing outer key and independently demand forward, GBuffer, shadow, Vertex
Factory, and deformation variants. Identical fixed-stage requests already
share compiled output, resource code, and RHI shaders through RenderCore's
unchanged lower caches, so a second Material/child-map owner would not remove
the remaining exact-set or PSO payloads. It would instead couple failure,
reload, and eviction across independently publishable families.

Each local shader-map cache admits at most 256 exact entries and each pipeline
cache at most 512; forward and shadow caches remain independent. Existing hits
retain stable indices and insertion beyond the budget evicts the oldest slot.
Maps and pipelines strongly retain value identities, compiled resources, typed
refs, and merged layouts, but no `DMaterial`, instance, or render proxy. The
focused cache test proves bounded insertion, stable hits, and oldest-entry
eviction. This preserves the measured baseline partition and provides the one
selected storage path without introducing speculative hierarchy.

#### Acceptance Gate

- One evidence-backed storage model owns Material and mesh Material shader
  sets. Its partition, eviction, failure, generation, and lifecycle rules are
  documented and tested; no speculative second ownership path remains.

### Stage 6: Qualify, document, and retire transitional paths

- [x] Remove bounded Stage 2 compatibility access, obsolete combined-map
  helpers, duplicate diagnostic wrappers, manual generation plumbing, and dead
  private payload members after all migrations pass.
- [x] Run the smallest registered shader, Material, render-proxy, Renderer
  resource, GBuffer, StaticMesh, SkeletalMesh, Terrain, RHI, Vulkan, reload,
  lifecycle, and application targets selected through the repository testing
  workflow, followed by the required aggregate/build tier.
- [x] Update Material System, Global Shader category boundaries, Shader Cache,
  Renderer Resource Recovery, Code Modules, and pass-specific contracts to the
  implemented ownership and failure behavior.
- [x] Record exact compile/cache, map/pipeline occupancy, reload/retry,
  fallback, unload, device, visual, lifecycle, build, test, and documentation
  evidence in this plan.
- [x] Complete lifecycle metadata and repository-required plan/stage commit
  provenance only after every acceptance gate passes.

#### Acceptance Gate

- Source, tests, diagnostics, cache behavior, lifecycle ordering, and lasting
  documentation agree on one typed Material/mesh Material shader architecture
  with no consumer-owned ordinary map compatibility path.
- Repeated compilation, publication, rendering, reload, retry, unload, device
  invalidation, shutdown, and restart leave no stale shader ref, incompatible
  pipeline, retained asset, live RHI payload, or duplicate registration.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Type system | Material/global/plain category separation, registration uniqueness, parameter metadata, typed lookup, invalid category rejection |
| Material identity | Existing shader-map identity preserved, exact program/layout/entry-point/target checks, deterministic hashing and diagnostics |
| Mesh identity | Stable Vertex Factory type, mesh pass and permutation separation, no runtime instance/RHI pointer in shader identity |
| Exact sets | Independent bounded demands, deterministic membership, complete-or-null publication, compatible merged layout |
| Compilation | Generated-stage reuse, fixed-stage compile, cold/warm cache, no second Material DDC, unchanged initial cache keys |
| Strong lifetime | Map-retained refs, pipeline-retained exact stages/layout, recorded draw safety across refresh and unload |
| Proxy coupling | Dynamic-only publication reuses shader/PSO; identity publication switches later draws atomically; no mixed binding identity |
| Recovery | Compile/binding/RHI failure, suppression, last-known-good program/map, ErrorMaterial provenance, manual retry, one recovery transition |
| Device lifecycle | No RHI fallback crosses device generation; lazy reconstruction and ordered shutdown/restart succeed |
| Asset lifecycle | Base/instance unload, parent replacement, proxy release, cache eviction, no `DObject` retention or stale callback |
| Migration | No direct map initialization/raw lookup/cast in migrated Material/mesh consumers; feature-local exceptions remain explicit |
| Performance | Compile requests, first-use latency, code/RHI/result retention, map/pipeline count, reload work, and diagnostics are bounded |
| Rendering | Forward/GBuffer/shadow, Opaque/Masked/Translucent, Local/Spline/Skeletal/Terrain, and Vulkan output match baseline |
| Documentation | Changed-document and all-plan validators pass; lasting contracts replace plan-only architecture text at completion |

## Definition of Done

- RenderCore exposes the selected typed Material and mesh Material shader
  categories, registration, identity, exact-map lookup, and strong refs;
  Renderer supplies Engine adaptation, cache storage, lifecycle integration,
  pipeline consumption, and diagnostics.
- Surface and mesh consumers no longer allocate, initialize, cast, retain, or
  manually generation-pair ordinary shader maps.
- Exact immutable Material data, compiled program, shader set, merged layout,
  pipeline, and recorded draw remain compatible across successful refresh,
  failed refresh, retry, unload, device invalidation, and shutdown.
- Existing shader/material cache identity and behavior remain stable unless a
  separately recorded and validated Stage 5 decision explicitly changes them.
- Fixed non-Material shaders remain on `FGlobalShader`; PSO, mesh draw, and pass
  processing remain outside the Material shader framework.
- Lasting Engine/RenderCore/Renderer documentation matches the implemented
  boundaries, validation evidence is recorded, and changes are committed with
  required plan/stage provenance.

## Deferred Follow-ups

- Renderer-wide PSO caching and precaching.
- Asynchronous Material shader-map prewarming beyond existing Material program
  compilation.
- Plugin hot-unload or runtime retirement of registered Material shader types.
- Multiple simultaneous shader platforms/RHIs or cross-platform cooked map
  bundles.
- General registered shader-pipeline types beyond the exact Material/mesh sets
  required here.

## Related Documentation

- [Material System](../../../Runtime/Rendering/MaterialSystem.md)
- [Global Shaders](../../../Runtime/Rendering/GlobalShaders.md)
- [Shader Cache](../../../Runtime/Rendering/ShaderCache.md)
- [Shader Parameters](../../../Runtime/Rendering/ShaderParameters.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Resource Lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Material System Roadmap](../../../Roadmaps/MaterialSystem.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/GlobalShader.h`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramCompiler.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderProxy.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SurfaceMaterial.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/MeshRendererShared.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/MeshRenderPreparationCommon.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderProxyTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
