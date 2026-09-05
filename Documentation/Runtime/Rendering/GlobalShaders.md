# Global Shaders

Summary: Define registration, atomic set ownership, typed lookup, generation recovery, and lifecycle for fixed non-Material shaders.

Modules: RenderCore, Renderer

Last reviewed: 2026-08-30

## Category and Ownership

`FGlobalShader` is the fixed, non-Material shader category. Its
`FGlobalShaderType` reuses the ordinary compiler, reflection, parameter
metadata, shader-map resource code, and lazy RHI shader implementation.
Material programs use the sibling `FMaterialShader` category. Vertex-factory
and mesh-pass combinations use `FMeshMaterialShader`, whose identity adds the
registered stable Vertex Factory type, mesh-pass key, and local permutation;
neither category is admitted to a Global Shader set.

RenderCore owns the process `FGlobalShaderMap`. Renderer does not own private
`FShaderMapBase` instances for global shaders. A shader class declares its
metadata with `DURIN_DECLARE_GLOBAL_SHADER`; exactly one translation unit
defines registration with `DURIN_IMPLEMENT_GLOBAL_SHADER`. Static registration
finishes while the defining module loads. Duplicate names fail immediately,
and hot unload of registered implementations after normal rendering demand is
not supported.

`GetGlobalShaderMap()` is an accessor only. It does not compile, refresh,
retry, or invalidate a section.

The initial inventory is deliberately closed over fixed Renderer programs:

| Category | Families | Ownership |
| --- | --- | --- |
| Global | EditorGrid, Gizmo, SimpleElement line/sprite, GBufferDebug, SkyBox, PostProcess Copy/FXAA, ContactVisibility compute/fragment, DeferredDirectionalLighting, GTAO raw/filter, VolumetricCloud compute/fragment/temporal/composite, VolumetricCloudShadow compute/fragment | 38 registered types in 19 exact sets, declared by Renderer and mapped by RenderCore |
| Material | Generated forward, GBuffer, and masked-shadow fragments | RenderCore `FMaterialShaderMap` payloads built from Engine's accepted immutable compiler result |
| Vertex-factory/mesh | GBuffer, StaticMesh, SplineMesh, and fixed opaque-shadow combinations | The same exact typed map API using `FMeshMaterialShader` identity |
| Finite Editor-only | TextureEditor preview and Mona ImGui backend programs | Stable feature-program contributions owned by their defining modules; excluded from Game Cook |

Generated Material programs remain generated rather than global. No eligible
fixed Renderer family retains a private-map compatibility path.

## Atomic Shader Sets

The publication unit is a caller-named exact shader set, not one monolithic
map of every registered global shader. A set sorts its type list by stable type
name, incorporates the section name and every type into its identity, compiles
the complete set from one source variant, and publishes only when compilation,
parameter binding, optional RHI creation, and layout merging succeed.

This partition keeps optional features independent and makes vertex/fragment
or compute membership reproducible. Reusing a section name with a different
type set is an invariant failure. `FGlobalShaderSetRef` strongly retains the
published map and exposes its exact generation, identity, and merged pipeline
layout. `TShaderMapRef<T>` provides type-safe access while retaining the same
set. A pipeline payload retains the set it used; parameter binding uses a
typed ref from that retained set rather than looking up a newer map.

Every exact set also contributes one stable runtime request until the target
inventory freezes. The 15 non-`EditorAssistance` sets are Win64/Game inputs;
EditorGrid, Gizmo, and both SimpleElement sets are Editor-only. In Authored mode
set construction asks ShaderBuild for a complete output. In Cooked mode the
same typed construction consumes the exact `DSHD` output from the qualified
`DSLB` library. Parameter binding, merged layouts, lazy RHI creation, generation
coupling, and last-known-good publication are shared after that data selection.

## Generations and Failure Recovery

`FRendererResourceCoordinator` explicitly supplies accepted shader, device,
and manual generations to RenderCore. Demand remains lazy:

- changed reload validates dependencies normally on the next demanded set;
- all reload force-recompiles the first eligible candidate in that shader
  generation;
- manual retry permits one new attempt for failed entries;
- repeated demand in the same relevant generation is suppressed.

A failed same-device refresh retains a complete last-known-good set and reports
the attempted generation and retained fallback. A successful later candidate
publishes atomically and reports one recovery transition. Pipeline slots use
the published set generation, so a stale shader fallback keeps a compatible
pipeline and a recovered set makes one new pipeline attempt eligible.

Device invalidation first releases Renderer consumers, then resets every
global section and the weak shader-map resource cache before the device
generation advances. No global RHI fallback crosses a device generation.
The global set owns the detailed shader failure diagnostic; a containing
feature slot mirrors the unavailable state for retry purposes without emitting
a second wrapper failure or recovery transition.

## Lifecycle

Normal lookup and lifecycle operations occur on the rendering thread after
RenderCore initialization. Renderer owns command admission but passes values
and callbacks explicitly; RenderCore has no dependency on Renderer and no
pointer to `FRendererResourceCoordinator`.

Shutdown closes Renderer admission, releases consumer pipelines and typed
refs, clears the global map, resets generation state, and only then permits RHI
and module teardown. RHI also broadcasts a pre-shutdown resource-release event;
RenderCore registers and removes its callback with module startup/shutdown so
standalone RHI lifecycle users cannot carry shader modules across device exit.
The supported module contract is static registration for
the module lifetime; future plugin hot unload requires a separate retirement
protocol before callable factories can be removed safely.

## Related Documentation

- [Shader Cache](ShaderCache.md)
- [Renderer Resource Recovery](RendererResourceRecovery.md)
- [Viewport Rendering](ViewportRendering.md)
- [Code Modules](../../Workspace/CodeModules.md)
