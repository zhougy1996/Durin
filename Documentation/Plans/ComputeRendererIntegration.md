# Compute Renderer Integration Plan

Summary: Integrate synchronous compute into the Renderer through FXAA resolved into a linear storage intermediate, followed by the existing graphics output path, with exact fallback, lifecycle safety, parity, and measured rollout evidence.

Last reviewed: 2026-08-12

Status: Active
Completed:

## Current Status

M1 resource transitions and M2 synchronous compute are complete. Public RHI can
create, bind, dispatch, synchronize, and retain compute work through both
command executors, including compute-to-graphics handoff. Renderer still owns
no production compute workload.

This plan selects FXAA as the first Renderer consumer. The existing fragment
FXAA remains the reference and fallback. The compute path reads Scene Color,
writes a size-keyed linear floating-point storage intermediate, and hands that
result to the existing graphics copy/output pass before optional editor
assistance. This avoids relying on storage support for the current sRGB final
output or swapchain formats while proving a real compute-to-graphics consumer.

The implementation has not started. Stage 0 must freeze image equivalence,
dispatch extent, eligibility, instrumentation, and fallback semantics before
the Renderer resource graph changes.

## Goal

Ship one bounded Renderer-owned workload that consumes the public synchronous
compute contract without Vulkan escape hatches. Eligible FXAA uses a
reflected compute shader and exact resource transitions, preserves the existing
visual result and editor-assistance ordering, survives resize/reload/failure
lifecycle events, and falls back transactionally to the established fragment
path. It also collapses output-layout-specific FXAA pipeline variants into one
compute PSO. Default rollout requires recorded workload evidence rather than
milestone pressure alone.

## Scope

- A compute FXAA shader whose sampling algorithm and constants match the
  existing fragment FXAA reference.
- Renderer-owned compute shader map, canonical compute PSO, typed parameters,
  resource slot, retry diagnostics, and release behavior.
- A bounded size-keyed `RGBA16_FLOAT` storage-and-sampled intermediate and an
  explicit eligibility decision owned above RHI/Vulkan.
- Scene-color-to-compute, compute-to-graphics-copy, and existing
  graphics-output-to-editor-assistance/Present/UI-sampling transitions.
- Exact fallback to fragment FXAA for disabled FXAA, unsupported or
  incompatible intermediates/extents, and recoverable compute-resource failure.
- Main and auxiliary offscreen viewport resize, shader reload, manual retry,
  device invalidation, recorded lifetime, output parity, and runtime coverage.
- GPU timestamp and command-counter evidence sufficient to decide whether the
  eligible compute route becomes the normal FXAA policy.
- Lasting Renderer compute-usage documentation and M3 roadmap completion
  evidence.

## Non-Goals

- Storage-capable sRGB final targets or swapchains, direct compute-to-Present,
  mutable-format image views, swapchain capability expansion, or changes to
  final-output texture formats.
- Replacing the FXAA algorithm, changing its quality constants, adding temporal
  anti-aliasing, exposure, tone mapping, bloom, or a general post-process graph.
- A render graph, automatic barrier synthesis, transient-resource allocator,
  or generic pass scheduler.
- Asynchronous compute, a second GPU queue, queue-family ownership transfer, or
  overlap scheduling.
- Indirect dispatch, GPU culling, compute skinning, mip generation, or other M4
  and M5 candidates.
- Making all Renderer targets storage-capable or generalizing feature-resource
  ownership beyond the selected workload.
- Treating a synthetic dispatch, raw Vulkan test, or device-idle readback as
  Renderer integration evidence.

## Design Decisions and Invariants

### Selected workload and output boundary

- The selected consumer is FXAA for scene views. It is already a real per-view
  feature, has an operator-visible toggle, runs in window-backed, main editor,
  and auxiliary editor views, and has an exact fragment implementation for
  comparison and fallback.
- Compute FXAA is eligible only when FXAA is enabled, the complete compute
  payload and size-keyed intermediate are ready, and its dimensions admit the
  direct-dispatch limits. Eligibility is one pure Renderer decision with a
  testable reason; backend type and Vulkan handles are not inputs.
- Present and offscreen outputs both remain ordinary graphics attachments. The
  compute result is sampled by the existing copy fragment path, so M3 does not
  require storage-capable sRGB images or swapchains. FXAA disabled continues to
  copy Scene Color directly.
- The compute shader uses an `8 x 8 x 1` local size. Dispatch dimensions are
  `ceil(Width / 8) x ceil(Height / 8) x 1`; the shader bounds-checks every
  invocation against the exact target extent.
- The compute result is a production intermediate consumed immediately by the
  existing graphics copy pass, not a test-only or discarded dispatch. The copy
  pass writes the actual viewport output; optional editor assistance then loads
  it, Present presents it, or Mona samples it through the existing path.

### Visual and color contract

- Compute and fragment FXAA share the same luma weights, direction reduction,
  clamp, sample positions, alpha preservation, linear-clamp sampling, and
  texel-size source. Common shader helpers are preferred over duplicated
  algorithm bodies.
- The established `SRGBA8_UNORM` Scene Color and final-output formats remain
  unchanged. Compute writes linear FXAA color to an `RGBA16_FLOAT`
  storage-and-sampled intermediate; the existing graphics copy pass owns the
  final sRGB attachment conversion. Stage 0 must freeze this conversion and
  parity tolerance rather than assuming sRGB storage-image support.
- Automated parity uses deterministic scene-color fixtures and compares exact
  dimensions, alpha, border pixels, flat regions, high-contrast diagonal edges,
  and odd extents. Any unavoidable format-rounding tolerance is recorded in
  Stage 0 and bounded per channel; an unbounded visual comparison is not
  acceptance evidence.
- Editor grid, gizmos, overlay lines, and icons remain after FXAA and therefore
  stay crisp. Compute integration cannot move assistance into Scene Color or
  into the anti-aliasing input.

### Resource state, recording, and lifetime

- Scene Color leaves the scene render pass as `GraphicsShaderRead` and is
  explicitly transitioned to `ComputeShaderRead` before dispatch. The linear
  intermediate is fully overwritten, so its prior contents enter compute
  through a discard-to-`ComputeShaderReadWrite` transition rather than a
  guessed previous-frame state.
- After dispatch, the intermediate transitions from
  `ComputeShaderReadWrite` to `GraphicsShaderRead`. The existing output render
  pass samples it and retains its current final-access behavior: Present or
  `GraphicsShaderRead` directly, or `ColorAttachmentReadWrite` before the
  editor-assistance load pass.
- The command list switches to compute only outside render passes, records PSO,
  reflected parameters, and dispatch, then switches back to the pipeline needed
  by the next operation. No device-idle call, raw barrier, or submission split
  supplies correctness.
- Recorded commands retain the compute PSO, shader, sampler, Scene Color,
  intermediate texture, output texture, and views through replay. Resizing may
  publish a new size-keyed intermediate/output while submitted batches continue
  to own old resources.

### Ownership, refresh, failure, and fallback

- `FPostProcessRenderer` owns the compute shader map and PSO through its existing
  render-thread resource lifecycle. A candidate publishes only when the compute
  shader, reflected layout, sampler dependency, and compute PSO are complete.
- Graphics copy/FXAA resources remain independently usable. Compute compilation
  or PSO failure degrades only the compute route; it must not suppress the
  complete fragment fallback or leave the viewport blank.
- Shader and manual refresh may retain the last-known-good compute payload while
  its device generation remains current. Device invalidation releases it before
  retry. Same-generation failures are suppressed and diagnosed through the
  existing `FRendererResourceCoordinator` contract.
- Size-keyed intermediate creation uses the existing Renderer slot/cache
  semantics. A failed candidate is not published or retried every frame; the
  fragment FXAA path remains available for that view while a later generation
  permits recovery.
- No Renderer code branches on Vulkan. If `RHIIsTextureSupported` rejects the
  exact portable `RGBA16_FLOAT` sampled/storage description or creation returns
  null, eligibility reports a fallback reason and selects fragment FXAA.

### Measurement and rollout

- CI correctness gates do not assert wall-clock or GPU-time thresholds.
  Performance qualification uses public RHI GPU timing queries on a named
  adapter, after warm-up, with fixed scene content and offscreen extents.
- Record fragment and compute FXAA samples separately for at least 1920x1080 and
  one odd extent; report adapter, driver, build profile, sample count, median,
  p95, and whether editor assistance was present. Unsupported timestamps are an
  explicit missing-evidence result, not a zero-duration success.
- Deterministic counters record the structural difference: fragment FXAA uses
  one output-layout-specific FXAA draw; compute FXAA uses one output-independent
  dispatch plus the already-required copy-pipeline family. Resource evidence
  also proves that three FXAA graphics PSO variants can be removed in favor of
  one compute PSO. These counts are architectural evidence, not a speedup claim.
- Compute becomes the normal eligible FXAA policy only when output parity
  and lifecycle gates pass and recorded GPU timing establishes an acceptable
  cost on the target adapter. The rollout decision and criterion are written in
  this plan before Stage 4 closes. If evidence is adverse or unavailable, M3
  remains active or the selected workload is revised; the milestone is not
  completed merely because a forced test path exists.

## Current Foundations and Gaps

| Area | Existing foundation | M3 gap |
| --- | --- | --- |
| Compute RHI | Complete-or-null compute PSOs, reflected parameters, direct dispatch, exact transitions, both executors, and compute-to-graphics validation are complete. | No Renderer feature owns or refreshes a compute payload. |
| FXAA | `FPostProcessRenderer` owns fragment copy/FXAA shaders, six output/layout PSOs, a linear-clamp sampler, size-keyed scene targets, and a per-view FXAA switch. | No compute shader, linear storage intermediate, dispatch route, or parity fixture exists. |
| Final output | Window/offscreen outputs remain sRGB graphics attachments; offscreen targets are registered with Mona. | They are intentionally unsuitable as the portable storage target; compute needs a Renderer-owned linear intermediate and existing graphics copy. |
| Ordering | Scene Color ends shader-readable; post-process writes output; optional editor assistance loads output and depth; Present/Mona consumes final color. | Compute and its sampled result must enter before the existing output pass without changing the assistance/final boundary. |
| Recovery | Renderer creation slots, generation-scoped retry, last-known-good retention, diagnostics, device invalidation, and bounded size caches are established. | Compute failure must degrade independently to fragment FXAA and resize must retain recorded-resource lifetime. |
| Measurement | RHI publishes GPU timestamp support and recorded begin/end timing queries; Renderer already emits per-view counters. | No FXAA route timing fixture, route counter, or documented rollout decision exists. |

## Implementation Stages

### Stage 0: Freeze equivalence, eligibility, and measurement contracts

- [ ] Factor or specify one shader-level FXAA algorithm shared by fragment and
  compute entry points, including the exact linear `RGBA16_FLOAT` intermediate
  and final sRGB conversion behavior plus bounded per-channel parity tolerance.
- [ ] Add a pure route-decision contract covering FXAA disabled, Present,
  offscreen storage eligibility, missing compute payload, zero/odd/over-limit
  extents, and editor-assistance presence.
- [ ] Inventory `FPostProcessRenderer` size-keyed target/slot dependencies,
  render-pass layouts, output pipeline variants, command counters, GPU timing
  query ownership, and relevant Engine/Vulkan fixtures.
- [ ] Establish deterministic input images and a reference fragment result for
  flat color, alpha, borders, diagonal edges, high contrast, and odd extents.
- [ ] Record the benchmark protocol, target adapter, sample count, warm-up,
  queried interval boundaries, and rollout criterion before performance results
  are observed.

#### Acceptance Gate

- The chosen workload, format semantics, route table, state sequence, parity
  tolerance, failure policy, and measurement protocol are unambiguous; no
  unresolved swapchain, async-queue, or general post-process-graph decision can
  leak into implementation.

### Stage 1: Build the transactional compute FXAA resource path

- [ ] Add the compute FXAA entry point with `8 x 8 x 1` local size, reflected
  sampled Scene Color, sampler, view uniform, storage output, and exact bounds
  check while retaining the fragment reference entry point.
- [ ] Add typed Renderer compute parameters and construct the canonical compute
  PSO exclusively from the reflected compute shader layout.
- [ ] Split or compose `FPostProcessRenderer` resource payloads so compute
  failure cannot invalidate complete graphics copy/FXAA fallback resources.
- [ ] Integrate compute payload creation, last-known-good shader/manual refresh,
  device invalidation, same-generation suppression, retry diagnostics, and
  shutdown release with `FRendererResourceCoordinator`.
- [ ] Add focused shader/layout, complete-or-null creation, failure isolation,
  retry, replacement, and release tests.

#### Acceptance Gate

- Renderer can own one complete compute FXAA payload and refresh or discard it
  transactionally; every injected compute failure leaves the established
  graphics FXAA route usable and diagnosable.

### Stage 2: Dispatch into a real graphics-consumed intermediate

- [ ] Add a size-keyed `RGBA16_FLOAT` sampled/storage texture to the
  post-process target cache through portable support checks and transactional
  creation; do not change final offscreen or swapchain texture formats.
- [ ] Implement the pure eligibility decision and expose a bounded route reason
  to tests/diagnostics without backend-specific inspection.
- [ ] Branch `FSceneRenderer` after the Scene Color pass: select fragment
  fallback or transition Scene Color/intermediate, switch to compute, bind the
  typed payload, and dispatch exact ceil-divided groups.
- [ ] Transition the compute result to `GraphicsShaderRead`, run the existing
  graphics output pass with its copy shader against that result, and preserve
  Present/Offscreen final access, editor-assistance load/depth, and constrained
  viewport/scissor behavior.
- [ ] Prove window-backed, main editor, and auxiliary views, alternating sizes,
  odd extents, FXAA toggle, assistance present/absent, and fragment fallback
  record the exact route and transition order through both command executors.

#### Acceptance Gate

- An eligible production view displays the graphics-copied compute FXAA result,
  optional editor assistance consumes the final output, all other route-table
  cases use the exact fragment fallback, and no Vulkan symbol or whole-device
  idle enters Renderer/Engine code.

### Stage 3: Prove pixels, refresh, and lifetime end to end

- [ ] Add Vulkan-backed Renderer fixtures that render the deterministic image
  matrix through fragment and compute FXAA and compare every required region at
  the frozen tolerance.
- [ ] Validate Present and offscreen outputs retain their existing final states,
  Mona sees the registered display texture, and editor assistance remains after
  FXAA with preserved load/depth semantics.
- [ ] Exercise resize while recorded work is pending, main/auxiliary extent
  alternation, output replacement failure, compute shader compile/PSO failure,
  shader reload, manual retry, device invalidation, and shutdown.
- [ ] Verify failed compute/intermediate refresh retains a same-device
  last-known-good payload when allowed, otherwise selects fragment fallback
  without a blank frame; later successful refresh becomes visible in process.
- [ ] Validate counters and resource statistics distinguish the compute-plus-copy
  route from one fragment FXAA draw, confirm obsolete FXAA graphics variants are
  absent, and prove repeated frames reach stable PSO/descriptor cache behavior
  without stale views or pending-delete failures.

#### Acceptance Gate

- Pixel parity, phase ordering, compute-to-graphics consumer handoff, fallback,
  refresh, replacement, recorded lifetime, repeated-frame cache use, and
  shutdown pass through public RHI on real Vulkan with validation clean.

### Stage 4: Measure, qualify, and publish M3

- [ ] Run focused `RenderCoreTests`, `EngineTests` owners, and
  `VulkanRHIIntegrationTests` coverage through the root
  [build and run](../Development/Build/BuildAndRun.md) workflow in dedicated-RHI
  and inline executor modes where the compute route is exercised.
- [ ] Capture the frozen GPU timing matrix and structural command counters,
  record raw sample artifacts outside source control when appropriate, and add
  the evidence summary plus rollout decision to `Current Status`.
- [ ] Because the change is user-visible in editor viewports and crosses Engine,
  Renderer, RenderCore, RHI, VulkanRHI, and multiple native targets, run the
  native aggregate at default target granularity and a full `all` build.
- [ ] Run a validation-enabled Debug Editor session covering main and camera
  preview offscreen views, resize, FXAA on/off, assistance overlays, shader
  reload/retry, several stable frames, and orderly shutdown; separately cover
  the window-backed Present output and its unchanged swapchain lifecycle.
- [ ] Publish the stable workload, eligibility, fallback, synchronization,
  refresh, and measurement contract under `Documentation/Runtime/Rendering/`
  and update viewport-rendering ownership/order text.
- [ ] Mark M3 and the required roadmap complete only if the rollout evidence
  admits the compute policy; explicitly review and defer M4/M5 when their entry
  evidence remains absent.

#### Acceptance Gate

- Focused tests in both relevant executor modes, the native aggregate, full
  build, validation-enabled runtime matrix, pixel/lifecycle gates, documentation
  validation, and the predeclared measurement gate pass; the selected Renderer
  route uses compute by policy rather than only through a test override.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Route selection | Eligible FXAA selects compute; disabled FXAA, incompatible intermediate, missing payload, and invalid extent select exact fragment/copy fallback | Pure Renderer tests |
| Shader/layout identity | Compute entry point reflects sampled input, sampler, uniform, and storage output into one complete canonical PSO | RenderCore and Renderer resource tests |
| Dispatch dimensions | Even, odd, minimum, and limit-adjacent extents produce exact ceil-divided nonzero groups and bounded writes | Renderer contract and Vulkan integration tests |
| Scene Color -> compute | Scene output becomes compute-readable only through an explicit public transition | Recorded command and Vulkan validation evidence |
| Compute -> graphics copy | Linear compute result becomes graphics-readable and the copy pass writes the existing sRGB Present/Offscreen attachment | Renderer layout tests and Vulkan image evidence |
| Output consumers | Assistance loads final copied color; Present and Mona retain their existing final-access/registration behavior | Engine viewport and runtime evidence |
| Pixel equivalence | Fragment and compute paths match the deterministic image matrix within the frozen channel tolerance | Vulkan-backed Renderer fixture |
| Resize and auxiliary views | Main/preview alternating and odd extents reuse bounded resources; old recorded targets survive replacement | Engine/Vulkan lifetime fixture |
| Recoverable failure | Compile, PSO, target replacement, and refresh failures retain valid state or choose fragment fallback; later retry is visible | Failure injection and reload tests |
| Present path | Window-backed FXAA may consume the compute intermediate through the ordinary copy render pass; swapchain creation/presentation remains unchanged | Runtime smoke and route test |
| Measurement | Named-adapter median/p95 samples and command counters satisfy the predeclared rollout criterion | Stage 4 qualification record |
| Shutdown | Compute payloads, output views, descriptors, and recorded references retire without validation or pending-delete failures | Vulkan teardown test and runtime smoke |

## Definition of Done

- A real scene view uses Renderer-owned compute FXAA output through public RHI,
  the existing graphics copy pass consumes it, and editor assistance,
  presentation, or Mona consumes the final output without a Vulkan escape hatch.
- Pixel behavior, post-process ordering, offscreen registration, main/auxiliary
  views, odd extents, and FXAA toggling retain their established contracts.
- Every ineligible or failed compute case uses the complete fragment fallback
  without blank output or repeated same-generation failure; Present and
  offscreen final-target contracts remain unchanged.
- Resize, shader reload, manual retry, device invalidation, recorded-command
  lifetime, repeated frames, and shutdown are completion-safe.
- The predeclared GPU measurement gate supports normal eligible rollout; a
  forced test-only path is insufficient for completion.
- Focused, aggregate, full-build, runtime, and documentation qualification pass;
  stable behavior is documented; M3 and the required roadmap are complete.

## Deferred Follow-ups

- Storage-capable sRGB final-target/swapchain admission, mutable-format views,
  or direct compute-to-Present output.
- HDR scene color, tone mapping, exposure, bloom, temporal anti-aliasing, and a
  general post-process graph.
- Indirect dispatch and GPU-generated argument buffers (conditional M4).
- Separate compute queues, ownership transfer, cross-queue synchronization, and
  overlap scheduling (evidence-gated M5).
- Other renderer consumers such as culling, skinning, mip generation, or IBL
  processing; each requires its own workload evidence.
- Persistent per-feature GPU performance history or automatic runtime
  benchmarking.

## Related Documentation

- [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [Synchronous Compute Pipelines](../Runtime/Rendering/SynchronousComputePipelines.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Shaders/Slang/PostProcess.slang`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RHI/Public/RHICapabilities.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
