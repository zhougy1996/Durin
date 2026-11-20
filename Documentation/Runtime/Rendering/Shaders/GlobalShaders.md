# Global Shaders

Summary: Define registration, atomic map publication, typed lookup, generation recovery, and lifecycle for fixed non-Material shaders.

Modules: Engine, RenderCore, Renderer, ShaderFormat

Last reviewed: 2026-11-20

## Category and Ownership

`FGlobalShader` is the fixed, non-Material shader category. Its
`FGlobalShaderType` reuses the ordinary compiler, reflection, parameter
metadata, shader-map resource code, and lazy RHI shader implementation.
Material programs use the sibling `FMaterialShader` category. Vertex-factory
and mesh-pass combinations use `FMeshMaterialShader`, whose identity adds the
registered stable Vertex Factory type, mesh-pass key, and local permutation;
neither category is admitted to a Global Shader set.

Engine owns the process `FGlobalShaderMap` and the Global/Material Shader categories. Renderer does not own private
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
| Global | EditorGrid, Gizmo, SimpleElement line/sprite, GBufferDebug, SkyBox, PostProcess Copy/FXAA, ContactVisibility compute/fragment, DeferredDirectionalLighting, GTAO raw/filter, VolumetricCloud compute/fragment/temporal/composite, VolumetricCloudShadow compute/fragment | Declared by Renderer and published by Engine |
| Material | Generated forward, GBuffer, and masked-shadow fragments | Engine `FMaterialShaderMap` payloads built from Engine's accepted immutable compiler result |
| Vertex-factory/mesh | GBuffer, StaticMesh, SplineMesh, and fixed opaque-shadow combinations | The same exact typed map API using `FMeshMaterialShader` identity |
| Finite Editor-only | TextureEditor preview and Mona ImGui backend programs | Stable feature-program contributions owned by their defining modules; excluded from Game Cook |

Generated Material programs remain generated rather than global. No eligible
fixed Renderer family retains a private-map compatibility path.

## Global Shader Map Publication

Renderer startup prepares the registered global sets eligible for the active profile.
It joins authored compilation or loads cooked outputs, then queues publication before
renderer startup resources. The render thread publishes the complete map only after
every section passes shader-map initialization. Each pass obtains its stages through
`GetGlobalShaderMap().GetShader<T>()`. A `TShaderMapRef<T>` retains the
section that owns the shader, including its parameter bindings and code.
Prepared draws keep the references they use for submission.

The cook registry supplies each eligible program's exact entry points. A
graphics pipeline takes the merged layout from its fragment shader's section,
so programs sharing a fullscreen vertex shader can have different layouts.
Compute pipelines use the compute shader's section. EditorGrid, Gizmo and
SimpleElement programs are eligible only in editor profiles. In Authored mode
set construction asks ShaderBuild for a complete output. In Cooked mode the
same typed construction consumes the exact `DSHD` output from the qualified
`DSLB` library. Parameter binding, merged layouts, lazy RHI creation, generation
coupling, and complete-or-null publication are shared after that data selection.

Registered direct, prepared and Cook requests apply the same
[parameter allocation policy](ShaderParameters.md#detached-semantic-schemas-and-allocation-candidates).
Each shader instance admits its own semantic schema against the final reflected
stage product and caches the resulting binding records. Shared parameter owners
share names and types, not descriptor coordinates. Editor-only finite programs
remain excluded from Game inventory; cooked initialization performs the same
admission and layout construction without source compilation.

Compiled stages retain immutable code and opaque, versioned target metadata.
ShaderFormat owns target validation, the Metal metadata codec, and the shared
`BuildLogicalBindings` conversion from reflection into logical declarations.
RenderCore has no separate conversion adapter and allocates no native slots.
DSHD v7, shared compiler output v8, and DSLB v7 preserve those bytes and validate
the complete shader identity before publication. The identity covers target
settings, format version, stage/group size, binary entry point, code, logical
reflection (including resource shape and constant layouts), and execution metadata. `FShaderCode::CodeHash` remains the separately
checked code-only hash. Obsolete cache values miss; incompatible cooked libraries
require recooking. Material cooked payload/family schema v28 uses the same opaque
metadata and shader validation rather than serializing Metal fields itself.
Compiler, cache and cooked stages use this same `FShaderCode` representation.
Reflection is the only stored declaration list; merged pipeline layouts and
native binding operations are prepared at pipeline creation. Identity-only
cache and serialization paths do not construct code owners or layouts.

## Generations and Failure Recovery

`FRendererResourceCoordinator` explicitly supplies accepted shader, device,
and manual generations to Engine's typed maps. Shader reload submits a replacement
for the published map. Render-frame processing installs it once all sections succeed.
Readers retain the previous generation during compilation and after a failed or
canceled replacement. Shader and manual generations advance for pipeline consumers when the
replacement is published. Repeated typed lookups read the installed map.
Compilation errors are reported once by the producer with the program name
and compiler diagnostic.

## Lifecycle

`FShaderCompilingManager::PrepareJobs` creates an existing job-group control without
starting compilation or invoking its completion. `SubmitJobs` returns `void` and
is the sole entry that starts the prepared group. ShaderBuild starts its existing
subscriber immediately through the private backend compile entry points; it has no
separate prepared/submitted phase. Handles only cancel, prioritize, query and wait.
Submitted controls remain waitable through completion. Canceling an unsubmitted
group completes it locally without registering it or consulting ShaderBuild.
Repeated submission, including after cancellation, does not start duplicate work.

Fixed aggregate Shader admission thresholds no longer reject requests. Existing
execution windows and Task scheduling control execution; single-request source and output limits remain
validated. Failed dispatch and invalid inputs arrive through `FShaderCompilerOutput`;
submission after shutdown or owner retirement completes as canceled. Callbacks run
outside state locks, may run synchronously during submission or cancellation, and
finish once. Waits from callbacks return `WouldBlock`. ShaderBuild retains source,
cache and compiler ownership without depending on Engine.

`FShaderMapPreparation` can prepare exact registered programs before a thumbnail
submission. Engine submits it through `FShaderCompilingManager`, sharing
in-flight and completed CPU outputs per target, type set, macros, and accepted
Shader generation. Reload/manual generations retire earlier preparation; forced
reload forces replacement compilation. Failed requests remain terminal until
invalidation. Preparation does not construct typed maps or RHI objects on workers.
`FPreparedShaderMapScope` forbids synchronous compilation during thumbnail capture;
material maps consume the prepared output in their requested stage order.
Thumbnail passes read their global shaders directly from the published map.
Unavailable prepared outputs defer the resource attempt as `Pending`, without
publishing a failure or suppressing a same-generation retry. Pending observations
are local to the current capture; each slot defers only when its own factory
encounters an unavailable output. Scene capture returns `RendererResourcesPending`
before graph execution so the thumbnail owner can retry. Fixed material stages
use material-independent requests: material permutation macros belong to the
accepted generated program, not to the fixed opaque-shadow preparation.

Normal lookup and lifecycle operations occur on the rendering thread after
Engine initialization. Renderer owns command admission but passes values
and callbacks explicitly; RenderCore has no dependency on Renderer and no
pointer to `FRendererResourceCoordinator`.

Shutdown closes Renderer admission, releases consumer pipelines and typed
refs, clears the global map, resets generation state, and only then permits RHI
and module teardown. RHI also broadcasts a pre-shutdown resource-release event;
Engine registers and removes its callback with module startup/shutdown so
standalone RHI lifecycle users cannot carry shader modules across device exit.
The supported module contract is static registration for
the module lifetime; future plugin hot unload requires a separate retirement
protocol before callable factories can be removed safely.

## Related Documentation

- [Shader Cache](ShaderCache.md)
- [Renderer Resource Recovery](../Pipeline/RendererResourceRecovery.md)
- [Viewport Rendering](../Pipeline/ViewportRendering.md)
- [Code Modules](../../../Workspace/CodeModules.md)
