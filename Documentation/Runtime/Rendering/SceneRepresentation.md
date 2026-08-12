# Renderer Scene Representation

Summary: Define engine-to-renderer scene publication, proxies, infos, mutation, and frame visibility.

Modules: Engine, RenderCore, Renderer

Last reviewed: 2026-08-12

Durin represents each renderable world resident with an Engine-facing
SceneProxy and a Renderer-owned SceneInfo. Components publish detached
candidates through `IScene`; rendering never retains or reads the originating
component, actor, reflected asset, or other game-thread object.

## Ownership Model

| Family | Detached proxy | Renderer scene entry |
| --- | --- | --- |
| Primitive | `FPrimitiveSceneProxy`, specialized by `FStaticMeshSceneProxy` and `FSkeletalMeshSceneProxy` | `FPrimitiveSceneInfo` |
| Light | `FLightSceneProxy`, specialized by directional, point, and spot proxies | `FLightSceneInfo` with authoritative typed family views |
| SkyBox | `FSkyBoxSceneProxy` | `FSkyBoxSceneInfo` |

A component constructs a complete proxy candidate from copied values and
retained renderer-facing resources on the game thread. `IScene` accepts unique
ownership and a family-specific `TSceneId`. The render-command pipe transfers
that candidate to the rendering thread, where `FScene` creates the paired
SceneInfo and becomes the only owner and mutator of the attached entry. Null or
invalid candidates publish nothing and cannot replace a complete entry.

Removal erases every typed membership reference before destroying the
SceneInfo and proxy on the rendering thread. Scene release clears typed views
before their owning maps. StaticMesh render data remains a non-owning borrow
bounded by the component render-state and asset-release fence protocol.
SkeletalMesh pose matrices and animated bounds are retained immutable values;
material proxies and SkyBox texture references remain counted references.

## Proxy and SceneInfo Responsibilities

`FPrimitiveSceneInfo` owns stable identity, owning scene, primitive kind,
visibility, transform, local bounds, derived world bounds, and typed-list
membership. StaticMesh and SkeletalMesh proxies own family-specific render data
and bindings. World bounds are rebuilt from the eight local AABB
corners whenever a finite transform is attached or updated; an invalid local
box remains invalid and is not used for culling. A skeletal dynamic update
replaces the immutable pose and local bound together and recomputes this world
bound before later FIFO visibility work.

`FLightSceneInfo` owns light identity, explicit family, conservative local
influence bounds, and typed membership. Directional, point, and spot proxies
own copied family values. `DLightComponent` centralizes registration,
visibility, transform, authored-property, and retirement publication; the
renderer never calls a component getter. Bounded per-view selection and the
shared GPU contract are defined by [Forward Lighting](ForwardLighting.md).

`FSkyBoxSceneInfo` owns runtime identity, persistent selection identity, the
selection-key tie-break, and typed membership. `FSkyBoxSceneProxy` owns the
retained texture reference, rotation, tint, and intensity. Active selection is
the minimum `(persistent identity, selection key, runtime identity)` tuple.

## Ordering and Typed Access

Ordinary scene mutation has one game-thread producer and uses FIFO
render-command order. There is no universal scene-entry revision. Remove then
add recreates an identity; update after remove is ignored unless a later add
has executed. The former SkyBox revision map was removed because SkyBox
publication has no independently completed work that can overtake this queue.
Owner-specific material and resource revisions remain at their actual
asynchronous or independently ordered boundaries.

`FScene` maintains one owning map per family and authoritative typed pointer
views for StaticMesh, SkeletalMesh, directional light, and SkyBox.
Attach, replacement, detach, and release update ownership and every relevant
view in one render command. Feature renderers iterate only their typed
SceneInfo view; they do not scan a shared primitive array or use RTTI to
rediscover proxy families. Material binding updates dispatch through the base
primitive-proxy contract rather than a StaticMesh branch in `FScene`.

## View-Local Environment Overrides

A submission may carry one optional `FViewEnvironmentOverride` inside
`FSceneViewRenderOptions`. This RenderCore value contains only a counted cube
texture reference plus copied rotation, tint, and intensity. It is command-local
content, not a primitive family: it has no Scene ID, transform, bounds,
visibility record, typed SceneInfo list, reflected owner, or persistent scene
selection state.

Renderer resolves the reference on the rendering thread and gives a valid
explicit environment precedence over the active scene SkyBox for that one
view. It then uses the shared SkyBox path before geometry. Empty options retain
ordinary scene-SkyBox behavior. A required override whose target is null, not a
cube, or cannot be rendered fails the view submission rather than selecting a
fallback texture. This path supports value-only editor captures without
changing the scene representation.

## Editor Primitive-Mutation Observation

The renderer scene remains render-thread-only. Editor CPU picking instead uses
a narrow Engine seam on `DLevel`: a game-thread subscriber first receives one
complete primitive snapshot and then monotonically revised mutation batches.
`DPrimitiveComponent` publishes registration, retirement, transform, owner
visibility, and proxy/data replacement through its authoritative render-state
paths; `DSkeletalMeshComponent` additionally publishes each complete current
pose bound. Payloads contain weak Actor/component identity plus primitive ID,
registration generation, family, visibility, and finite transformed bounds.

The observer owns no LevelEditor types, does not retain reflected objects, and
is removed when its owner detaches. Callbacks may not re-enter primitive
mutation; this is an unrecoverable callback contract. Consumers recover from
any non-consecutive externally supplied revision with a complete snapshot. This seam is separate from `IScene` and
does not expose `FScene`, SceneInfo, prepared views, or render-thread state.

## Failure and Thread Contracts

- Invalid identities, null proxies, and non-finite primitive transforms are
  rejected before enqueue and leave existing membership unchanged.
- Render-thread queries and SceneInfo mutation assert rendering-thread ownership.
- Typed SceneInfo access asserts that the explicit primitive kind matches the
  requested proxy family.
- Replacement detaches the prior typed entry before publishing the new entry;
  removal and release leave no typed pointer after ownership is destroyed.
- Scene mutation adds no waits or second task system. Existing targeted
  resource and asset-release fences retain their lifetime ownership.

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [Static Mesh Rendering](StaticMeshRendering.md)
- [Cube Textures](CubeTextures.md)
- [Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Rendering Capability Expansion Roadmap](../../Roadmaps/RenderingCapabilityExpansion.md)
