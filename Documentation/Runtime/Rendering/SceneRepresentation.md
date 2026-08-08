# Renderer Scene Representation

Last reviewed: 2026-08-08

Durin represents each renderable world resident with an Engine-facing
SceneProxy and a Renderer-owned SceneInfo. Components publish detached
candidates through `IScene`; rendering never retains or reads the originating
component, actor, reflected asset, or other game-thread object.

## Ownership Model

| Family | Detached proxy | Renderer scene entry |
| --- | --- | --- |
| Primitive | `FPrimitiveSceneProxy`, specialized by `FStaticMeshSceneProxy` and `FTextureCubePreviewSceneProxy` | `FPrimitiveSceneInfo` |
| Light | `FLightSceneProxy`, specialized by `FDirectionalLightSceneProxy` | `FLightSceneInfo` |
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
bounded by the component render-state and asset-release fence protocol;
material proxies and SkyBox texture references remain counted references.

## Proxy and SceneInfo Responsibilities

`FPrimitiveSceneInfo` owns stable identity, owning scene, primitive kind,
visibility, transform, local bounds, derived world bounds, and typed-list
membership. StaticMesh and TextureCube preview proxies own family-specific
render data and bindings. World bounds are rebuilt from the eight local AABB
corners whenever a finite transform is attached or updated; an invalid local
box remains invalid and is not used for culling.

`FLightSceneInfo` owns light identity and typed membership.
`FDirectionalLightSceneProxy` owns copied direction, color, intensity,
ambient, and rim-light values. Component registration, visibility, transform,
and authored property changes publish a replacement candidate; the renderer
never calls a component getter.

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
views for StaticMesh, TextureCube preview, directional light, and SkyBox.
Attach, replacement, detach, and release update ownership and every relevant
view in one render command. Feature renderers iterate only their typed
SceneInfo view; they do not scan a shared primitive array or use RTTI to
rediscover proxy families. Material binding updates dispatch through the base
primitive-proxy contract rather than a StaticMesh branch in `FScene`.

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
- [RHI Command Execution](RHICommandExecution.md)
- [Rendering Capability Expansion Roadmap](../../Roadmaps/RenderingCapabilityExpansion.md)
