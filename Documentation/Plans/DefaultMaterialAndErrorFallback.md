# Default Material and Error Fallback Plan

Summary: Replace the ambiguous yellow material fallback with an asset-backed Engine default material and an asset-independent error surface.

Last reviewed: 2026-08-05

Status: Completed
Completed: 2026-08-05

## Current Status

Completed on 2026-08-05 from implementation baseline
`493f5da91c6ca73aa9e1138ef6799e3224ae8fc4` (the plan's architectural baseline
remains `098fe5fbc23edb0909c6637c5cd44a9b67973114`).

Implementation handoff:

- Working set: material render representation/proxy compilation, the Engine
  default-material service and lifecycle, StaticMesh component/proxy binding,
  Renderer v2 rejection, Level Editor slot presentation, generic package-only
  Cook publication, the checked-in default asset, focused native tests, and the
  owning Runtime documentation.
- Key symbols: `GetErrorMaterialRenderData`, `FDefaultMaterialService` public
  functions, `GetEngineBuiltInCookRoots`, `EMaterialFallbackReason`,
  `DStaticMeshComponent::CreateSceneProxy`, and
  `FStaticMeshRenderer::DrawProxy_RenderThread`.
- Frozen extension points: `DEngine::Init` initializes the service after
  mounts/RHI/render admission and before scene/world creation;
  `DEngine::BeginDestroy` releases it after consumer detachment;
  Engine exposes fixed Cook roots and `FCookContext` publishes ordinary
  package-only assets without empty `.dbulk` companions. `DevTool` owns no
  material path or policy.
- References: DefaultMaterial is exact-v2 lit neutral gray `(0.5, 0.5, 0.5)`;
  ErrorMaterial is exact-v2 unlit magenta `(1, 0, 1)`, two-sided and
  depth-writing. The existing PBR CPU reference tolerance remains `1e-6`; the
  error terminal's no-light visibility is fixed by unlit identity and tested
  without AssetCore or RHI resources.
- Decisions: empty serialized assignments remain null; one retained authored
  proxy serves every normal empty slot; invalid compilation, structural proxy,
  default-asset, or Renderer layout state selects the non-recursive code
  terminal; texture/environment recovery remains resource-local.
- Open questions: none. Project/world overrides, domain-specific defaults, a
  checkerboard error shader, neutral new-user-material defaults, and future
  asynchronous last-known-good policy remain the deferred follow-ups below.
- Validation: focused ErrorMaterial/default-service/Cook/StaticMesh/editor-model
  tests passed; the complete native aggregate passed; the complete `all` build
  passed; and `DurinEditor` completed a hidden-window three-tick startup and
  orderly shutdown through `DurinDevTool`.

The plan starts from baseline
`098fe5fbc23edb0909c6637c5cd44a9b67973114`, after completion and integration of the PBR
Material Surface plan.

An unassigned StaticMesh material slot currently becomes an empty
`FMaterialRenderProxyRef`. `FStaticMeshSceneProxy` resolves that empty binding
to a default-constructed `FMaterialRenderData`, whose v2 uniform seed retains
the early orange/yellow test BaseColor `(0.95, 0.62, 0.22)`. Renderer validation
failure also selects a default-constructed `FMaterialRenderData`. Normal absence
and broken material state therefore share one appearance and one ambiguous
"Renderer Fallback" label.

The existing representation is already safe and deterministic: Engine owns
the exact v2 layout, material proxies publish counted immutable snapshots, and
Renderer owns texture and environment fallbacks. The missing boundary is
semantic. A valid but unassigned surface should use a real Engine material
asset; invalid render state must remain diagnosable even when asset loading is
unavailable.

StaticMesh integration must build on the landed positional contract:
`DStaticMeshComponent::OverrideMaterials` is indexed by the mesh's stable slot
table, mesh changes preserve dormant entries, and slot matching/reimport owns no
GUID or orphan path. This plan changes only the final unassigned fallback after
component override and mesh default resolution; it must not reintroduce slot
identity or compatibility storage.

## Goal

Establish three distinct outcomes for every StaticMesh material binding:

1. a valid explicitly assigned material;
2. the valid asset-backed Engine `DefaultMaterial` when no material is
   assigned; or
3. an asset-independent `ErrorMaterial` render representation when material
   state is invalid or the Engine default asset cannot be used.

An ordinary material omission must look neutral and intentional. A content or
engine contract error must remain conspicuous, deterministic, and renderable
without loading another asset.

## Scope

- One built-in `DMaterial` asset at fixed virtual path
  `/Engine/Materials/DefaultMaterial`.
- Explicit Engine ownership, startup loading, retention, shutdown, and Cook
  inclusion for that asset.
- One code-constructed error render-data singleton that has no package,
  texture, DDC, Cook, or asset-manager dependency.
- Explicit fallback reasons and non-recursive selection rules at material
  compilation, proxy binding, and Renderer validation boundaries.
- StaticMesh empty-slot resolution, live binding updates, import defaults,
  Details presentation, preview/thumbnail behavior, diagnostics, tests, and
  Runtime documentation.
- Removal of the early orange/yellow test color from the no-material and
  invalid-material runtime paths.

## Non-Goals

- Project-configurable or per-world default materials.
- Serializing the built-in material into every StaticMesh slot or component
  override.
- Changing the permanent PBR v2 layout identity or the schema-v2 parameter
  defaults used by existing material assets and upgrade behavior.
- Material graphs, shader compilation, asynchronous material compilation, or
  new material domains.
- Masked/translucent passes, blending, culling, or depth-policy changes.
- Replacing Renderer-owned white, black, flat-normal, or environment resource
  fallbacks; those remain resource-level recovery rather than whole-material
  substitution.
- A procedural checkerboard shader permutation. The initial ErrorMaterial is a
  solid unlit diagnostic surface and does not expand the v2 material layout.

## Reference Model and Selected Divergence

Unreal Engine treats its default material as a special material asset:
`UMaterial::GetDefaultMaterial` loads the appropriate default material when
needed, and material render proxies may select it for content errors or
incomplete shader state. Unreal also identifies default/fallback materials as
special Engine materials that must have broad shader coverage.

Durin adopts the asset-backed default-material ownership but deliberately
separates its error terminal:

- `DefaultMaterial` means valid absence of an assignment and is ordinary Engine
  content.
- `ErrorMaterial` means a broken contract and is built without AssetCore.
- A broken `DefaultMaterial` never falls back to itself and never triggers a
  second load.

This divergence prevents missing or corrupt Engine content from erasing the
only visual diagnostic and prevents broken user materials from appearing like
intentional neutral surfaces.

Reference APIs:

- [Unreal `UMaterial::GetDefaultMaterial`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterial)
- [Unreal `FMaterialRenderProxy::GetMaterialWithFallback`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialRenderProxy/GetMaterialWithFallback)
- [Unreal special Engine materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/shader-development-in-unreal-engine)

## Design Decisions and Invariants

### Selection Contract

Material selection is decided before drawing and has one terminal result:

| Condition | Selected surface | Diagnostic |
| --- | --- | --- |
| Component override resolves to a valid material | Assigned material | None |
| No override, mesh slot has a valid default material | Mesh default material | None |
| Neither component nor mesh assigns a material | Engine `DefaultMaterial` asset | None |
| Explicit material publishes invalid data | Code `ErrorMaterial` | Asset-qualified material error |
| Renderer receives unsupported/malformed layout | Code `ErrorMaterial` | `ShaderBinding` error |
| Default asset is missing, wrong type, invalid, or unavailable | Code `ErrorMaterial` | One Engine-default error with path and reason |
| A role texture is null/not ready/failed | Same selected material plus role texture fallback | Existing resource diagnostic |
| Studio environment is unavailable | Same selected material plus black environment set | Existing environment diagnostic |

An unassigned slot is not an error and emits no warning. A null proxy that
reaches `FStaticMeshSceneProxy` after normal binding resolution is a structural
error rather than an alternative spelling for "unassigned" and selects
ErrorMaterial.

The current engine has no asynchronous material compiler. If one is added
later, a valid last-known-good snapshot remains preferred; an assigned material
with no prior ready snapshot may temporarily use DefaultMaterial, while a
reported compile/content failure uses ErrorMaterial. This rule is recorded for
future compatibility but does not add asynchronous state in this plan.

### DefaultMaterial Asset

- Virtual path: `/Engine/Materials/DefaultMaterial`.
- Source package:
  `Engine/Content/Materials/DefaultMaterial.dasset`.
- Type: base `DMaterial`, not `DMaterialInstance`.
- Initial authored surface: BaseColor `(0.5, 0.5, 0.5)`, Normal `(0, 0, 1)`,
  Metallic `0`, Roughness `0.5`, AmbientOcclusion `1`, Emissive `(0, 0, 0)`,
  Opacity `1`, and OpacityMask `1`, with null textures and default UV
  transforms.
- Static properties: Opaque, Lit, one-sided, and Automatic depth write. The
  current fixed opaque pipeline may not yet expose every property, but the
  asset records the intended future behavior.
- The asset compiles through the same canonical v2 representation and proxy as
  every other `DMaterial`; Renderer has no default-asset-specific shader path.
- The checked-in values are reference-tested, not hard-coded into the loader.
  An intentional asset appearance change updates the asset and rendered
  references without changing the v2 layout.
- The asset is discoverable when Engine Content is shown, but ordinary empty
  slots store null and resolve through the Engine service. An explicit user
  assignment of this same asset remains an explicit assignment.
- The initial asset references no textures. This keeps startup and Cook
  dependencies minimal but is not a new restriction on `DMaterial` itself.

### Engine Ownership and Lifecycle

- Engine, not Renderer, AssetCore, editor code, or `DevTool`, owns the default
  material policy.
- Add one explicit Engine material service, provisionally
  `FDefaultMaterialService`, initialized on the game thread after Engine Content
  is mounted and AssetCore is ready, but before scene proxies may be created.
- Initialization synchronously loads the fixed path once, verifies the exact
  asset type, requests its stable `FMaterialRenderProxyRef`, and retains the
  asset/proxy for the Engine lifetime. No render-thread code loads assets.
- The service exposes a game-thread accessor returning the counted default
  proxy. It does not expose the reflected material object to Renderer.
- Shutdown stops new default bindings after scene/preview/thumbnail consumers
  detach, then releases the retained asset/proxy through the established
  material proxy and Engine shutdown ordering.
- Initialization failure is non-fatal. The accessor returns an empty proxy,
  normal binding code selects ErrorMaterial, and one deduplicated diagnostic
  retains the fixed path and AssetCore failure.
- Repeated components, views, previews, and thumbnails reuse the same counted
  proxy identity. They do not load the asset independently.

The service may be process-global internally because material selection is a
cross-component Engine contract, but it must have explicit Engine-driven
`Initialize`/`Shutdown` calls and game-thread assertions. Lazy first use and
function-local asset loading are not permitted.

### ErrorMaterial Terminal

- ErrorMaterial is an Engine-owned `FMaterialRenderData` constructed from code;
  it is not `DMaterial`, has no virtual path, and never enters AssetCore or Cook.
- It uses the exact current v2 layout, zero texture references, BaseColor
  `(1, 0, 1)`, Opacity `1`, Opaque, Unlit, two-sided, and depth-writing
  behavior. Unlit magenta remains visible without direct light, IBL, or texture
  resources.
- Expose an immutable `GetErrorMaterialRenderData()`-style accessor usable from
  the render thread. Construction must not enqueue work or touch reflected
  objects.
- Replace ambiguous `bFallback`/`IsFallback` representation semantics with an
  explicit error classification such as `IsError()`. A valid DefaultMaterial
  representation is authored data and is never marked as an error/fallback.
- Separate the canonical v2 builder seed from ErrorMaterial construction. The
  early orange/yellow schema seed must no longer double as the Renderer error
  terminal.
- `FMaterialRenderRepresentation::TryCreate`, material compilation failure,
  an empty/invalid proxy, and Renderer v2 binding rejection all converge on the
  same immutable ErrorMaterial data without an asset lookup.
- Renderer never recursively validates a newly selected fallback. The error
  singleton is validated by focused tests and startup invariants; if its exact
  binding is nevertheless incompatible, checked builds report an invariant
  failure and production skips the affected draw after recording one
  diagnostic.

### StaticMesh and Editor Semantics

- Preserve assignment precedence: component override, mesh default, Engine
  DefaultMaterial.
- `DStaticMeshComponent::CreateSceneProxy` binds the Engine default proxy for a
  valid unassigned slot. Clearing an override or mesh default immediately
  rebinds that proxy through the existing component revision ordering.
- Import continues to create a stable material slot when source material data
  is absent, but does not serialize an explicit reference to the Engine asset.
- Rename the user-facing `Renderer Fallback` source to `Engine Default` (or an
  equally explicit final name) and display the built-in virtual path. Error is
  presented as a diagnostic state, not an assignable source option.
- StaticMesh level rendering, mesh/material preview, thumbnails, and any other
  empty-slot consumers must use the same service and proxy identity.
- A missing material slot index, stale binding, or proxy publication failure is
  not silently reclassified as Engine Default; it uses ErrorMaterial.

### Cook and Distribution

- DefaultMaterial uses ordinary `DMaterial` package serialization and asset
  Cook. It owns no `.dbulk`, DDC format, or custom payload.
- The fixed path is a soft Engine dependency, so the Engine material subsystem
  registers it as a required built-in Cook root. Cook inclusion belongs to
  Engine/asset cooking logic; `DevTool` receives no material-specific command or
  path.
- Editor/source mode loads the package from Engine Content. Cooked runtime mode
  loads the same virtual path from cooked Engine content and never falls back to
  the source package.
- A Cook integration test must prove that a minimal project with no explicit
  material reference still publishes and loads DefaultMaterial. Omitting or
  corrupting it must exercise the non-fatal ErrorMaterial terminal.

### Diagnostics

- Use distinct reason names in counters/logs/tests: `UnassignedDefault`,
  `DefaultAssetUnavailable`, `MaterialDataInvalid`, `UnsupportedLayout`, and
  `MissingProxy` (final enum spelling may follow the nearest diagnostics API).
- Normal use of DefaultMaterial is observable through a counter/debug snapshot
  but does not log per component or per frame.
- Default asset initialization failure logs once per Engine lifecycle.
- Invalid user material and Renderer binding diagnostics retain the offending
  asset/proxy identity where available and state that ErrorMaterial was used.
- Existing role-texture and environment errors remain separate; they must not
  increment whole-material fallback counters.

## Current Foundations and Gaps

### Foundations

- `DMaterial` already serializes the complete canonical PBR parameter set and
  publishes a stable counted `FMaterialRenderProxyRef`.
- `FMaterialRenderRepresentation` validates an immutable exact v2 layout before
  Renderer consumption.
- StaticMesh component binding updates already preserve slot ordering and reject
  stale revisions.
- Engine Content mounts and typed `Asset::LoadAsset` support fixed built-in
  virtual paths.
- Renderer-owned white, black, flat-normal, and environment fallback resources
  remain available when the selected material contains null references.
- Material, StaticMesh, preview, thumbnail, Cook, reload, and real Vulkan test
  owners already exist.

### Gaps

- Empty slots and invalid data both select default-constructed yellow render
  data.
- No material subsystem owns or loads a built-in DefaultMaterial asset.
- Default construction, canonical builder seed, and error recovery share one
  representation and the ambiguous `IsFallback` flag.
- The StaticMesh Details source is named `Renderer Fallback`, hiding whether
  the state is normal absence or an actual error.
- A fixed-path Engine asset needs explicit Cook-root coverage because empty
  slots intentionally serialize no asset reference.
- Diagnostics cannot currently distinguish whole-material selection from
  texture/environment resource fallback.

## Implementation Stages

### Stage 0: Freeze Fallback Semantics and References

Dependencies: completed PBR Material Surface plan.

- [x] Revalidate every production/test construction of empty
  `FMaterialRenderData`, `IsFallback`, null material proxies, and
  `RendererFallback` UI state against the selection table above.
- [x] Freeze the DefaultMaterial path, authored values, static properties,
  ErrorMaterial values, classification names, and initialization/shutdown
  ordering.
- [x] Freeze CPU binding references for neutral lit DefaultMaterial and unlit
  magenta ErrorMaterial, plus rendered-output framing and tolerances.
- [x] Record exact Engine startup and Cook extension points without adding a
  material rule to `DevTool`.

#### Acceptance Gate

- Every absence, validation failure, resource failure, startup failure, and
  future not-ready condition has one non-recursive selected result.
- No open decision can change serialized asset identity, module ownership,
  Cook inclusion, or Renderer behavior in later stages.

### Stage 1: Establish the Asset-Independent Error Terminal

Dependencies: Stage 0.

- [x] Add the immutable code-constructed ErrorMaterial render data with exact
  v2, opaque, unlit, two-sided magenta behavior and no asset/RHI dependency.
- [x] Separate canonical representation seeding from error construction and
  replace ambiguous fallback classification with explicit error semantics.
- [x] Route representation creation failure, material compilation failure,
  null structural proxies, and Renderer layout rejection to ErrorMaterial.
- [x] Add non-recursive invariant handling if the error singleton cannot decode
  under the current exact v2 binding.
- [x] Add focused representation, proxy, validation, diagnostic, and CPU
  surface tests.

#### Acceptance Gate

- ErrorMaterial is visible without assets, textures, direct light, or IBL and
  no invalid path can recurse into material or asset loading.
- Ordinary valid materials and resource-level fallbacks retain existing output.

### Stage 2: Add the Engine DefaultMaterial Asset and Service

Dependencies: Stage 1.

- [x] Create and verify the neutral `DMaterial` package at the fixed Engine
  path.
- [x] Add the explicit game-thread Engine material service and integrate it
  after Engine Content mount/AssetCore startup and before scene proxy creation.
- [x] Retain one stable default proxy across level, preview, and thumbnail
  consumers; release it through ordered Engine shutdown.
- [x] On missing, wrong-type, corrupt, or invalid default content, emit one
  diagnostic and expose ErrorMaterial without making startup fatal.
- [x] Register the fixed path through Engine-owned built-in Cook roots and
  prove source and cooked loading without `DevTool` knowledge.
- [x] Add asset, lifecycle, Cook, missing-content, and shutdown tests.

#### Acceptance Gate

- DefaultMaterial is a real inspectable/cookable Engine asset and normal empty
  slots can acquire one shared valid proxy before rendering.
- Removing all asset availability still leaves a deterministic magenta error
  surface and actionable diagnostic.

### Stage 3: Integrate StaticMesh, Import, Preview, and Editor Presentation

Dependencies: Stage 2.

- [x] Bind the Engine default proxy for every valid unassigned StaticMesh slot
  while preserving component override and mesh-default precedence.
- [x] Preserve null serialized assignments during import, save/reload,
  reimport, duplication, Undo/Redo, and cooked loading.
- [x] Rebind default/explicit material transitions through the existing stable
  proxy and component revision contract without recreating unrelated state.
- [x] Replace the `Renderer Fallback` Details label with explicit Engine
  Default presentation and keep ErrorMaterial diagnostic-only.
- [x] Align level, preview, thumbnail, and auxiliary viewport consumers.
- [x] Add assignment precedence, live update, import, editor-model,
  preview/thumbnail, and serialization tests.

#### Acceptance Gate

- Dragging a no-material mesh into a scene produces the neutral Engine default
  surface everywhere, with no warning and no yellow test fallback.
- Broken material state produces magenta everywhere and cannot be mistaken for
  an ordinary empty slot.

### Stage 4: Close Validation and Documentation

Dependencies: Stage 3.

- [x] Search production and tests for remaining implicit default construction,
  ambiguous fallback labels, per-consumer default loads, or recursive fallback.
- [x] Run focused material, StaticMesh, import, asset package/Cook, preview,
  thumbnail, shader binding, renderer reload, and real Vulkan output coverage.
- [x] Run the complete native suite, full `all` build, and hidden-window editor
  smoke through the repository entrypoint.
- [x] Update Runtime material, asset lifecycle/Cook, StaticMesh, viewport, and
  diagnostics contracts with the selected ownership and failure table.
- [x] Record the final baseline, working set, decisions, open questions, and
  validation evidence, then complete the plan.

#### Acceptance Gate

- Default and error surfaces remain distinct through source/cooked runtime,
  reload, device retry, multi-view, and shutdown coverage.
- Long-lived rules live in owning Runtime documentation and the completed plan
  contains a compact implementation handoff.

## Validation Matrix

| Area | Required coverage | Acceptance |
| --- | --- | --- |
| Selection | override, mesh default, empty slot, invalid proxy/layout | Exact terminal from the frozen table |
| Default asset | path, type, values, proxy identity, retention | One valid shared authored material |
| Error terminal | no AssetCore/RHI/light/IBL, exact v2, recursion guard | Unlit magenta or explicit draw skip on invariant failure |
| Lifecycle | startup order, repeated init guard, shutdown, stale commands | No lazy render-thread load or dangling proxy |
| Serialization | import, save/reload, reimport, duplicate, Undo/Redo | Empty assignment remains null and semantic |
| Cook | unreferenced minimal project, cooked load, omission/corruption | Required Engine asset or non-fatal error terminal |
| Editor | source labels, clearing assignments, preview, thumbnails | Engine Default is normal; Error is diagnostic |
| Resources | missing textures, IBL absence, device retry/reload | Same material with existing resource fallbacks |
| Rendered output | level, preview, thumbnail, no-light error case | Neutral default and conspicuous error references |
| Aggregate | full native suite, full build, editor smoke | No regression and linked editor starts normally |

## Definition of Done

- A no-material StaticMesh uses `/Engine/Materials/DefaultMaterial` as a normal
  authored material without serializing that reference into empty slots.
- Invalid material state uses an asset-independent, exact-v2, unlit magenta
  ErrorMaterial with an actionable reason.
- Default asset failure is non-fatal and cannot recurse.
- Engine owns startup, retention, Cook inclusion, and shutdown; Renderer and
  `DevTool` contain no asset-policy logic.
- Assignment precedence, live updates, import, source/cooked loading, preview,
  thumbnail, reload, Vulkan output, full build, and editor smoke are validated.
- Runtime documentation owns the lasting selection, lifecycle, Cook, and
  diagnostics contracts.

## Deferred Follow-ups

- Project or world override of the Engine DefaultMaterial.
- Domain-specific default materials after more material domains exist.
- A procedural checkerboard/error permutation if solid magenta proves
  insufficient in real diagnostics.
- Neutral defaults for newly created user `DMaterial` assets; changing the
  frozen schema-v2 authored defaults requires a separate compatibility review.
- Last-known-good/default placeholder policy when asynchronous material
  compilation is implemented.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [PBR Material Surface Plan](PBRMaterialSurface.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderProxy.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/StaticMeshMaterialSlotDetails.cpp`
- `Engine/Content/Materials/DefaultMaterial.dasset`
