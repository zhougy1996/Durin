# Global Shader Framework Plan

Summary: Introduce UE-aligned global shader types, maps, typed lookup, and generation-aware recovery for fixed renderer shaders.

Last reviewed: 2026-08-29

Status: Active
Completed:

## Current Status

RenderCore already registers typed `FShaderType` values, compiles
`FShaderMapBase` instances through the shared shader cache, lazily creates RHI
shaders, and exposes `TShaderRef<T>`. Renderer resource slots add transactional
generation, retry, fallback, diagnostics, and device invalidation around those
primitives. Fixed renderers nevertheless compile and retain their own shader
maps, repeat typed lookup and binding checks, and manually propagate the
published shader generation into dependent pipeline slots.

This plan selects a RenderCore-owned global-shader domain for fixed,
non-Material shaders. It preserves Durin's feature-local failure isolation and
last-known-good behavior instead of adopting Unreal Engine's fatal global-
shader assumption. The Primitive Draw Interface plan consumes this foundation
for its simple-element shaders after Stages 0-2 are accepted.

## Goal

Provide one UE-aligned `FGlobalShader` and `FGlobalShaderMap` path through which
fixed renderer passes register shader types, resolve typed shader references,
refresh lazily, retain valid fallback generations, invalidate device resources,
and publish diagnostics without owning private `FShaderMapBase` compilation
state.

## Scope

- RenderCore-owned global shader type registration, map storage, typed lookup,
  generation identity, compile/binding diagnostics, and explicit lifecycle.
- Public `FGlobalShader`, `FGlobalShaderType`, `FGlobalShaderMap`,
  `GetGlobalShaderMap()`, and `TShaderMapRef<T>` concepts aligned with Unreal
  Engine where Durin's module and recovery contracts permit.
- `DURIN_DECLARE_GLOBAL_SHADER` and `DURIN_IMPLEMENT_GLOBAL_SHADER` macros that
  retain the repository macro prefix while separating declaration from one
  translation-unit registration definition.
- Lazy changed/all reload, manual retry, complete-or-null publication,
  last-known-good retention, and device-generation invalidation.
- Typed multi-stage resolution and pipeline-layout construction that retain the
  exact shader references and published generations used by a pipeline.
- Migration of fixed Renderer shader families, beginning with EditorAssistance
  and bounded fullscreen/debug passes, followed by the remaining eligible
  fixed passes.
- Focused compile, cache, reload, failure-injection, Vulkan, lifecycle, and
  migration-parity coverage.

## Non-Goals

- Material shader maps, compiled material identities, vertex-factory
  permutations, mesh pass processing, or authored Material behavior.
- A process service locator for `FRendererResourceCoordinator`; Renderer keeps
  explicit command admission and fans accepted generations into RenderCore.
- A renderer-wide graphics PSO cache. Global shader references remove shader-map
  ownership duplication, but PSO identity and caching remain a separate concern.
- Eager compilation of every registered shader at startup or on reload.
- Unifying renderer pipeline initializers, vertex declarations, pass
  parameters, geometry, or draw order.
- Compatibility aliases that leave both private shader-map ownership and the
  global shader path as permanent public alternatives.

## Design Decisions and Invariants

- `FGlobalShader` derives from `FShader`; `FGlobalShaderType` derives from or
  specializes `FShaderType` without duplicating compiler, reflection, parameter
  metadata, or RHI shader machinery.
- A global shader is a fixed non-Material shader category, not a promise that
  one monolithic map candidate must compile every registered type atomically.
  The map partitions demand into bounded sections or sets selected in Stage 0,
  so one optional feature cannot make unrelated fixed rendering unavailable.
- `FGlobalShaderMap` is RenderCore-owned and RHI-lifecycle-bound. Public lookup
  is rendering-thread-only after initialization; reload, retry, device
  invalidation, and shutdown enter through explicit lifecycle functions called
  by the composed Renderer owner.
- `GetGlobalShaderMap()` returns the active map and never creates, reloads, or
  invalidates resources as an accessor side effect. Typed demand is explicit
  on the returned map.
- `TShaderMapRef<T>` retains the owning global-map payload or section strongly
  enough for recorded commands and fallback pipelines. It never stores an
  unowned shader pointer across refresh or device invalidation.
- A graphics pipeline payload retains the exact vertex/fragment shader refs and
  their resolved generation set. Draw parameter binding uses those retained
  refs rather than re-querying the newest global map after pipeline resolution.
- Multi-stage candidates never silently combine incompatible generations. Stage
  0 must select and test either atomic shader-set resolution or an exact
  compatibility identity derived from every retained stage before migration.
- Shader compile and binding failures retry on Shader or Manual generations;
  RHI shader failures retry on Device or Manual generations. Same-device
  refresh may retain a complete prior payload; device-generation changes never
  retain dependent RHI fallback.
- `renderer.reload-shaders changed|all` remains ordered, lazy, and demand-driven.
  `all` bypasses successful compiled-output reuse only for the first eligible
  candidate in that generation. `renderer.retry-resources` retries failed
  entries without rebuilding healthy global shaders.
- Global-map diagnostics identify shader type, virtual path, entry point,
  frequency, section/set identity, attempted generation, retained fallback,
  and terminal category exactly once per relevant failure generation.
- Registration is complete before normal rendering demand. Duplicate type
  names or conflicting implementation definitions fail deterministically;
  unloadable modules cannot leave callable factories or metadata in the map.
- Existing `FShaderMapBase`, `TShaderRef<T>`, shader cache artifacts, and stable
  virtual paths remain lower-level implementation foundations. The migration
  does not change persisted shader-cache identity without an explicit schema
  decision and compatibility test.

## Current Foundations and Gaps

`RenderCore/Public/Shader/Shader.h` already supplies shader types, permutation
hooks, typed instances, parameter metadata, `FShaderMapBase`, `TShaderRef<T>`,
merged pipeline layouts, and lazy RHI creation. The shared shader cache owns
source resolution, dependency fingerprints, compiled-output coalescing,
persistence, validation, and force-recompile behavior. Renderer owns
`FRendererResourceCoordinator`, `TRenderResourceCreationSlot`, diagnostics, and
device invalidation.

The missing layer is a registered global shader category and map that composes
those foundations. Fixed passes currently call `InitializeFromShaderTypes()`,
cast returned shader instances, create typed refs, check RHI handles, retain
maps, and synchronize pipeline generations independently. EditorGrid, Gizmo,
OverlayLine, and OverlayIcon expose the duplication most clearly, but targeted
search shows the same pattern across post-process, debug, lighting, ambient
occlusion, cloud, shadow, and fixed mesh-stage shaders.

## Implementation Stages

### Stage 0: Freeze global shader taxonomy, ownership, and compatibility identity

- [ ] Inventory every fixed shader type and classify it as global, Material,
  vertex-factory/mesh, generated, or intentionally renderer-local; record the
  initial migration set without reclassifying Material shader families.
- [ ] Select the bounded global-map section or shader-set identity that permits
  lazy demand and feature-local failure while preventing incompatible
  vertex/fragment generations from entering one pipeline candidate.
- [ ] Specify registration timing and unload rules for engine, project, and
  future plugin shader definitions, including duplicate type/implementation
  rejection and no callable metadata after module retirement.
- [ ] Define active-RHI ownership, rendering-thread access, startup, reload,
  manual retry, device invalidation, and shutdown ordering without exposing the
  Renderer coordinator through a global pointer.
- [ ] Capture current shader compile counts, cache hits, first-demand behavior,
  reload/retry diagnostics, fallback behavior, pipeline generation coupling,
  Vulkan output, and shutdown state for the EditorAssistance pilot.

#### Acceptance Gate

- Every initial shader has one category and owner; one selected map partition
  and compatibility identity preserve lazy demand, independent failure, exact
  shader/pipeline pairing, and the documented generation contract.
- Registration and lifecycle ordering are executable without a RenderCore to
  Renderer dependency, unloadable callback leak, eager all-shader compile, or
  device-crossing fallback.

### Stage 1: Add global shader types, registration, and typed map lookup

- [ ] Add `FGlobalShader` and `FGlobalShaderType` on top of the existing shader
  base/type machinery, including compile-permutation and environment hooks.
- [ ] Add `DURIN_DECLARE_GLOBAL_SHADER` and
  `DURIN_IMPLEMENT_GLOBAL_SHADER`; require exactly one implementation
  definition and preserve parameter-struct metadata composition.
- [ ] Implement `FGlobalShaderMap`, `GetGlobalShaderMap()`, and
  `TShaderMapRef<T>` with selected section/set ownership, strong retained
  lifetime, const lookup, and deterministic missing-type diagnostics.
- [ ] Add typed APIs for resolving one shader and a compatible multi-stage set,
  plus pipeline-layout construction from the exact retained shader reflections.
- [ ] Prove registry order independence, duplicate rejection, missing
  implementation behavior, type-safe retrieval, map lifetime, and no accessor
  mutation with RenderCore unit tests.

#### Acceptance Gate

- A registered fixed shader can be obtained through
  `TShaderMapRef<T>(GetGlobalShaderMap())`-style vocabulary without a consumer
  allocating, initializing, casting, or owning `FShaderMapBase`.
- Multi-stage lookup returns only a compatible, strongly retained set with a
  reproducible identity and pipeline layout.

### Stage 2: Integrate generation-aware creation, reload, recovery, and shutdown

- [ ] Back each selected map section/set with transactional creation state that
  publishes complete candidates, suppresses same-generation retry, and reports
  failure/recovery transitions through the renderer diagnostic vocabulary.
- [ ] Connect accepted shader reload, manual retry, and device invalidation
  generations explicitly from `FRendererResourceCoordinator` to the global map
  while preserving render-command ordering and lazy demand.
- [ ] Retain last-known-good shader payloads for same-device compile/binding/RHI
  refresh failure and discard every dependent RHI payload before advancing a
  device generation.
- [ ] Define pipeline consumers' exact shader-set generation API so a stale
  global shader fallback retains a matching last-known-good PSO and a recovered
  shader set triggers one eligible PSO reconstruction.
- [ ] Add failure injection for compile, binding, RHI creation, mixed-stage
  refresh, manual retry, device invalidation, repeated lookup, and shutdown.

#### Acceptance Gate

- Changed/all reload, manual retry, failure suppression, fallback retention,
  recovery diagnostics, and device invalidation match the existing Renderer
  resource-recovery contract without per-feature shader slots.
- No draw can bind parameters through shader refs newer or older than the refs
  retained by its resolved pipeline payload.

### Stage 3: Migrate EditorAssistance and establish the primitive-draw dependency

- [ ] Convert EditorGrid, Gizmo, OverlayLine, and OverlayIcon fixed shaders to
  global shader registration and typed map lookup while keeping their vertex
  declarations, geometry, pipeline configuration, draw order, and feature
  failure independence unchanged.
- [ ] Replace per-renderer Base shader-map slots with global refs plus
  renderer-specific transactional resources such as Gizmo static geometry,
  Overlay declarations, and the icon atlas/sampler.
- [ ] Retain exact shader refs in each pipeline payload and remove manual Base
  shader-generation rewriting only after the global set identity proves the
  same fallback behavior.
- [ ] Publish the accepted Global Shader API as the dependency gate for the
  Primitive Draw Interface simple-element shader implementation.
- [ ] Run focused EditorAssistance CPU, resource failure/recovery, Vulkan
  capture, reversed/forward depth, Present/Offscreen, reload, and device
  invalidation parity tests.

#### Acceptance Gate

- EditorAssistance contains no private ShaderMap compilation or typed cast
  boilerplate; all four features preserve visual output, independent pipeline
  variants, reload fallback, diagnostics, device recovery, and release.
- The Primitive Draw Interface plan can register its simple-element shaders
  without depending on an EditorAssistance-private helper.

### Stage 4: Migrate remaining eligible fixed renderer passes

- [ ] Migrate bounded fullscreen and debug families first, then lighting,
  post-process, shadow, ambient-occlusion, cloud, and other Stage 0-classified
  global shaders in independently validated slices.
- [ ] Keep generated Material programs, mesh shader maps, and vertex-factory
  families on their existing typed identities; extract shared pipeline or
  material work only through separately approved plans.
- [ ] Remove obsolete private shader-map payloads, casts, duplicate compile
  diagnostics, and generation plumbing immediately after each family passes
  parity.
- [ ] Measure compile request count, shader-map/code/RHI retention, reload work,
  PSO recreation, first-use latency, and diagnostic volume against Stage 0.
- [ ] Audit public/private headers and module descriptors so RenderCore remains
  below Renderer and no feature module owns global map lifetime.

#### Acceptance Gate

- Every Stage 0-eligible fixed shader resolves through one Global Shader path;
  targeted search finds no migrated renderer calling
  `InitializeFromShaderTypes()` or casting a global shader out of a private map.
- Cache identity, compile count, memory, first-use latency, reload work, and
  diagnostics remain within the recorded baseline or have an accepted measured
  tradeoff.

### Stage 5: Qualify, document, and retire transitional APIs

- [ ] Remove transitional adapters and any duplicate public fixed-shader
  creation path after all selected consumers migrate.
- [ ] Run the smallest registered RenderCore shader, Renderer resource,
  EditorAssistance, fixed-pass, RHI, Vulkan, module-lifecycle, and application
  targets selected through the repository testing workflow, followed by the
  required bounded aggregate and build tier.
- [ ] Update Shader Cache, Renderer Resource Recovery, Viewport Rendering, Code
  Modules, and any pass-specific contracts to the implemented ownership and
  failure semantics.
- [ ] Record exact compile/cache, reload/retry, fallback, device, visual,
  lifecycle, budget, build, test, and documentation evidence in this plan.
- [ ] Complete lifecycle metadata and repository-required plan/stage commit
  provenance only after every acceptance gate passes.

#### Acceptance Gate

- Source, tests, diagnostics, cache behavior, lifecycle ordering, and lasting
  documentation agree on one Global Shader architecture with no private
  fixed-shader-map compatibility path.
- Repeated initialization, rendering, reload, retry, device invalidation,
  shutdown, and restart leave no stale shader ref, cross-generation pipeline,
  duplicate registration, live RHI payload, or module callback.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Type system | Global/base category separation, exactly-one implementation registration, parameter metadata, typed retrieval, invalid cast prevention |
| Map identity | Stable section/set identity, order independence, exact multi-stage compatibility, retained shader refs and pipeline layout |
| Compilation | Cold/warm demand, DDC and memory hits, changed/all reload, force recompile, dependency change and corruption repair |
| Recovery | Compile/binding/RHI failure, same-generation suppression, last-known-good fallback, Manual retry, one recovery transition |
| Pipeline coupling | Stale shader fallback retains matching PSO; recovered set rebuilds once; draw parameters use pipeline-retained refs |
| Device lifecycle | Device generation discards RHI payloads and fallback, lazy reconstruction succeeds, shutdown leaves no live resource |
| Isolation | One optional global shader or section failure does not suppress unrelated fixed passes or EditorAssistance features |
| Migration | No private map compile/cast boilerplate remains in migrated families; non-global Material/mesh families remain unchanged |
| Performance | Compile requests, first-use latency, shader code/RHI memory, reload work, PSO creation, and diagnostic volume are bounded |
| Rendering | Forward/reversed depth, Present/Offscreen, fullscreen/debug/assistance captures and parameter binding match baseline |
| Documentation | Changed/all validation and all-plan lifecycle validation pass after lasting contracts are updated |

## Definition of Done

- RenderCore exposes the selected UE-aligned Global Shader types, map, typed
  lookup, registration, lifecycle, and diagnostics as the only fixed-shader
  ownership path.
- Fixed consumers no longer compile, cast, retain, reload, or invalidate private
  shader maps; they retain typed global refs and renderer-specific resources.
- Exact shader refs and compatibility generations remain coupled to dependent
  pipelines across successful refresh, failed refresh, retry, device
  invalidation, recorded command lifetime, and shutdown.
- EditorAssistance and every Stage 0-selected fixed family pass focused,
  aggregate, build, Vulkan, recovery, lifecycle, cache, and budget gates.
- Lasting RenderCore and Renderer documentation matches implemented ownership;
  transitional APIs are removed and changes are committed with required plan
  provenance.

## Deferred Follow-ups

- Renderer-wide graphics/compute Pipeline State Cache and PSO precaching.
- `FShaderPipelineType`-style registered shader pipeline groups beyond the
  bounded compatibility sets required here.
- Material and mesh shader-map convergence where future permutation and vertex
  factory requirements justify it.
- Async global shader prewarming and loading-screen integration.
- Plugin hot-unload of global shader implementations beyond the selected safe
  registration window.
- Multi-RHI or multiple simultaneous shader-platform maps.

## Related Documentation

- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Resource Lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Graphics State and Bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Primitive Draw Interface Plan](PrimitiveDrawInterface.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/ShaderCompilerCore.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompilerCore.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResourceCreation.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererResourceDiagnostics.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/`
- `Engine/Source/Runtime/Renderer/Private/Renderers/`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererEditorAssistanceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`

