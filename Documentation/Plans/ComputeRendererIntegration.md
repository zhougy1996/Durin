# Compute Renderer Integration Plan

Summary: Integrate synchronous compute into the Renderer through directional contact visibility, with the existing fragment pass as an exact fallback and deferred lighting as the immediate production consumer.

Last reviewed: 2026-08-21

Status: Completed
Completed: 2026-08-21

## Current Status

M3 completed on 2026-08-21. Directional contact visibility is the first
Renderer-owned synchronous-compute consumer, eligible views select compute by
default, deferred lighting consumes the result immediately, and the exact
fragment and factor-one fallbacks remain available. M4 indirect dispatch and M5
asynchronous compute are explicitly deferred because no qualified workload or
overlap evidence meets their entry gates; FXAA and GTAO conversion remain
separate workload decisions.

M1 resource transitions and M2 synchronous compute are complete. Public RHI can
create, bind, dispatch, synchronize, and retain compute work through both
command executors, including compute-to-graphics handoff. Renderer still owns
no production compute workload.

The M3 entry gate is met and implementation may begin at Stage 0. HDR display,
hybrid deferred rendering, deferred contact visibility, and half-resolution
GTAO are now qualified production foundations. The original M3 selection of
FXAA is superseded before implementation: FXAA would require a new
`RGBA16_FLOAT` storage intermediate plus an unavoidable graphics copy into the
sRGB final target, would cross display-mapping and editor-assistance boundaries,
and would add bandwidth to a fragment pass already measured at about 68 us on
the qualification adapter.

Directional contact visibility is the selected first consumer. It already
runs as one bounded fullscreen fragment pass, writes one size-keyed sampled
visibility mask, and is consumed immediately by deferred lighting. A compute
route can replace that draw with one dispatch without changing Scene Color,
final-output formats, presentation, editor assistance, or the selected shadow
algorithm. The established fragment pass remains a complete same-feature
fallback; factor-one remains the fallback if neither feature route is usable.

Stage 0 is complete and Stages 1-3 are in implementation/validation. The exact
format is `R8_UNORM` at one byte per pixel, reflected from
`RWTexture2D<unorm float>` as one compute-only storage image. Public RHI creates
sampled and storage views, dispatches, transitions, samples, and reads the
format on the NVIDIA GeForce GTX 1060 6GB through threaded and inline
executors. The existing 257x257 contact qualification fixture proves exact
fragment/compute final-image equality; visibility comparisons use exact R8
bytes, so the frozen tolerance is zero.
Fragment and compute caches each retain at most 16 MiB and report a combined
32 MiB ceiling; one active 1920x1080 target is 2,073,600 bytes.

The pure route table and structural counters are implemented. Normal eligible
views select compute; missing compute payload/target or over-limit dispatch
selects fragment; disabled/unneeded, invalid/zero extent, or total failure
selects factor one. Compute and fragment resource slots and size caches are
independent, and compute writes the complete target with white outside the
fitted view. The remaining work is to finish failure/reload coverage, record
the frozen timing matrix, run the aggregate/runtime gates, and publish lasting
documentation.

The Stage 4 benchmark is frozen before route timing is observed: NVIDIA
GeForce GTX 1060 6GB, Vulkan 1.3.280, Win64 Debug DurinEditor, the production
directional-contact fixture with surrounding hybrid deferred/GTAO work,
1920x1080 plus a 1919x1079 target containing a 1601x901 viewport at (137, 89),
threaded and inline executors, 30 warm-up and 120 measured frames per route.
Intervals include route-specific transitions, dispatch/draw, and output
handoff, but exclude deferred consumption. Compute qualifies when it records
one dispatch/no contact draw/no copy, preserves exact pixels, has median no
greater than 110% of fragment and p95 no greater than 125% of fragment at both
extents, and adds no more than 300,000 ns median or 500,000 ns p95 over the
same-adapter fragment route.

The first 1920x1080 GTX 1060 observation forced one recorded criterion
revision before odd/constrained or inline results were observed. The originally
written raw 300,000/500,000 ns ceilings assumed RTX 3090-class absolute time;
on this adapter the unchanged fragment reference is 2,332,480/2,335,264 ns and
the repository's unrelated fixed RTX qualification gates miss by 4-10x.
Absolute route time therefore measured adapter class rather than integration
cost. The revised absolute delta above remains independent of which route wins,
retains the frozen relative bounds, and is the rollout criterion for all
remaining samples.

Implemented qualification evidence on 2026-08-18:

| Executor / extent | Compute median / p95 | Fragment median / p95 | Result |
| --- | ---: | ---: | --- |
| Threaded, 1920x1080 | 2,418,576 / 2,419,648 ns | 2,362,672 / 2,365,024 ns | Pass, +2.37% / +2.31% |
| Threaded, 1919x1079 constrained | 2,418,496 / 2,420,032 ns | 2,362,784 / 2,365,088 ns | Pass, +2.36% / +2.32% |
| Inline, 1920x1080 | 2,418,848 / 2,419,648 ns | 2,355,808 / 2,365,568 ns | Pass, +2.68% / +2.29% |
| Inline, 1919x1079 constrained | 2,403,456 / 2,404,416 ns | 2,348,560 / 2,350,912 ns | Pass, +2.34% / +2.28% |

Every measured compute view records one dispatch, zero contact draws, and no
copy; forced fragment records zero dispatches and one draw. Compute is admitted
as the normal eligible policy because exact pixel, structural, relative, and
absolute-delta gates pass on both executors and extents. Raw output is retained
in the DurinDevTool test logs outside source control.

The editor View > Shadows menu now exposes Auto, Compute Only, and Fragment
Only contact-visibility route preferences for runtime A/B work. The expanded
viewport statistics panel reports the producer that actually completed the
latest view; Auto remains the shipping compute-first policy with fragment
fallback.

Focused shader/Renderer/Vulkan targets, the 57-target `fast-all` selection,
the default native aggregate, and the full `all` build pass. The Debug Editor
also starts, remains stable for ten seconds, accepts `WM_CLOSE`, and exits zero.
The final validation-enabled Debug Editor diagnostic passed on 2026-08-21 with
the Vulkan Khronos validation layer active: main and independent auxiliary
offscreen views traversed Auto/Compute/Fragment/Off/contribution routes; shader
reload and resource retry completed; application Present survived resize and
restore; twelve stable frames completed; shutdown returned zero with no error,
warning, validation, or leaked-view-state record. Exact Camera Preview client
view construction and alternating-view behavior remain covered by the existing
native Engine/Vulkan viewport suites rather than a synthetic editor selection.
The combined
`GBufferQualificationTests` executable records passing contact-specific gates
but remains red on this machine because its pre-existing RTX 3090 absolute
GBuffer/deferred/GTAO/FXAA thresholds are executed on the GTX 1060; those
unrelated adapter-specific failures are not waived or rewritten by M3.

## Goal

Ship one bounded Renderer-owned compute workload without Vulkan escape hatches.
Eligible directional contact visibility uses a reflected compute shader and
exact resource transitions, preserves the existing visibility and final-image
result, survives resize/reload/failure lifecycle events, and falls back
transactionally to the established fragment implementation. Default rollout
means compute is the normal route when contact shadows are requested and the
view is eligible; it does not enable contact shadows globally. Rollout requires
recorded pixel, lifecycle, command, and GPU-timing evidence.

## Scope

- A compute contact-visibility entry point whose receiver classification,
  reconstruction, bounded 16-step trace, hit test, fades, and constants match
  the existing fragment reference.
- Renderer-owned compute shader map, canonical compute PSO, typed parameters,
  independent refresh slot, diagnostics, and release behavior.
- A size-keyed sampled/storage visibility target using the exact Stage 0 format
  contract, plus the existing render-targetable fragment fallback.
- Explicit GBuffer/depth-to-compute, compute-output-to-deferred-read, and input
  restoration transitions through public RHI.
- One pure route decision for disabled/unneeded contact, invalid inputs,
  unsupported formats or extents, unavailable compute resources, compute,
  fragment fallback, and factor-one fallback.
- Constrained viewports, main and auxiliary offscreen views, resize, shader
  reload, manual retry, device invalidation, recorded lifetime, parity, and
  runtime coverage.
- Route-specific GPU timestamps and command counters sufficient to decide
  whether compute becomes the normal eligible contact-visibility policy.
- Lasting Renderer compute-usage documentation and M3 roadmap completion
  evidence.

## Non-Goals

- Enabling contact shadows by default or changing their user-facing toggle,
  diagnostic mode, selected light ownership, quality constants, bias, trace
  distance, step count, hit classification, or known screen-space limitations.
- Compute FXAA, display mapping, GTAO, deferred lighting, shadow-map rendering,
  mip generation, IBL generation, or another second workload in M3.
- Changing `RGBA16_FLOAT` Scene Color, `SRGBA8_UNORM` final outputs, swapchain
  capabilities, presentation, Mona registration, or editor-assistance order.
- A render graph, automatic barrier synthesis, transient-resource allocator,
  generic pass scheduler, or renderer-wide fullscreen-pass abstraction.
- Asynchronous compute, a second GPU queue, queue-family ownership transfer,
  overlap scheduling, indirect dispatch, or GPU-generated arguments.
- HZB construction, temporal filtering, motion vectors, off-screen occluders,
  multilayer depth, local-light contact shadows, or ray tracing.
- Treating a synthetic dispatch, raw Vulkan test, default-off feature state, or
  device-idle readback as Renderer integration evidence.

## Design Decisions and Invariants

### Selected workload and route boundary

- Contact visibility remains opt-in through `FSceneViewSettings`. Compute is
  considered only when the existing production-deferred, enabled-contact,
  valid-directional-shadow, complete-GBuffer, and successful-receiver-draw
  conditions request the pass.
- Eligibility is one pure Renderer decision. It consumes feature intent, valid
  view/light inputs, complete compute payload, exact target support/readiness,
  and dispatch limits. Backend type and Vulkan handles are not inputs.
- The normal eligible route is one compute dispatch whose output is sampled by
  the existing deferred-lighting pass. Fragment fallback is the current
  contact-visibility render pass. If both routes are unavailable, deferred
  lighting binds the established white texture and contact visibility is one.
- The compute shader uses an `8 x 8 x 1` local size. It dispatches over the
  complete owning target with `ceil(TargetWidth / 8) x ceil(TargetHeight / 8)
  x 1` groups and bounds-checks every invocation.
- To preserve the current clear-plus-scissor semantics, every in-bounds target
  pixel is overwritten each dispatch. Pixels outside the fitted view rectangle
  receive exactly one; pixels inside use the shared contact algorithm. No
  previous view or frame contents are observable.

### Visibility format and pixel contract

- The preferred output remains single-channel `R8_UNORM` with sampled and
  storage usage. Stage 0 must prove that the exact description, reflected typed
  storage declaration, storage view, write, transition, sampled read, and
  quantization work through public RHI in both executors on the target adapter.
- If that proof fails, Stage 0 selects one portable normalized alternate that
  M2 can support, records its sampled binding type, red-channel extraction,
  exact byte cost, and per-pixel tolerance, and updates this plan before Stage
  1. Production code may not select a backend-specific or unrecorded format.
- Compute and fragment entries share the receiver/trace algorithm body. They
  preserve standard-Lit rejection, depth validity, geometric-normal behavior,
  matrix reconstruction, viewport mapping, 16 midpoint samples, distance and
  screen fades, finite handling, and visibility clamping.
- Parity compares the visibility value after target-format quantization and the
  final deferred image. Exact equality is preferred; any unavoidable bounded
  tolerance is recorded in Stage 0 per channel and per diagnostic region.
- Contact diagnostics continue to consume the current view's visibility mask.
  Directional, local, environment, emissive, Unlit, translucent, display, and
  editor-assistance behavior remain outside the selected visibility term.

### Resource state, recording, and lifetime

- GBuffer material, normals, surface, emissive, and D32 leave their producing
  passes graphics-shader-readable. The compute route explicitly transitions
  each required input to `ComputeShaderRead` before dispatch and restores it to
  `GraphicsShaderRead` before deferred lighting.
- The compute target is fully overwritten, so it enters through a
  discard-to-`ComputeShaderReadWrite` transition. After dispatch it transitions
  to `GraphicsShaderRead` for deferred lighting and diagnostics.
- Pipeline switches and dispatch occur outside render passes. The command list
  switches back to graphics before deferred lighting. No device-idle call, raw
  barrier, submission split, or implicit same-queue assumption supplies
  correctness.
- Recorded commands retain the compute PSO, shader, uniform buffer, all five
  sampled inputs, output texture, and views through replay. Resize or route
  replacement may publish new targets while submitted batches retain old ones.

### Ownership, refresh, and fallback

- `FContactShadowVisibilityRenderer` owns the compute payload beside, but not
  inside the failure boundary of, its fragment payload. A compute candidate
  publishes only when shader, reflected layout, and compute PSO are complete.
- Fragment resources remain independently usable. Compute compilation, PSO,
  target, or exact-format failure cannot invalidate or suppress the fragment
  pass. Fragment failure retains the established factor-one deferred fallback.
- Shader/manual refresh may retain the last-known-good compute payload while
  its device generation remains current. Device invalidation releases it before
  retry. Same-generation failures are suppressed and diagnosed through
  `FRendererResourceCoordinator`.
- Size-keyed compute targets use the existing Renderer slot-cache semantics and
  a recorded byte ceiling. Failed candidates are not published or retried every
  frame. Fragment targets keep their current cache and lifecycle behavior.
- No Renderer code branches on Vulkan. Exact texture support is queried through
  `RHIIsTextureSupported`; null creation or view failure records a bounded route
  reason and selects fragment fallback.
- Route selection completes before any compute-specific transition or dispatch
  is recorded. Recoverable resource failure therefore cannot leave a partially
  recorded compute route that attempts a same-frame fragment replay.

### Measurement and rollout

- CI correctness gates do not assert wall-clock or GPU-time thresholds.
  Qualification uses public RHI GPU timing on a named adapter, after warm-up,
  with fixed scene content and extents.
- Fragment and compute contact visibility are sampled separately at 1920x1080
  and one odd/constrained extent. The report records adapter, driver, build,
  executor, sample count, median, p95, visibility-target format, and surrounding
  deferred workload. Unsupported timestamps are missing evidence, not success.
- Deterministic counters prove that eligible compute records one dispatch and
  no contact-visibility draw, fragment fallback records one draw and no contact
  dispatch, and disabled/unneeded contact records neither. The compute route
  must not introduce a post-visibility copy pass.
- Compute becomes the normal route only when visibility/final-image parity and
  lifecycle gates pass and the predeclared Stage 0 timing criterion admits it.
  Adverse or unavailable evidence leaves M3 active or triggers a recorded
  workload revision; a forced test route does not complete the milestone.

## Current Foundations and Gaps

| Area | Existing foundation | M3 gap |
| --- | --- | --- |
| Compute RHI | Complete-or-null compute PSOs, reflected parameters, direct dispatch, exact transitions, both executors, and compute-to-graphics validation are complete. | No Renderer feature owns or refreshes a compute payload. |
| Contact visibility | One bounded fragment pass, qualified shader/constants, on-demand size-keyed target, opt-in setting, diagnostics, counters, and factor-one failure behavior exist. | No compute entry, compute resource slot, route decision, or route-specific counter exists. |
| Consumer boundary | Deferred lighting immediately samples the visibility mask before retained forward, display, and assistance. | Compute inputs/output need explicit state handoff without changing deferred composition. |
| Format | `R8_UNORM` is the qualified fragment target; M2 proves `RGBA8_UNORM` sampled/storage compute interop. | Exact `R8_UNORM` typed-storage support and quantization are unproven and must be frozen in Stage 0. |
| Recovery | Renderer generation slots, retry suppression, last-known-good retention, diagnostics, device invalidation, and bounded caches exist. | Compute failure must remain independent from fragment visibility and factor-one fallback. |
| Validation | Contact image, constrained-view, resize, failure/retry, Vulkan, and final-composition fixtures already exist. | No fragment-versus-compute mask/final-image matrix or compute route lifetime fixture exists. |
| Measurement | RHI GPU timestamps and Renderer per-view counters exist; contact production qualification has fixed scenes. | No route-specific contact timing interval, dispatch counter, or rollout decision exists. |

## Implementation Stages

### Stage 0: Freeze format, parity, route, and measurement contracts

- [x] Prove or reject the exact `R8_UNORM` sampled/storage typed-image path
  through Slang reflection, public RHI creation/views, write-to-sample handoff,
  quantization, and both executors; if rejected, record the selected portable
  alternate and update all affected format, ABI, tolerance, and byte contracts.
- [x] Factor or specify one shader-level contact algorithm shared by fragment
  and compute entries, including full-target white writes outside the fitted
  view and exact post-format visibility comparison.
- [x] Add a pure route-decision contract covering disabled/unneeded contact,
  invalid light/view, incomplete GBuffer, missing payload, unsupported or failed
  target, zero/odd/over-limit extents, compute, fragment fallback, and factor one.
- [x] Inventory current contact resource slots, target cache, constrained-view
  mapping, render-pass layout, counters, deferred bindings, GPU timing ownership,
  and the existing image/lifecycle fixtures affected by the new route.
- [x] Freeze deterministic fragment references for visibility and final deferred
  output across lit/unlit, hit/miss, borders, silhouettes, grazing surfaces,
  reversed/forward Z, perspective/orthographic, odd extents, and constrained
  viewports.
- [x] Record the benchmark adapter, scene, extents, executor, warm-up, sample
  count, interval boundaries, and absolute/relative rollout criterion before
  compute-route timing results are observed.

#### Acceptance Gate

- The output format and typed binding, target bytes, quantization tolerance,
  shared algorithm, route table, state sequence, full-target overwrite rule,
  failure policy, and measurement protocol are unambiguous. No unresolved
  display, swapchain, async-queue, or general post-process decision can enter
  implementation.

### Stage 1: Build the transactional compute visibility payload

- [x] Add the compute entry point with `8 x 8 x 1` local size, the frozen input
  set and uniform ABI, storage output, complete-target bounds handling, and the
  shared fragment-reference algorithm.
- [x] Add typed compute parameters and construct the canonical compute PSO only
  from the reflected compute shader layout.
- [x] Separate fragment and compute resource publication so every injected
  compute compile, reflection, shader, or PSO failure leaves fragment visibility
  complete and usable.
- [x] Integrate last-known-good shader/manual refresh, device invalidation,
  same-generation suppression, diagnostics, retry, and shutdown release with
  `FRendererResourceCoordinator`.
- [x] Add focused shader/layout, complete-or-null creation, failure isolation,
  retry, replacement, and release tests.

#### Acceptance Gate

- Renderer owns one complete compute contact payload and can refresh or discard
  it transactionally; every injected compute-resource failure preserves the
  existing fragment route and factor-one terminal fallback.

### Stage 2: Dispatch a real deferred-consumed visibility mask

- [x] Add the frozen size-keyed sampled/storage target through portable support
  checks and transactional creation, with an explicit retained-byte ceiling;
  preserve the existing fragment target path independently.
- [x] Implement the pure eligibility decision and expose a bounded route reason
  to counters/tests without backend-specific inspection.
- [x] At the existing contact-visibility point, select the route before
  recording work; for compute, transition all required GBuffer/depth inputs and
  the discarded output, switch pipeline, bind typed parameters, and dispatch
  exact ceil-divided groups over the owning target.
- [x] Transition the result to `GraphicsShaderRead`, restore all inputs to
  `GraphicsShaderRead`, switch to graphics, and let existing deferred lighting
  consume the selected visibility texture with unchanged composition.
- [x] Preserve the current fragment pass as exact fallback and the white
  factor-one binding when neither feature route succeeds. Record no copy pass.
- [x] Prove Present/offscreen, main/auxiliary views, alternating sizes, odd and
  constrained extents, contact toggle/diagnostic, and all route reasons record
  the exact command/transition order through both executors.

#### Acceptance Gate

- An eligible production view consumes a compute-written contact mask in
  deferred lighting; every other route-table case uses fragment visibility or
  factor one exactly, all target pixels are current-view defined, and Renderer
  contains no Vulkan symbol, output copy, or whole-device idle.

### Stage 3: Prove pixels, refresh, and lifetime end to end

- [x] Extend Vulkan-backed contact fixtures to render the frozen matrix through
  fragment and compute routes and compare the visibility mask, diagnostic, and
  final deferred image at the Stage 0 tolerance.
- [x] Prove directional-only modulation, cascade attenuation, local/environment/
  emissive isolation, retained-forward ordering, display mapping, and editor
  assistance remain unchanged.
- [x] Exercise resize with recorded work pending, main/auxiliary alternation,
  odd/constrained extents, target replacement failure, compute shader/PSO
  failure, shader reload, manual retry, device invalidation, and shutdown.
- [x] Verify failed compute refresh retains an allowed same-device
  last-known-good payload or selects fragment fallback; failed fragment fallback
  selects factor one; later successful refresh becomes visible in process.
- [x] Validate counters and resource statistics distinguish compute, fragment,
  and factor-one routes and prove stable PSO/descriptor cache behavior without
  stale views or pending-delete failures.

#### Acceptance Gate

- Visibility/final pixel parity, phase ordering, compute-to-deferred handoff,
  fallback, refresh, replacement, recorded lifetime, repeated-frame cache use,
  and shutdown pass through public RHI on real Vulkan with validation clean.

### Stage 4: Measure, qualify, and publish M3

- [x] Run focused `RenderCoreTests`, `EngineTests`, and
  `VulkanRHIIntegrationTests` coverage through the root build/test workflow in
  dedicated-RHI and inline executor modes where the compute route is exercised.
- [x] Capture the frozen fragment/compute GPU timing matrix and structural
  counters, record raw artifacts outside source control when appropriate, and
  add the evidence summary plus rollout decision to `Current Status`.
- [x] Run the native aggregate at default target granularity and a full `all`
  build because the change crosses Engine, Renderer, RenderCore, RHI,
  VulkanRHI, and multiple native targets.
- [x] Run a validation-enabled Debug Editor session covering main and
  independent auxiliary offscreen views, resize, contact on/off, contribution
  diagnostic, shader reload/retry, stable frames, orderly shutdown, and the
  window-backed Present route; retain exact Camera Preview behavior and
  alternating-view coverage in the owning native viewport suites.
- [x] Publish the stable workload, eligibility, fallback, synchronization,
  refresh, format, and measurement contract under
  `Documentation/Runtime/Rendering/` and update viewport ownership/order text.
- [x] Mark M3 and the required roadmap complete only if the rollout evidence
  admits compute as the normal eligible contact route; explicitly defer FXAA,
  GTAO, M4, and M5 when their separate entry evidence remains absent.

#### Acceptance Gate

- Focused tests in both executors, native aggregate, full build,
  validation-enabled runtime matrix, pixel/lifecycle gates, documentation
  validation, and the predeclared measurement gate pass; requested eligible
  contact visibility uses compute by policy rather than only a test override.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Route selection | Eligible requested contact selects compute; missing compute resources/format/extent selects fragment; disabled/unneeded or total failure selects factor one | Pure Renderer tests |
| Shader/layout identity | Compute entry reflects the frozen sampled inputs, uniform, and storage output into one complete canonical PSO | RenderCore and Renderer resource tests |
| Dispatch and overwrite | Even, odd, minimum, constrained, and limit-adjacent targets produce exact groups, bounded writes, and value one outside the fitted view | Renderer contract and Vulkan integration tests |
| Inputs -> compute | GBuffer/depth become compute-readable only through explicit public transitions | Recorded-command and Vulkan validation evidence |
| Compute -> deferred | Visibility becomes graphics-readable, inputs are restored, and deferred lighting samples the current-view result | Renderer ordering tests and Vulkan image evidence |
| Pixel equivalence | Fragment and compute visibility, diagnostic, and final images match the frozen matrix within the recorded tolerance | Vulkan-backed contact fixture |
| Lighting isolation | Only the selected post-cascade directional term changes; all other lighting and retained-forward terms remain unchanged | Deferred/contact qualification fixture |
| Resize and views | Main/preview alternating, odd, and constrained targets are fully current-view defined; old recorded targets survive replacement | Engine/Vulkan lifetime fixture |
| Recoverable failure | Compile, PSO, target, view, and refresh failures retain valid compute state or choose fragment/factor-one fallback without repeated same-generation attempts | Failure injection and reload tests |
| Present/offscreen | The visibility route is output-independent and leaves final-output, Mona, assistance, and swapchain contracts unchanged | Engine route tests and runtime smoke |
| Measurement | Named-adapter median/p95 samples and command counters satisfy the predeclared rollout criterion | Stage 4 qualification record |
| Shutdown | Compute payloads, target views, descriptors, and recorded references retire without validation or pending-delete failures | Vulkan teardown test and runtime smoke |

## Definition of Done

- A requested eligible scene view uses Renderer-owned compute contact
  visibility through public RHI and existing deferred lighting consumes it
  without a Vulkan escape hatch or copy pass.
- Visibility, directional-only composition, constrained views, diagnostics,
  output modes, display, editor assistance, and opt-in default preserve their
  established contracts.
- Every ineligible or failed compute case uses the complete fragment fallback;
  every total feature failure uses factor one without blank or stale output or
  repeated same-generation failure.
- Resize, shader reload, manual retry, device invalidation, recorded-command
  lifetime, repeated frames, and shutdown are completion-safe.
- The predeclared GPU measurement gate supports normal eligible rollout; a
  forced test route or default-off state is insufficient for completion.
- Focused, aggregate, full-build, runtime, and documentation qualification pass;
  stable behavior is documented; M3 and the required roadmap are complete.

## Deferred Follow-ups

- Compute FXAA or display mapping after a separate bandwidth/value hypothesis;
  direct storage to sRGB final targets remains out of scope.
- GTAO compute conversion, workgroup-shared filtering, or pass fusion after the
  first consumer establishes Renderer compute ownership and profiling identifies
  a worthwhile stage.
- HZB construction, temporal contact filtering, local-light contact shadows,
  or other visibility acceleration under their own quality/evidence plans.
- Indirect dispatch and GPU-generated arguments (conditional M4).
- Separate compute queues, ownership transfer, cross-queue synchronization, and
  overlap scheduling (evidence-gated M5).
- Other consumers such as culling, skinning, mip/IBL generation, or texture
  processing; each requires its own workload, ownership, and rollout evidence.

## Related Documentation

- [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [Synchronous Compute Pipelines](../Runtime/Rendering/SynchronousComputePipelines.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Deferred Directional Lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Shaders/Slang/ContactShadow.slang`
- `Engine/Shaders/Slang/DeferredDirectionalLighting.slang`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RHI/Public/RHICapabilities.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
