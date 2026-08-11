# TextureCube Thumbnail View-Environment Refactor Plan

Summary: Move TextureCube thumbnails from an editor-only mesh Component and PrimitiveSceneProxy path to a value-owned per-view environment override while preserving the shared asynchronous thumbnail pipeline.

Last reviewed: 2026-08-12

Status: Active
Completed:

## Current Status

No implementation stage has started. This plan selects a direct view-environment
path for TextureCube thumbnails and retains preview-world Components only for
assets whose thumbnails are genuinely represented by scene-resident geometry.

The current TextureCube provider still loads `/Engine/Models/Sphere`, spawns an
Actor, installs `DTextureCubePreviewComponent`, assigns the sphere and cube, and
publishes `FTextureCubePreviewSceneProxy` as a primitive. That proxy passes
through primitive identity, bounds, visibility classification, typed SceneInfo
storage, and prepared-view collection. The final renderer no longer consumes
the sphere render data: it changes the view to a fixed 100-degree vertical field
of-view and invokes the shared SkyBox fullscreen draw. The Component and sphere
therefore survive from the former reflection-sphere implementation rather than
representing the current wide-environment output.

The selected correction keeps Durin's existing cold-generation session,
revision validation, cancellation, render-thread capture, readback, persistent
cache, and provider-unload contracts. It adds one value-only render option for
an explicit view environment, lets the shared thumbnail scene pool configure
that option, and routes it through the existing `FSkyBoxRenderer`. TextureCube
provider code supplies a counted `FRHITextureReferenceRef` and view contract;
it does not install a Component, proxy, callback, or reflected asset pointer in
the renderer scene.

This matches the useful responsibility split in Unreal Engine without copying
its synchronous thumbnail architecture: direct texture thumbnail renderers do
not require a mesh preview scene, while mesh, material, animation, and actor
thumbnails use preview scenes and Components. Durin retains its own asynchronous
cache and module-unload safety rather than introducing an Unreal-style UObject
thumbnail renderer boundary.

Two implemented contract documents currently describe a reflective sphere even
though production code and cache identity use the wide-environment path. Their
lasting text is corrected only after the new path and output baseline are
qualified, so the plan does not publish intended behavior as implemented early.

## Goal

- Remove `DTextureCubePreviewComponent` and every TextureCube-thumbnail-specific
  primitive scene type, typed list, visibility branch, and renderer collection.
- Represent a TextureCube thumbnail as one immutable view-environment input
  containing a stable RHI texture reference and copied presentation values.
- Preserve the current wide-environment image, orientation, opaque-output,
  post-process, revision, cancellation, persistent-cache, and readback behavior.
- Keep scene-resident Material, StaticMesh, and SkeletalMesh thumbnails on the
  existing preview-world Component path.
- Make an unavailable explicit thumbnail environment fail capture rather than
  silently rendering the runtime black-cube fallback as a successful thumbnail.
- Leave Renderer and DurinEd free of `DTextureCube`, TextureEditor session types,
  and provider-owned callbacks.

## Scope

- A RenderCore-owned value type describing an optional per-view cube environment
  override and a submission-options type accepted by `IRendererModule`.
- A typed render result sufficient for the thumbnail pool to distinguish a
  completed view from invalid output or an unavailable required environment.
- Renderer integration that gives the explicit view environment precedence over
  a scene SkyBox and executes it through the existing `FSkyBoxRenderer`.
- A provider-neutral environment configuration capability on
  `IRenderedAssetThumbnailPreviewScene` and corresponding state in
  `FRenderedAssetThumbnailPreviewScenePool`.
- TextureEditor migration from Actor/Component/sphere setup to a stable RHI
  reference plus explicit wide-environment view setup.
- Removal of the reflected preview Component, specialized primitive proxy,
  primitive kind, renderer typed membership, visibility preparation, and
  `FTextureCubeThumbnailRenderer` adapter.
- Focused render-contract, scene-contract, thumbnail-pool, TextureCube-provider,
  cancellation, lifetime, rendered-output, documentation, build, and runtime
  qualification.

## Non-Goals

- Replacing preview worlds or Components for Material, MaterialInstance,
  StaticMesh, SkeletalMesh, particle, actor, or other scene-backed thumbnails.
- Reworking the interactive TextureCube editor or choosing whether it should
  use a reflective sphere, environment view, or both.
- Changing runtime `DSkyBoxComponent`, active-sky selection, environment
  lighting, reflection capture, cube import, cube face orientation, or RHI cube
  upload contracts.
- Introducing an asset-specific `RenderTextureCubeThumbnail` method on
  `IRendererModule`.
- Capturing provider callbacks, virtual renderer objects, lambdas owned by an
  editor feature module, or arbitrary render-pass extensions in queued render
  commands.
- Making every rendered-thumbnail provider independent of the shared preview
  scene allocation. Lazy scene creation for direct-only jobs is deferred until
  measured scene construction cost justifies changing scheduler admission.
- Generalizing the first environment override into a public render graph,
  plugin render-pass registry, or unbounded type-erased payload system.
- Changing thumbnail dimensions, encoding, color space, cache budget, request
  priority, render-per-frame budget, or persistent object layout.

## Design Decisions and Invariants

### Scene-resident content versus view-local content

- A thumbnail uses the preview scene when its visual representation requires
  world transform, bounds, visibility, material slots, animation state,
  lighting interaction, or ordinary scene lifetime. Material, StaticMesh, and
  SkeletalMesh providers remain in this category.
- The current TextureCube thumbnail is a camera-oriented fullscreen environment
  sample. It has no meaningful primitive transform, local bounds, visibility,
  material binding, collision, or editor picking state and therefore does not
  enter `IScene` primitive membership.
- An explicit environment is one render submission's content override. It is
  not serialized, reflected, registered with a world, selected as a persistent
  scene SkyBox, or discoverable through renderer SceneInfo accessors.

### Ownership and module layering

- RenderCore owns the public value-only submission vocabulary, tentatively
  `FViewEnvironmentOverride` and `FSceneViewRenderOptions`, because
  `IRendererModule` is the cross-module render boundary and RenderCore already
  depends on RHI.
- The environment value contains only a counted `FRHITextureReferenceRef`,
  copied rotation, tint, and nonnegative intensity. It contains no `DObject*`,
  `DTextureCube*`, concrete texture resource pointer, Scene proxy, or editor
  provider identity.
- Renderer owns validation, preparation, precedence, draw order, and execution
  of the environment option. It maps the option into command-local prepared
  sky data and reuses `FSkyBoxRenderer`; it does not learn about thumbnails or
  TextureEditor.
- DurinEd owns the pooled configuration, capture snapshot, result handling,
  cancellation generation, output target, readback, and reset behavior. Its
  public rendered-thumbnail extension contract may expose the RenderCore value
  and must declare the corresponding public module dependency rather than rely
  on incidental include visibility.
- TextureEditor owns asset loading, exact-class validation, resource readiness,
  stable-reference acquisition, wide-environment camera contract, revision
  revalidation, cache identity, and asset-qualified diagnostics.
- Engine owns no thumbnail-specific Component, proxy, or primitive kind after
  migration. General SkyBox scene state remains unchanged.

### Submission and thread contract

- The provider obtains and configures the environment on the game thread only
  after the TextureCube reports a ready build/resource revision and a non-null
  stable texture reference.
- `FRenderedAssetThumbnailPreviewScenePool::BeginCapture` snapshots the view,
  environment value, capture generation, renderer endpoint, scene endpoint,
  and output target into the existing render command. Later pool mutation or
  provider reset cannot alter that accepted command.
- The render thread resolves the stable reference to its currently targeted
  `FRHITexture`. No render-thread code reads the originating TextureCube,
  session, Component, or feature module object.
- Cancellation may invalidate publication and reset game-thread pool state
  while an accepted render command is queued. The command-owned counted RHI
  reference remains valid until that command completes; the generation check
  still rejects stale pixels.
- Provider removal captures no function pointer or destructor whose code lives
  in TextureEditor. Session destruction remains on the game thread through the
  established registration-removal protocol.

### Render result and failure policy

- `IRendererModule::RenderView` returns a small typed result rather than leaving
  the thumbnail pool unable to distinguish a completed render from an early
  resource return. Exact enumerator names are selected in Stage 1, but the
  result must distinguish success, invalid output, renderer-resource failure,
  and unavailable required environment.
- An explicit view environment is required content. A null reference, a stable
  reference whose render-thread target is null, an incompatible texture, or a
  failed SkyBox draw makes the capture fail with one stable core diagnostic.
- Runtime scene SkyBoxes retain their existing black-cube fallback behavior.
  That availability policy does not apply to an explicit thumbnail override,
  because publishing a black fallback as the authored asset's persistent
  thumbnail would be a false success.
- The thumbnail pool performs readback only after a successful render result.
  Failure updates its capture state and error under the existing capture mutex;
  the cache then follows its ordinary failed-render path.
- Provider revision validation remains before capture, after readback, and at
  pixel publication. The render result complements those checks and does not
  replace them.

### Environment precedence and draw ordering

- Precedence is exact: a valid explicit view environment overrides the active
  scene SkyBox for that submission; otherwise the active scene SkyBox is used;
  otherwise no sky draw is issued.
- The selected environment is drawn once through `FSkyBoxRenderer` before
  opaque, masked, translucent, and editor-assistance geometry, matching its
  semantic role as a background. The removed TextureCube adapter no longer
  draws a fullscreen triangle after geometry.
- A TextureCube thumbnail session installs no scene geometry, so moving the
  environment draw to the normal SkyBox position must retain the frozen output.
  A mismatch is diagnosed before changing the visual contract.
- The environment view's 100-degree vertical field-of-view moves from a hidden
  constant in Renderer to a TextureEditor-owned visual-contract value. Renderer
  must not silently rewrite a provider-selected projection.

### Pool reset and mutual exclusion

- The shared pool stores at most one pending environment configuration because
  its existing budget admits at most one active rendered capture.
- `SetView` and environment configuration are order-independent. Environment
  state is stored separately from the built `FSceneView` and combined only when
  capture snapshots its input.
- `Reset()` always clears the environment reference and presentation values in
  addition to resetting view and capture state. No Material, StaticMesh,
  SkeletalMesh, or subsequent TextureCube job may inherit prior environment
  state.
- Scene-backed sessions retain their current obligation to destroy their own
  Actors and Components in `ResetPreview`. The value-only environment is
  pool-owned configuration and is cleared by pool reset rather than represented
  as provider-owned world content.

### Visual identity and persistent cache

- Stage 0 freezes the production wide-environment output, including direction,
  projection, opacity, post-process, face orientation, and representative edge
  sampling, before changing ownership.
- An implementation-only migration that reproduces the frozen pixels keeps the
  current provider schema, environment fixture version, and shader contract so
  valid persistent objects are not invalidated without an output reason.
- If the qualified pixels differ, implementation pauses until the difference is
  classified. An intentional visual change increments the TextureCube generator
  schema and environment-view fixture version. The shader contract increments
  only if shader code or bindings change; reusing the unchanged SkyBox shader
  is not itself a shader-contract change.
- `/Engine/Models/Sphere` and its fixture version remain because Material
  thumbnails still use them. Only TextureCube's dependency on that fixture is
  removed.

### Rejected alternatives

- Replacing `DTextureCubePreviewComponent` with `DSkyBoxComponent` would remove
  the specialized primitive but retain an Actor, reflected Component, scene
  identity, asset pointer, package-dirty side effects, and persistent-sky
  selection semantics for one value-only capture. It is an acceptable emergency
  compatibility bridge, not the selected final architecture.
- Adding `RenderTextureCubeThumbnail` to `IRendererModule` would mirror one UE
  surface but encode an editor asset type into Durin's runtime renderer API.
  The selected environment option names the rendering capability instead.
- A type-erased provider callback or custom render lambda would make queued work
  depend on feature-module code after registration removal and is prohibited.
- Publishing a proxy directly to `IScene` without a Component would reduce
  reflection overhead but preserve the incorrect primitive membership,
  visibility, bounds, and typed-list model.

## Current Foundations and Gaps

| Boundary | Current foundation | Gap selected by this plan |
| --- | --- | --- |
| Provider extension | Cold-generation sessions own load, readiness, setup, revision validation, and reset | Setup exposes only preview world and view; no value-only environment input |
| Shared capture | One pooled preview scene owns view, light, output target, render command, readback, and cancellation generation | Capture can submit only `IScene` plus `FSceneView`, and `RenderView` reports no failure |
| Stable texture lifetime | `DTexture` publishes a counted stable `FRHITextureReferenceRef`; the current proxy retains it | Reference retention is coupled to a fake StaticMesh Component and primitive proxy |
| Sky rendering | `FSkyBoxRenderer` owns the fullscreen triangle, shader, sampler, resource lifecycle, and scene-color ordering | TextureCube thumbnails reach it through a second renderer adapter and late primitive draw |
| Scene representation | Primitive SceneInfo provides identity, transform, bounds, visibility, and typed membership for real primitives | TextureCube preview occupies all of these structures without using their semantics |
| Visual contract | Cache identity names `WideEnvironment`; renderer produces a fixed 100-degree environment view | FOV is hidden in Renderer and implemented documentation still describes a reflection sphere |
| Failure handling | Sessions revalidate revisions before capture, after capture, and before pixel publication | A render-thread texture-resolution failure can currently yield a readable fallback/clear image instead of a failed capture |
| Tests | Provider capture, proxy stable-reference, scene typed-list, fixture, and Vulkan rendered-output coverage exists | Tests assert the legacy Component/proxy shape rather than a direct environment submission and reset contract |

## Implementation Stages

### Stage 0: Freeze the wide-environment contract and dependency inventory

Dependencies: none.

- [ ] Record the current TextureCube thumbnail output for the directional cube
  fixture at the production output size and at the smallest existing Vulkan
  integration size that retains useful orientation evidence.
- [ ] Add or tighten assertions for principal directions, representative edges,
  opaque alpha, fitted viewport behavior, and the current 100-degree vertical
  field-of-view rather than relying only on a broad screenshot comparison.
- [ ] Characterize current render counters and scene membership so removal can
  prove that the same image no longer submits a TextureCube primitive.
- [ ] Inventory every source, reflection metadata, module dependency, test, and
  lasting document that names `DTextureCubePreviewComponent`,
  `FTextureCubePreviewSceneProxy`, `TextureCubePreviewSceneInfos`, or
  `FTextureCubeThumbnailRenderer`.
- [ ] Confirm that no interactive TextureCube editor path uses the specialized
  Component; split that consumer explicitly if the inventory finds one.
- [ ] Record the cache-version rule for byte-identical versus deliberately
  changed output in the implementation handoff before Stage 1.

#### Acceptance Gate

- The current wide-environment pixels, orientation, FOV, opacity, Scene
  membership, and dependency inventory are evidence-backed; the specialized
  Component has no consumer outside the rendered TextureCube thumbnail path;
  and the cache-version decision is unambiguous before public interfaces change.

### Stage 1: Add an explicit renderer view-environment contract

Dependencies: Stage 0.

- [ ] Add RenderCore-owned `FViewEnvironmentOverride` and
  `FSceneViewRenderOptions` value types with a stable texture reference,
  rotation, tint, and intensity.
- [ ] Add a typed `RenderView` result and update `IRendererModule`,
  `FRendererModule`, `FSceneRenderer`, and the small set of engine/editor call
  sites to handle or deliberately ignore it at their owned boundary.
- [ ] Give a valid explicit environment precedence over the active scene SkyBox
  during prepared-view construction without mutating `FScene`.
- [ ] Resolve and validate the required environment texture on the render
  thread; do not substitute default black for an invalid explicit override.
- [ ] Execute the selected environment through the existing
  `FSkyBoxRenderer` before scene geometry and preserve ordinary scene-SkyBox
  behavior when no override exists.
- [ ] Add focused RenderContract coverage for empty options, explicit override,
  scene-SkyBox fallback, precedence, invalid required environment, output/resource
  failure, and unchanged ordinary viewport submission.

#### Acceptance Gate

- Runtime views render unchanged with empty options; a valid explicit
  environment draws once through the shared SkyBox path without joining Scene;
  a missing required environment returns failure and never becomes a black-cube
  success; and Renderer/RenderCore contain no TextureEditor or thumbnail type.

### Stage 2: Extend the shared thumbnail capture boundary

Dependencies: Stage 1.

- [ ] Add a provider-neutral view-environment configuration method to
  `IRenderedAssetThumbnailPreviewScene` using the RenderCore value type.
- [ ] Declare the required public module dependency for every public header that
  exposes the new type; do not depend on global include-directory leakage.
- [ ] Store view and optional environment independently in
  `FRenderedAssetThumbnailPreviewScenePool` so setter order cannot discard one.
- [ ] Snapshot both values into `BeginCapture`, pass render options to
  `IRendererModule`, and translate a failed render result into the existing
  capture failure state before readback.
- [ ] Clear the environment and release its reference on every `Reset`, failed
  preparation, cancellation, provider removal, pool replacement, and shutdown
  path already converging on pool reset.
- [ ] Add pool-level tests for setter order, overwrite, reset, failed render,
  cancellation during queued work, reference lifetime, and no cross-job leakage
  between environment-backed and scene-backed providers.

#### Acceptance Gate

- The shared core can capture either an ordinary preview scene or the same scene
  with one explicit view environment; it owns no concrete asset/provider branch;
  accepted queued work retains only core/RHI values; reset is complete and
  idempotent; and no invalid environment proceeds to readback or persistence.

### Stage 3: Migrate the TextureCube provider to value-only setup

Dependencies: Stage 2.

- [ ] Move the 100-degree environment FOV into the TextureEditor-owned visual
  contract and make `MakeTextureCubePreviewView` express the actual submitted
  projection without Renderer rewriting it.
- [ ] Make the TextureCube session pass its stable
  `FRHITextureReferenceRef` to the pool after readiness validation.
- [ ] Remove sphere retention, preview Actor creation, Component creation,
  transform setup, and all session fields used only by that world content.
- [ ] Preserve the TextureCube asset pointer only for the established
  game-thread load/readiness/revision-validation lifetime; do not capture it in
  the render command.
- [ ] Preserve asset-qualified diagnostics, opaque output, request identity,
  provider generation, cancellation serial, and pre/post-capture revision
  validation.
- [ ] Replace the Component/proxy construction test with provider/pool tests
  proving stable-reference capture, reset release, render-thread retarget
  observation, and failure when the explicit target is unavailable.
- [ ] Compare the migrated Vulkan output to the Stage 0 baseline and stop before
  legacy removal if the difference is unexplained.

#### Acceptance Gate

- TextureCube cold generation creates no Actor, Component, retained sphere, or
  primitive scene entry; its accepted command contains a stable RHI reference
  and copied view/options only; cancellation and revision changes reject stale
  publication; and the qualified rendered output matches the selected cache
  version policy.

### Stage 4: Remove the legacy TextureCube primitive path

Dependencies: Stage 3.

- [ ] Delete `DTextureCubePreviewComponent` source/header, generated reflection
  input, constructor, transient asset property, and TextureEditor module entry.
- [ ] Delete `FTextureCubePreviewSceneProxy`, its Engine implementation, and
  `EPrimitiveSceneProxyKind::TextureCubePreview`.
- [ ] Remove TextureCube preview accessors and typed membership from
  `FPrimitiveSceneInfo`, `FScene`, detach/add/release logic, scene-contract
  tests, and renderer public/private headers.
- [ ] Remove TextureCube preview classification and storage from scene
  visibility and prepared-view data.
- [ ] Delete `FTextureCubeThumbnailRenderer` and its `FSceneRenderer` member,
  preparation loop, late draw call, includes, and build metadata.
- [ ] Update exhaustive primitive-kind switches and tests without adding a
  compatibility enumerator or dead branch.
- [ ] Verify by targeted search that production source, module reflection input,
  and tests contain no legacy symbol; retain historical plan references as
  provenance rather than rewriting archives.

#### Acceptance Gate

- The Engine/Renderer primitive model contains only real primitive families;
  no TextureCube-thumbnail-specific Component, proxy, SceneInfo list,
  visibility branch, prepared collection, or renderer adapter remains; and all
  scene add/remove/visibility invariants continue to pass for retained kinds.

### Stage 5: Publish the lasting contract and qualify the editor

Dependencies: Stages 0-4.

- [ ] Update Asset Thumbnails to distinguish scene-backed thumbnail content
  from the value-only TextureCube environment path and document pool ownership,
  reset, cancellation, and module-unload behavior.
- [ ] Update Cube Textures to describe the actual wide-environment view,
  orientation, stable-reference transfer, required-resource failure, and cache
  identity instead of a reflective sphere.
- [ ] Update Renderer Scene Representation to remove TextureCube preview from
  primitive families and record view-local environment override precedence.
- [ ] Update any directly affected module-routing or rendering contract text;
  do not copy implementation-stage checklists into lasting documentation.
- [ ] Run changed-document validation and the all-plan validator.
- [ ] Run the smallest affected native targets during implementation, including
  `TextureThumbnailTests`, `ThumbnailTests`, `RenderContractTests`, and the
  scene/renderer contract target containing the removed primitive cases.
- [ ] Run full native-test validation because the final change crosses test
  targets and alters shared RenderCore/Renderer submission infrastructure.
- [ ] Complete a full `all` build because the result changes user-visible
  Content Browser rendering and the public renderer-module contract.
- [ ] Run the bounded TextureCube Content Browser/editor thumbnail smoke and
  link the verified editor executable from the same Agent Build Profile in the
  final handoff.
- [ ] Record output identity/version, focused/full test evidence, build profile,
  runtime result, removed-symbol audit, and final file/module ownership in
  Current Status before completion.

#### Acceptance Gate

- Lasting documentation and implementation agree; targeted and full native
  tests pass; the full editor build and TextureCube thumbnail smoke pass from
  one profile; no persistent cache incompatibility is left implicit; and every
  Definition of Done item has direct evidence.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Visual baseline | Wide-environment FOV, orientation, representative face/edge colors, opacity, fitted viewport, and post-process output match Stage 0 or carry an explicit version bump. |
| Render submission | Empty options preserve runtime views; explicit environment precedence is deterministic; normal scene SkyBox fallback remains unchanged. |
| Required resource | Null reference, null render-thread target, incompatible texture, and SkyBox resource failure produce capture failure and no successful persistent object. |
| Thread and lifetime | Game-thread configuration, render-thread resolution, queued-reference retention, cancellation generation, pool reset, and provider removal retain no unsafe pointer or callback. |
| Pool reuse | Setter order, overwrite, failure, reset, scene-backed follow-up, environment-backed follow-up, and shutdown cannot leak prior environment state. |
| Provider revisions | Ready revision is required before capture and revalidated after readback and before publication; retarget/rebuild/delete cannot publish stale pixels. |
| Scene cleanup | TextureCube contributes no primitive, bounds, visibility record, typed SceneInfo, prepared primitive, or late draw; retained primitive kinds conserve counters and membership. |
| Module boundary | RenderCore/RHI value types cross the public boundary; Renderer and DurinEd contain no `DTextureCube`; queued commands contain no TextureEditor-owned executable code. |
| Cache | Byte-identical output preserves identity; intentional output changes bump the selected generator/fixture versions; corrupt/warm/cancel behavior remains unchanged. |
| Integration | Focused thumbnail/render/scene targets, justified full native tests, full editor build, and a bounded Content Browser TextureCube smoke pass. |

## Definition of Done

- TextureCube thumbnail generation installs no Actor, Component, StaticMesh,
  primitive proxy, SceneInfo, visibility record, or renderer primitive draw.
- `DTextureCubePreviewComponent`, `FTextureCubePreviewSceneProxy`, the primitive
  kind, typed lists, and `FTextureCubeThumbnailRenderer` are absent from
  production source and reflection/build metadata.
- One RenderCore-owned value contract carries an optional explicit environment
  through `IRendererModule` without naming TextureCube assets or thumbnails.
- Explicit environment rendering uses `FSkyBoxRenderer`, precedes geometry,
  overrides scene sky only for that submission, and never converts a missing
  required resource into black-fallback success.
- The shared thumbnail pool snapshots and clears the environment safely across
  success, failure, cancellation, provider removal, reuse, and shutdown.
- TextureEditor retains asset ownership only for load/readiness/revision checks;
  queued render work retains a counted stable RHI reference and copied values.
- Material, StaticMesh, and SkeletalMesh thumbnails retain their existing
  preview-world Component behavior and rendered baselines.
- Persistent cache identity is preserved for byte-identical output or explicitly
  versioned for an intentional change.
- Asset Thumbnails, Cube Textures, and Renderer Scene Representation contain the
  final lasting contracts and no longer describe the removed reflection-sphere
  thumbnail path.
- Required focused/full tests, documentation validation, full editor build, and
  bounded user-visible runtime qualification pass with recorded evidence.

## Deferred Follow-ups

- Lazily create `FPreviewScene` only after a scene-backed provider requests a
  world, allowing direct-only TextureCube jobs to avoid preview-world and light
  construction. Measure the shared scene cost before selecting this change.
- Add a reusable interactive preview-environment controller if MaterialEditor,
  StaticMeshEditor, SkeletalMeshEditor, or TextureEditor needs user-selectable
  HDRI environments. Do not extend the thumbnail-only interface speculatively.
- Revisit whether `IRendererModule::RenderView` should eventually accept one
  owning request structure instead of positional scene/view/output/options
  parameters when another independent submission option appears.
- Add richer typed render diagnostics only if callers other than the thumbnail
  pool need to present distinct renderer failure categories.
- Revisit the interactive TextureCube editor's reflection-sphere presentation
  separately; its inspection goal differs from Content Browser recognition.

## Related Documentation

- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Cube Textures](../Runtime/Rendering/CubeTextures.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Static Mesh Render Data Lifecycle](Archive/2026-08/StaticMeshRenderDataLifecycle.md)
- [Static Mesh LOD Resources Refactor](Archive/2026-08/StaticMeshLODResourcesRefactor.md)
- [Renderer SceneProxy and SceneInfo Contract](Archive/2026-08/RendererSceneProxyAndInfoContract.md)
- [Per-View Visibility and LOD](Archive/2026-08/PerViewVisibilityAndLOD.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/IRendererModule.h`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Renderer/Public/RendererModule.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkyBoxRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkyBoxRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TextureCubeThumbnailRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TextureCubeThumbnailRenderer.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/FPrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailExtension.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailPreviewScene.h`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailCache.cpp`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnailTypes.h`
- `Engine/Source/Editor/TextureEditor/Public/Preview/TextureCubePreviewComponent.h`
- `Engine/Source/Editor/TextureEditor/Private/Preview/TextureCubePreviewComponent.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/TextureCubeAssetThumbnail.cpp`
- `Engine/Source/Editor/TextureEditor/TextureEditor.dmodule`
- `Engine/Tests/Native/EngineTests/Private/TextureAssetThumbnailTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Thumbnail/RenderedAssetThumbnailTestFixtures.h`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RenderedAssetThumbnailFixtureTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/TextureReferenceTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/TextureResourceLifetimeContractTests.cpp`
