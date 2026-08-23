# Volumetric Cloud Production Qualification Plan

Summary: Close the volumetric-cloud roadmap through a frozen cross-feature runtime, cook, editor, lifecycle, and publication matrix.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

All stages are complete. P0 through P5 and P2.5 are archived with their required
focused, aggregate, Vulkan, package/cook, full-build, documentation, and Editor
smoke evidence. P6 therefore changes no cloud algorithm or public contract; it
closes the program by replaying the smallest cross-feature correctness matrix,
reviewing the already accepted same-adapter performance evidence, publishing
the final validation ownership, and completing the roadmap. The cloud scene
contract (6 tests), Renderer scene/transaction contract (27 tests), focused
volume import/cook (6 tests), exact volume preview (2 tests), cloud Details
(1 test), and both inline/threaded Vulkan lifecycle and authored-scene route
targets pass with two build jobs.

The 58-target `fast-all` contract/feature/infrastructure aggregate passed with
two build and test jobs in 260.62 seconds. The full `all` target then passed
with the same limit. The verified checkout-local DurinEditor binary remained
healthy through the bounded eight-second startup smoke before the harness
closed it. Renderer/Vulkan lifecycle tests own orderly resource and application
shutdown independently from that bounded startup observation.

The 2026-08-23 qualification session shares the workstation with other agents.
Per the repository testing contract, GPU timing from this session is diagnostic
only. P6 will not rebaseline or replace the accepted quiet-lane RTX 3090 /
Vulkan 1.4.325 P3/P4 measurements. Correctness, image, route, lifetime, and
release assertions remain required in both command-executor lanes.

## Goal

Close the volumetric-cloud roadmap with reproducible evidence that its authored
asset, scene publication, spatial/temporal/lighting renderer, viewport/editor,
package/cook, recovery, and shutdown boundaries work together without adding a
new feature or weakening a previously accepted numeric gate.

## Scope

- Freeze and execute the final cross-feature test matrix using existing named
  targets and their production fixtures.
- Exercise compute and fragment routes, inline and threaded command execution,
  offscreen and Present output, resize, temporal history, cloud shadows,
  invalidation/retry, release, import/reimport/cook, previews, Details, and
  editor persistence.
- Review the accepted P3/P4 image, GPU-time, p95, memory, and work-structure
  evidence without taking new shared-machine timings as a baseline.
- Run the full editor build and smoke path after bounded tests pass.
- Repair active documentation links, publish final ownership through existing
  Runtime/Editor contracts, complete P6, and close the roadmap.

## Non-Goals

- New cloud algorithms, atmosphere integration, local volumes, multiple cloud
  layers, broader source formats, asynchronous compute, or a render graph.
- Performance rebaselining while another agent or GPU workload may compete.
- Repeating every historical child-plan test when an existing aggregate or
  focused cross-feature target already covers the boundary.

## Design Decisions and Invariants

- P6 consumes the shipped `High` default, quality tiers, route fallback,
  composition order, history transaction, selected-light, shadow receiver,
  import, and editor contracts unchanged.
- The named RTX 3090 / Vulkan 1.4.325 P3/P4 measurements recorded on
  2026-08-23 remain the performance authority because no later runtime cloud
  implementation change exists. Shared-machine reruns cannot revise them.
- Both `inline` and `threaded` executor lanes are required for Vulkan cloud
  correctness. Tests run sequentially so independently launched agents cannot
  be coordinated by the local `durin-gpu` lock.
- A missing or invalid cloud, failed target/pipeline/history candidate, or
  recovery transition remains non-fatal to the containing view and publishes
  either last-known-good or the documented identity fallback.
- Existing Runtime and Editor documents remain authoritative. The roadmap and
  completed plan record closure evidence; no duplicate production-status
  contract is introduced.

## Current Foundations and Gaps

| Boundary | Existing evidence | P6 closure |
| --- | --- | --- |
| Asset and authoring | P2/P2.5/P5 import, reimport, cook, preview, Details, persistence, and Editor smoke gates | Replay the focused volume-import, preview, and viewport customization coverage. |
| Scene publication | P2 stable identity, selection, immutable proxy, mutation, and release coverage | Replay the cloud scene contract target. |
| Spatial/temporal/lighting | P1/P3/P4 public-RHI routes, history, lighting, shadow, recovery, and qualification | Replay Vulkan lifecycle and scene routes in both executors. |
| Performance and memory | Accepted P3/P4 RTX 3090 / Vulkan 1.4.325 image, median/p95, work, and retained-byte gates | Audit the recorded evidence; do not rebaseline under contention. |
| Program handoff | Lasting Runtime/Editor documents exist, but P6 and the roadmap are open | Validate documentation, complete this plan, archive the roadmap, and leave the plan in the shared completed-plan batch queue. |

## Implementation Stages

### Stage 0: Freeze the cross-feature matrix

- [x] Confirm every P0-P5 and P2.5 dependency is archived with its required
  acceptance evidence and no later cloud-runtime implementation change exists.
- [x] Freeze sequential inline/threaded correctness lanes and prohibit shared
  workstation timings from changing accepted performance gates.
- [x] Map asset/cook, editor, scene, renderer, viewport, recovery, build, smoke,
  documentation, and publication boundaries to named validation owners.

#### Acceptance Gate

- Scope, decisions, validation commands, performance provenance, and closure
  ownership are explicit before executing P6.

### Stage 1: Replay bounded cross-feature correctness

- [x] Run the cloud scene contract and Renderer transaction/statistics coverage.
- [x] Run volume import/cook, exact slice-preview, and cloud Details coverage.
- [x] Run the public-RHI cloud lifecycle/recovery integration target in inline
  and threaded executor lanes.
- [x] Run the authored-scene offscreen/Present/resize integration target in
  inline and threaded executor lanes.

#### Acceptance Gate

- Every selected test passes sequentially; compute/fragment parity, history
  commit/abort, failure/retry, debug modes, selected-light shadowing, offscreen,
  Present, resize, import/cook, editor presentation, and release remain covered.

### Stage 2: Confirm production budgets and complete the product gate

- [x] Reconcile the accepted P3/P4 4K image, median/p95, retained-byte, work,
  recovery, and release evidence against every shipped tier and both executors.
- [x] Run the affected aggregate lane required by the final handoff.
- [x] Run a low-concurrency full `all` build and the verified Editor smoke path.

#### Acceptance Gate

- No accepted numeric gate is missing or weakened, the aggregate and full build
  pass, the Editor passes bounded startup smoke, and lifecycle coverage passes
  orderly application/resource shutdown from the verified profile.

### Stage 3: Publish and close

- [x] Repair active documentation links and confirm Runtime/Editor contracts
  collectively own every lasting cloud behavior.
- [x] Record exact validation evidence, mark this plan completed, update P6 and
  completion criteria in the roadmap, and validate all documentation.
- [x] Prepare the completed plan and roadmap for the 2026-08 lifecycle archive
  transactions and reference repair.

#### Acceptance Gate

- No required checklist is open, the plan and roadmap are complete and ready
  for lifecycle archival with valid references.

## Validation Matrix

| Boundary | Selection | Required result |
| --- | --- | --- |
| Scene/parameters/eligibility | `VolumetricCloudSceneContractTests` | Stable identity, selection, immutable publication, mutation, diagnostics, serialization, and release pass. |
| Outer-view transactions/statistics | Registered Renderer scene contract target | Cloud history commit/abort/invalidation and copied bounded statistics pass. |
| Import/reimport/cook | `TextureTests` focused volume-source import/cook case | Direct PNG atlas lifecycle and cook-without-authoring dependency pass. |
| Exact preview and Details | `TextureThumbnailTests` and `ViewportTests` focused cloud cases | R8/RGBA8 slices, registration, cloud grouping, and read-only diagnostics pass. |
| Renderer lifecycle | `VolumetricCloudVulkanTests`, inline and threaded | Compute/fragment, debug, temporal, shadow, failure/retry, invalidation, and release pass. |
| Authored render routes | `VolumetricCloudSceneVulkanTests`, inline and threaded | Disabled/invalid identity, authored compute/fragment output, offscreen/Present, resize, and shutdown pass. |
| Performance and memory | Accepted P3/P4 RTX 3090 / Vulkan 1.4.325 evidence | All frozen image, median/p95, work, and 224 MiB gates remain documented; no shared-machine rebaseline. |
| Aggregate/build/smoke | Affected aggregate, full `all` build, Editor launch | All pass sequentially from `Win64-Debug-DurinEditor`. |
| Documentation | Changed, plan, and roadmap validators | No active-document error; archive transaction repairs direct references. |

## Definition of Done

- The frozen correctness matrix, aggregate, full build, and Editor smoke pass.
- Accepted quiet-lane performance/image/memory evidence covers every shipped
  tier and both executors without a new contested-machine baseline.
- Lasting behavior remains published under the owning Runtime and Editor docs.
- P6 is complete, the volumetric-cloud roadmap is archived, and this plan is in
  the shared completed-plan batch queue with no open required milestone or
  broken active reference.

## Deferred Follow-ups

- Fog, local volumes, multiple layers, atmosphere integration, broader import,
  asynchronous compute, and Render Graph remain separate evidence-triggered
  programs rather than hidden cloud-roadmap work.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/Archive/2026-08/VolumetricCloudRendering.md)
- [Volumetric cloud spatial rendering](../../../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Volumetric cloud scene contract](../../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Volumetric cloud temporal reconstruction](../../../Runtime/Rendering/VolumetricCloudTemporalReconstruction.md)
- [Volumetric cloud lighting and shadows](../../../Runtime/Rendering/VolumetricCloudLightingAndShadows.md)
- [Volumetric cloud authoring architecture](../../../Editor/Architecture/VolumetricCloudAuthoring.md)
- [Volumetric cloud authoring guide](../../../Editor/Guides/VolumetricCloudAuthoring.md)
- [Volume textures](../../../Runtime/Assets/VolumeTextures.md)
- [Build and run workflow](../../../Agents/BuildAndRun.md)
- [Testing workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/VolumetricCloudComponent.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneViewState.h`
- `Engine/Source/Runtime/RenderCore/Public/VolumetricCloudView.h`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/VolumetricCloudDetails.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MVolumeTextureEditor.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
