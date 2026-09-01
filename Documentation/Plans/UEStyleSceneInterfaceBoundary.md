# UE-Style Scene Interface Boundary Plan

Summary: Make component-level void Add/Remove operations the only public scene lifecycle API while keeping proxy admission and ownership inside Renderer.

Last reviewed: 2026-09-01

Status: Completed
Completed: 2026-09-01

## Current Status

The preceding proxy-lifecycle plan established complete immutable Light,
SkyBox, and VolumetricCloud proxies, exact-pointer retirement, and safe
command-admission failure. This plan removed the remaining public mix of
Primitive ID/proxy publication, feature proxy publication, and recoverable
`bool` Add results.

This plan selects an Engine-owned virtual component boundary, Renderer-private
proxy admission helpers, and explicit one-shot Scene release. Primitive dynamic
updates remain ID/data-based.

## Goal

- Expose only component-level `void` Add/Remove lifecycle operations.
- Keep proxy creation synchronous on the game thread and never capture a
  reflected component in a render command.
- Treat command-admission failure as a required lifecycle violation while
  preserving leak-safe proxy destruction internally.
- Require every owned Scene to be explicitly released before reset and before
  Renderer shutdown.
- Keep concrete Scene and proxy mutation private to Renderer and tests.

## Selected Decisions

- `FSceneInterface` declares only public virtual component lifecycle methods
  returning `void`. Renderer-private `FScene` implements component validation,
  proxy construction, token state, and private `TryAdd/Remove*Proxy` helpers;
  low-level admission is not part of the Engine interface or its vtable.
- Light, SkyBox, and VolumetricCloud components retain raw pointers only as
  opaque removal tokens; Primitive retains its stable ID plus a publication
  flag.
- Add writes component publication state only after command admission succeeds;
  Remove clears it only after retirement admission succeeds.
- Proxy absence caused by visibility or unsupported render representation is a
  legal no-publication result. Scene mismatch, duplicate publication, invalid
  threading, and admission failure are required contract failures.
- `FScene::Release()` is explicit, one-shot, and non-idempotent. It transitions
  `Active -> Releasing`, queues render-thread clearing, then transitions to
  `Released`. The owning `FScenePtr` may reset only after Release was requested.
- Ordinary lifecycle operations never flush rendering commands.

## Implementation Stages

### Stage 0: Establish the Engine component boundary

- [x] Add component-level public virtual methods to `FSceneInterface` without
  proxy/admission methods in the upper interface.
- [x] Add the component friendship and private proxy builders/publication state
  needed by the concrete Renderer Scene without exposing Renderer types.
- [x] Add compile-time contract coverage for void component APIs and hidden
  proxy APIs.

Stage 0 is complete when Engine callers cannot publicly publish a proxy or
observe an Add admission result.

### Stage 1: Migrate component render state

- [x] Move Primitive create/destroy/recreate onto component Add/Remove and keep
  dynamic updates ID/data-based.
- [x] Move Light, SkyBox, and VolumetricCloud create/destroy/dirty paths onto
  the component boundary.
- [x] Preserve visibility, eligibility, deterministic selection, history keys,
  and exact-pointer retirement.

Stage 1 is complete when every converted component delegates publication state
to `FSceneInterface` and captures no reflected object in render commands.

### Stage 2: Privatize Renderer mutation

- [x] Implement private proxy helpers in `FScene` with leak-safe admission and
  required Active-state checks.
- [x] Remove public concrete proxy overloads and move `Scene.h` to Renderer
  Private.
- [x] Replace Primitive add-or-replace lifecycle with ordered remove-old/add-new
  while preserving the ID-keyed Registry and dynamic update contracts.
- [x] Provide a controlled friend test accessor for direct Renderer contracts.

Stage 2 is complete when production code has one component-level lifecycle
entry and Renderer tests alone can reach the proxy seam.

### Stage 3: Require explicit Scene release

- [x] Add `Active`, `Releasing`, and `Released` Scene states and one-shot
  `Release()`.
- [x] Require Release before `FScenePtr` reset, render-thread-only final
  destruction, and empty Registries at destruction.
- [x] Track active and allocated scenes so Renderer shutdown rejects live
  owners and verifies final deletion after its barrier.
- [x] Update Engine, Preview Scene, and test owners to detach components,
  Release, then reset.

Stage 3 is complete when Renderer shutdown cannot overtake a live or unreleased
Scene.

### Stage 4: Qualification and lasting documentation

- [x] Cover component APIs, admission rejection, exact retirement, pending
  Release, duplicate Release, missing Release, and shutdown constraints.
- [x] Update authoritative Scene, lighting, SkyBox, and cloud runtime contracts.
- [x] Run focused native targets, final `test affected`, changed/all document
  validation, and all-plan validation.
- [x] Record evidence and complete this plan.

Stage 4 is complete when all acceptance gates pass without transitional public
proxy APIs or ordinary lifecycle flushes.

## Validation Evidence

- `RendererSceneContractTests`: 44 passed.
- `WorldTests`: 107 passed.
- `SkyBoxTests`: 11 passed.
- `VolumetricCloudSceneContractTests`: 7 passed.
- `ViewportTests`: 105 passed after preserving Primitive registration
  notifications for the editor picking index.
- `SkeletalMeshRenderResourcesVulkanTests`,
  `StaticMeshRenderPreparationVulkanTests`, `TerrainRenderVulkanTests`, and
  `EditorGridVulkanTests`: passed with Scene release ordered before Renderer or
  rendering-thread shutdown.
- `DirectionalShadowBaselineVulkanTests`, `GBufferQualificationTests`, and
  `TerrainRenderQualificationTests`: qualification mode passed.
- Final `test affected`: all 61 selected native-test targets passed.
- Changed/all documentation and all-plan validation passed.

## Acceptance Gates

- [x] Public lifecycle Add/Remove accepts components, returns `void`, and never
  exposes proxy ownership or admission.
- [x] Component pointers are read only synchronously on the game thread and are
  never captured by Renderer commands.
- [x] Admission failure destroys an unpublished proxy exactly once and becomes
  a required high-level contract failure.
- [x] Primitive rebuild and feature rebuild retire old state before adding new
  state in the same FIFO stream.
- [x] Release is explicit and one-shot; final Scene/SceneInfo/proxy destruction
  occurs on the render thread before Renderer shutdown completes.
- [x] Existing selection, lighting, dynamic Primitive update, texture lifetime,
  and cloud history behavior remains unchanged.
- [x] `test affected`, changed/all documentation validation, and all-plan
  validation pass.

## Related Code and Contracts

- [`SceneInterface.h`](../../Engine/Source/Runtime/Engine/Public/SceneInterface.h)
- [`SceneOwnership.h`](../../Engine/Source/Runtime/RenderCore/Public/SceneOwnership.h)
- [`Scene.cpp`](../../Engine/Source/Runtime/Renderer/Private/Scene.cpp)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Completed Proxy Lifecycle Plan](Archive/2026-09/UEStyleSceneProxyLifecycle.md)
