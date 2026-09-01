# Renderer Scene Representation

Summary: Define engine-to-renderer scene publication, proxies, infos, mutation, and frame visibility.

Modules: Engine, RenderCore, Renderer

Last reviewed: 2026-09-01

Durin represents each renderable world resident with an Engine-created
SceneProxy and a Renderer-owned SceneInfo. Components call only the public
component-level `FSceneInterface::Add*`/`Remove*` operations; rendering never
retains or reads the originating component, actor, reflected asset, or other
game-thread object. The concrete `FScene` declaration and its proxy-mutation
seam are Renderer-private.

## Ownership Model

| Family | Detached proxy | Renderer scene entry |
| --- | --- | --- |
| Primitive | `FPrimitiveSceneProxy`, specialized by `FStaticMeshSceneProxy`, `FSkeletalMeshSceneProxy`, and `FSplineMeshSceneProxy` | `FPrimitiveSceneInfo` with StaticMesh/SkeletalMesh/SplineMesh typed views |
| Light | `FLightSceneProxy`, specialized by directional, point, and spot proxies | `FLightSceneInfo` with authoritative typed family views |
| SkyBox | `FSkyBoxSceneProxy` | `FSkyBoxSceneInfo` |
| Volumetric cloud | `FVolumetricCloudSceneProxy` | `FVolumetricCloudSceneInfo` |

The public virtual interface carries only component pointers and returns
`void`. Renderer-private `FScene` validates the registered component and owning
Scene synchronously on the game thread, invokes the component's private proxy
builder through friendship, and passes only detached copied state to its own
private admission helpers. For Light, SkyBox, and VolumetricCloud the component
retains only `Proxy.get()` as an opaque removal token after successful command
admission. Primitive retains its stable Scene ID and a publication flag. The
render-command pipe temporarily carries shared ownership because its callable
is copyable; after attachment the paired SceneInfo is the only authoritative
owner. A null proxy caused by hidden state or an unsupported render
representation is a legal no-publication result.

Removal erases every typed membership reference before destroying the
SceneInfo and proxy on the rendering thread. Scene release clears typed views
before their owning maps. StaticMesh render data remains a non-owning borrow
bounded by the component render-state and asset-release fence protocol.
SkeletalMesh pose matrices and animated bounds are retained immutable values.
SplineMesh retains copied normalized deformation values and bounds while
borrowing the source StaticMesh render data under the same retirement fence;
material proxies and SkyBox texture references remain counted references.

## Proxy and SceneInfo Responsibilities

`FPrimitiveSceneInfo` owns stable identity, owning scene, primitive kind,
visibility, transform, local bounds, derived world bounds, and typed-list
membership. StaticMesh, SkeletalMesh, and SplineMesh proxies own
family-specific render data and bindings. World bounds are rebuilt from the eight local AABB
corners whenever a finite transform is attached or updated; an invalid local
box remains invalid and is not used for culling. A skeletal dynamic update
replaces the immutable pose and local bound together and recomputes this world
bound before later FIFO visibility work. A SplineMesh dynamic update similarly
accepts only a newer non-zero deformation revision and atomically replaces the
copied parameters and local/world bounds. Stale updates and updates after
retirement are ignored without reading the component.

`FLightSceneInfo` owns explicit family, conservative local influence bounds,
and typed membership. `FLightSceneProxyDesc` copies the stable light identity;
directional, point, and spot proxies own copied family values.
`DLightComponent` centralizes registration,
visibility, transform, authored-property, and retirement publication; the
renderer never calls a component getter. Bounded per-view selection and the
shared GPU contract are defined by [Forward Lighting](ForwardLighting.md).

`FSkyBoxSceneInfo` owns typed membership. `FSkyBoxSceneProxyDesc` owns persistent
candidate identity, the selection-key tie-break, runtime diagnostic identity,
retained texture reference, rotation, tint, and intensity. Active selection is
the minimum `(persistent identity, selection key, runtime identity)` tuple.

## Ordering and Typed Access

Ordinary scene mutation has one game-thread producer and uses FIFO
render-command order. Light, SkyBox, and VolumetricCloud registries key
membership by the exact proxy pointer. Rebuild submits Remove for the old token
before Add for the newly constructed proxy, so immediate Add/Remove and repeated
dirty rebuilds need no tombstone or universal publication revision. Work that
has an independently ordered boundary retains a feature-specific generation;
the cloud proxy carries a unique immutable history key for temporal invalidation.

Renderer-private `FScene` composes `FLightSceneRegistry`, `FSkyBoxSceneRegistry`, and
`FVolumetricCloudSceneRegistry`; each registry owns its pointer-keyed family map,
indexes, and active-candidate policy. Primitive membership remains directly
owned and keyed by Scene ID while its registry extraction is deferred. A
Primitive proxy rebuild submits Remove-old and Add-new; duplicate membership is
a contract violation rather than an implicit replacement. Attach, detach, and
release update ownership and every relevant view in one render command. Feature renderers iterate only their typed
SceneInfo view; they do not scan a shared primitive array or use RTTI to
rediscover proxy families. Render-thread selection, counts, and SceneInfo
access remain concrete `FScene` operations and are not part of the publication
interface. SplineMesh visibility and preparation counters
separate visible, prepared, rejected, section/triangle, accepted dynamic-update,
and retained-deformation values and conserve candidates against outcomes.
Material binding updates dispatch through the base
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
any non-consecutive externally supplied revision with a complete snapshot. This seam is separate from `FSceneInterface` and
does not expose `FScene`, SceneInfo, prepared views, or render-thread state.

## Failure and Thread Contracts

- Invalid Desc values, null proxies, and non-finite primitive transforms are
  rejected before enqueue. Private `FScene::TryAdd/Remove*Proxy` helpers consume
  an Add `unique_ptr` in every case and report admission only inside concrete
  Scene lifecycle methods; ordinary callers receive `void` and an admission
  failure triggers `requiref`.
- Components assign their token/publication flag only after successful Add
  admission and clear it only after successful Remove admission. They never
  dereference an accepted proxy token.
- Render-thread queries and SceneInfo mutation assert rendering-thread ownership.
- Typed SceneInfo access asserts that the explicit primitive kind matches the
  requested proxy family.
- Rebuild detaches the exact prior typed entry before publishing the new entry;
  removal and release leave no typed pointer after ownership is destroyed.
- Scene mutation adds no waits or second task system. Existing targeted
  resource and asset-release fences retain their lifetime ownership.
- `FScene` follows `Active -> Releasing -> Released`. `Release()` is a required,
  one-shot game-thread operation that queues Registry clearing. `FScenePtr`
  deletion is queued behind that command, occurs only on the render thread,
  and requires empty Registries. Engine and preview owners first detach their
  World/components, then call `Release()`, then reset the pointer. Renderer
  shutdown rejects active owners and verifies that all allocated scenes have
  been deleted after its final flush.

## Related Documentation

- [Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md)
- [Viewport Rendering](ViewportRendering.md)
- [Static Mesh Rendering](StaticMeshRendering.md)
- [Cube Textures](CubeTextures.md)
- [Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Rendering Capability Expansion Roadmap](../../Roadmaps/Archive/2026-08/RenderingCapabilityExpansion.md)
