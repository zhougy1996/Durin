# Dynamic Sky Lighting Plan

Summary: Replace the bespoke IBL asset payload with ordinary HDR cube sources and an independent Sky Light component that supports bounded GPU updates from a dynamic sky.

Last reviewed: 2026-09-10

Status: Archived
Completed: 2026-09-10

## Current Status

Stages 0-6 are complete. HDR asset workflows, GPU capture/filtering, scene
ownership, lifecycle recovery, Studio/content migration, clean-cache Cook and
packaged Game are validated. All 87 affected native targets and the dedicated
forward/deferred qualification passed. The final 1920x1080 Game run passed
three 120-update batches, memory/steady-overhead budgets and actual scene-tick
latency bounds. Long-lived contracts are in
[Sky Lighting](../../../Runtime/Rendering/SkyLighting.md); detailed receipts follow.

## Goal

Authors use ordinary HDR TextureCube assets and an independent Sky Light
component. A specified cube or the selected procedural sky supplies linear
radiance. Renderer captures and filters that radiance on the GPU, then publishes
a complete lighting generation to both deferred lighting and retained forward
consumers. Changing the procedural sky updates diffuse and specular illumination
at runtime without an editor bake or CPU readback.

## Scope

- Specified HDR cube and captured-sky source modes.
- A bounded procedural sky foundation, shared by visible sky and capture, with
  an animated sun-direction input and authored sky parameters.
- Manual refresh and configurable low-frequency automatic refresh.
- GPU diffuse convolution and GGX specular prefiltering, with an engine-shared
  BRDF LUT and complete-generation replacement.
- Editor component placement, diagnostics, normal asset import/reimport, cooked
  Game execution, default Studio migration, and removal of `.iblbulk` handling.

Excluded: volumetric-cloud/fog capture, arbitrary scene geometry and local
reflection probes, full atmospheric multiple scattering, a complete weather or
day/night authoring system, baked GI or new sky occlusion, seamless blending
between lighting generations, and production time slicing across frames.
Existing cloud rendering remains outside the capture; the editor must make
that limitation visible. Low-frequency updates do not by themselves guarantee
a frame-time budget: qualification must measure the update-frame peak.

## Selected Design

### Ownership and data

Engine owns a proposed `DSkyLightComponent`/actor, source-mode settings,
eligibility, stable identity, and immutable scene publication. Renderer owns
capture, convolution shaders, scheduling, and GPU resource lifetime. TextureBuild
owns ordinary HDR-preserving cube projection, encoding, and conventional mips;
lighting prefilter mips are not ordinary texture mips. Do not introduce a separate
EnvironmentLightingBuild module as a prerequisite for this GPU runtime path.

Users need not create a separate EnvironmentLighting asset. Ordinary TextureCube
source and platform data use existing package/BulkData mechanisms. Filtered
lighting is transient Renderer-owned derived state in this first version; no
new sidecar, GPU readback, or persistent IBL DDC format is required. Cook includes
the referenced source's GPU-ready platform data, required global shaders, and
shared LUT. Game performs the bounded initial GPU update. Texture DDC remains
responsible for ordinary source-to-platform texture builds.

Keep diffuse irradiance as a small cube for this change. Store dimensions, format,
and prefilter mip count explicitly and remove consumers' reliance on the current
fixed environment constants. Keep the BRDF LUT independent of sky identity and
refresh; its version must match the material BRDF convention.

### Sky and scene semantics

SpecifiedCube references a normal HDR cube. CapturedSky consumes the scene's
selected sky provider through a dedicated radiance-rendering interface. The
provider supplies one immutable snapshot to all six faces, in linear HDR before
exposure/tone mapping and without surface lighting, post-processing, cloud
history, or recursive Sky Light contributions. Visible procedural sky and
capture share radiance evaluation, not the main view's screen-space output.

One eligible Sky Light is selected deterministically per scene using priority
and stable identity; component removal, hiding, source loss, and multiple worlds
must not leak selection. Light intensity and specified-cube rotation are applied
at sampling time without re-filtering. Captured sky transforms and provider
parameters are capture inputs; a standalone Sky Light rotation must not rotate
captured lighting away from the visible sky. Stage 0 freezes tint and exposure
semantics and the sun-disc/direct-directional-light energy convention.

### Threads, updates, and failure

GameThread publishes immutable settings and stable texture references; render
work retains no managed assets or components. One capture/filter job is admitted
per scene at a time. Manual and timed requests coalesce, continuous sky changes
cannot starve publication, and the next job uses the latest pending snapshot.
An in-flight snapshot may finish while sky time advances; a source/component
replacement invalidates its publication authority. Publish snapshot identity
and age for diagnostics so bounded temporal lag is distinguishable from stale
ownership.

Build a candidate set separately from the last complete set. Publish only when
all faces, diffuse output, and specular mips are valid and appropriately ordered
for GPU consumption. Never expose mixed generations. Retain the last complete
set for a recoverable update failure on the same active source; source removal
or replacement must not indefinitely display another source's lighting. Use a
defined neutral fallback until the new source is ready. Device invalidation
discards invalid GPU generations and schedules recovery through the existing
resource coordinator. Retire old resources only after queued work releases them.

## Frozen Stage 0 Decisions

These are implementation requirements, not claims about existing runtime support.

### Reuse points and required additions

| Concern | Existing implementation | Required change |
| --- | --- | --- |
| Visible sky | `DSkyBoxComponent`, `FSkyBoxSceneProxy`, `FSkyBoxRenderer`, `SkyBox.slang` reconstruct a rotation-only world direction and write linear scene color | Add an analytic provider and a shared direction-to-radiance shader function; retain ordinary cube SkyBox rendering |
| Selection | `FVolumetricCloudSceneRegistry::GetActive` orders priority, persistent GUID and object path, and rechecks counted texture references | Use the same ordering for separate Sky Light and procedural-sky registries; the single-token SkyBox registry cannot provide this selection |
| Scene lifetime | `FScene::TryAddVolumetricCloudProxy`, exact-pointer removal and `Release_RenderThread` own detached snapshots | Add scene-owned lighting state and provider membership; no process-global active sky or component pointer in render work |
| HDR import | `PrepareTextureCubePanoramaSource` already stores RGBA32F `LongLatCube` source | Add explicit output mode; current `TextureCubeBuilder` tone maps, imported-face scratch storage requires RGBA8, and `TextureBuilder::BuildMipChain` selects BC UNORM formats |
| Lighting consumers | `FEnvironmentLightingResources`, `FSurfaceMaterialResources` and deferred lighting bind the same legacy environment | Replace initialization and binding ownership; `SurfaceLighting.slang` currently hardcodes `roughness * 6.0` |
| Graph | `FRDGBuilder` declares exact face/mip ranges, typed compute storage images and all-range extraction | Add capture/convolution passes and candidate extraction; do not add manual barriers or infer GPU completion from `Execute` |
| Shaders | `GSkyBoxShaderSet` demonstrates typed `GameAndEditor` global registration | Register capture, convolution, LUT and visible analytic sky before shader inventory freeze; Cook must request every required set |
| Recovery | `FRendererResourceCoordinator` fans out shader/device/manual invalidation; creation slots preserve complete payloads | Route scene lighting and shared LUT through that fan-out; separate source authority from device/shader generations |
| Frame integration | `FSceneRenderPipeline` currently ensures legacy IBL before scene rendering | Dispatch scene lighting updates independently of view existence, and resolve one complete generation before deferred/forward bindings; multiple views must not multiply update admission |

Clouds remain a later scene-color feature. Neither cloud spatial evaluation,
temporal history nor cloud shadows participate in the sky provider. A captured
sky diagnostic must say that clouds, fog and geometry are excluded.

### Analytic sky provider

Add `DProceduralSkyComponent`/actor in Stage 3. This is an authored, infinite,
direction-only sky, not an atmospheric scattering approximation. Publish one
normalized world-space **to-sun** direction `S`, world-space zenith `U`, linear
RGB zenith/horizon/ground colors `Z/H/G`, horizon exponent `p`, broad solar halo
color `A`, halo exponent `q`, tint `T`, and exposure `e` in one immutable value.
For normalized ray `D`, use the following shared visible/capture evaluator:

```text
h = clamp(dot(D, U), -1, 1)
base = h >= 0 ? lerp(H, Z, pow(h, p)) : lerp(H, G, -h)
halo = A * pow(max(dot(D, S), 0), q) * smoothstep(-0.05, 0.05, h)
L(D) = (base + halo) * T * exp2(e)
```

Defaults: `U=(0,0,1)`, `S=normalize(-0.4,0.5,0.75)`,
`Z=(0.18,0.28,0.50)`, `H=(0.30,0.32,0.35)`,
`G=(0.025,0.020,0.018)`, `A=(0.5,0.39,0.275)`,
`p=1`, `q=16`, `T=(1,1,1)`, `e=0`.
Colors and tint are finite `[0,16]`, `p` is `[0.25,8]`, `q` is `[1,64]`,
and exposure is `[-8,8]` EV. Reject a zero/nonfinite direction and normalize
valid directions. Clamp final analytic radiance to `[0,16384]` with an authored
parameter-limit diagnostic when the bound is reached, keeping subsequent half
float irradiance finite. Component rotation transforms local zenith and sun
direction before publication; component translation and scale have no effect.
An explicit sun-direction setter supports Tick-driven animation; a demonstration
animates it continuously, rather than rotating an HDR cube.

There is **no concentrated sun disc** in either visible or captured radiance.
The broad halo is indirect sky energy. A separately authored directional light
owns direct sun energy and shadows; the demonstration drives its direction from
the same to-sun value, with the sign conversion required by the light contract.
The provider neither creates nor modifies a directional light. A realistic sun
disc or physical solar irradiance calibration requires a later scope decision.

An eligible selected procedural sky takes precedence over a cube SkyBox for
visible scene sky. Without one, existing cube SkyBox rendering remains. CapturedSky
requires that same selected procedural provider; it does not silently capture a
cube SkyBox. A scene with no provider reports missing provider and uses neutral
lighting until a provider becomes eligible.

### Radiance and sampling conventions

- Reuse [Cube Textures](../../../Runtime/Rendering/CubeTextures.md) exactly: world
  `+X/+Y/+Z` is forward/right/up, layer order is `+X/-X/+Y/-Y/+Z/-Z`, rows are
  top-to-bottom and samples use pixel centers. Share the existing face-direction
  equations with capture. Longitude wraps and latitude clamps; filter samples
  use cube directions across edges, never clamp a convolution to one face.
- Radiance is nonnegative linear RGB in the engine's existing linear sRGB
  primaries. No exposure adaptation, display transfer or tone mapping enters
  stored HDR platform data or filtered outputs. Alpha is one. Reject nonfinite,
  negative or out-of-range HDR lighting input instead of silently converting it
  to LDR. Source values above 16384 are outside the initial lighting envelope.
- Ordinary HDR cube output uses uncompressed `RGBA32_FLOAT`, complete ordinary
  mips, and `bSRGB=false`. This avoids a new BC6H encoder and half conversion
  while preserving the imported float source. Existing and missing output-mode
  properties default to the current LDR recipe. The explicit HDR mode is valid
  for float panorama source only in this version; six-face LDR import is unchanged.
  Derive each lower ordinary mip with solid-angle-aware direction sampling from
  the panorama; do not introduce face-edge clamping seams or use lighting mips.
- Store diffuse **irradiance E**, not `E/pi`: a constant radiance `C` must yield
  `pi*C`. Cosine-weighted Hammersley integration multiplies the sample average
  by pi. Keep the existing Lambertian division by pi in `PBRLighting.slang`.
- Specular uses GGX importance sampling, `alpha=roughness^2`, `N=V`, and
  `NoL`-weighted normalized radiance. Mip zero copies source radiance; mip `m`
  uses perceptual roughness `m/(MipCount-1)`. Consumers use
  `roughness*(MipCount-1)`, clamped to the published mip range. Use source
  ordinary mips with sample-solid-angle LOD for the bounded hotspot test.
- The shared 128-square RGBA16F BRDF LUT retains the existing split-sum scale
  and bias in RG, and uses the current `EnvironmentLightingBuild.cpp` integration
  convention with 1024 Hammersley samples. Give that convention an explicit
  version independent of any sky identity. Generate it once on GPU per
  device/shader generation; Cook includes its generator shaders, not a legacy
  environment payload or CPU bake provider.
- Provider tint/exposure belong inside capture and visible evaluation. Sky Light
  has only a finite scalar intensity `[0,16]` applied after sampling both diffuse
  and specular; it does not alter visible sky. SpecifiedCube rotation is inverse
  component rotation applied to normal/reflection lookup directions. CapturedSky
  ignores Sky Light transform. Neither intensity nor specified-cube rotation
  invalidates convolution, and neither modifies the BRDF LUT.
- All capture faces use one immutable provider value at an origin of zero.
  Camera translation, near plane, world origin and viewport size are irrelevant.

### Ownership, scheduling and fallback

Sky Light and procedural provider selection each use enabled/visible/valid
candidates, priority descending in `[-1000,1000]`, persistent GUID ascending,
then component object path ascending. Class defaults allocate no persistent ID.
Keep separate ownership epoch and content revision: ordinary animated provider
changes increment revision, while selected component/provider replacement,
source asset assignment or resolved texture allocation replacement increments
authority. Replacing the source immediately invalidates the old active set and
any candidate's right to publish. Counted references retain the exact resolved
input allocation for the admitted job.

Maintain at most one admitted job and one latest pending snapshot per scene.
Automatic refresh defaults to enabled with a 1-second interval, clamped to
`[0.25,60]` seconds. Manual refresh coalesces into the pending snapshot. Initial
eligible source and source replacement request one update even when automatic
refresh is disabled. Start the next interval at admission; continuously changing
content cannot cancel a same-authority candidate. Publish its captured revision
and time, then admit the latest pending value when due. No catch-up job loop.

Use one ordered immediate RHI timeline. Capture/filter all faces and mips in one
update graph, extract into candidate-owned references with shader-read final
access, and atomically replace the complete set only after successful graph
recording and authority validation. Later consumer commands on the same timeline
observe ordered writes; this does not claim GPU completion. Command retention and
RHI GPU-aware deletion retire resources. No production readback or idle wait.

Black diffuse/specular cubes and a valid sampler are the neutral fallback:
zero environment contribution, while direct lighting continues. An unavailable
LUT likewise disables environment contribution as a whole. Same-authority
recoverable failure retains the last complete set and schedules a bounded retry
no more frequently than the refresh minimum; replacement/removal cannot retain
another source's lighting. Device invalidation discards all GPU generations and
requests startup recovery. Shader refresh may retain a same-device compatible
set until the complete replacement succeeds. Explicit scene release cancels
pending admission and removes authority before retiring resources.

### Supported envelope and qualification limits

The first supported lane is Windows x64 Vulkan, `windows-msvc-x64`, Editor and
cooked Win64 Game. The checked local default is `Win64-Debug-DurinEditor`.
CPU contract coverage runs there; performance acceptance uses an optimized
Release profile, with the exact selected preset and build revision recorded.
Other RHIs/platforms are not qualified by this plan. Require compute dispatch,
sampled/filterable RGBA32F cube input, sampled/storage RGBA16F images and per-face,
per-mip views. Add capability validation before allocation: current inspected
RHI headers do not expose a complete format-support query for this feature.
Unsupported capability reports a stable diagnostic and uses the neutral fallback;
do not start an invalid dispatch or silently switch algorithms.

The named qualification device is **NVIDIA GeForce GTX 1060 6GB**, reported by
`nvidia-smi` on 2026-09-10 with driver **582.66**, 6144 MiB VRAM. This is hardware
inventory, not a Vulkan feature test or a performance receipt. CIM inventory was
denied in this session; `nvidia-smi` supplied the device identity successfully.

| Limit | Initial envelope / acceptance ceiling |
| --- | --- |
| Source HDR cube | At most 512 pixels per face, complete ordinary mips; reject larger lighting sources with a resize instruction |
| Captured radiance | 128 pixels per face, RGBA16F, complete ordinary mips |
| Specular output | 128 pixels per face, RGBA16F, 8 mips; 128 samples per filtered texel |
| Diffuse output | 16 pixels per face, RGBA16F, one mip; 256 samples per texel |
| Shared LUT | 128 by 128 RGBA16F, 1024 samples per texel, once per generation |
| Convolution update | GPU median <= 4 ms, p95 <= 6 ms, maximum <= 8 ms |
| Initial update plus LUT | GPU maximum <= 20 ms; measured separately from pipeline compilation |
| Steady-frame incremental overhead | GPU p95 <= 0.10 ms and render-thread CPU p95 <= 0.20 ms |
| Lighting-owned live GPU image allocation | <= 16 MiB, including candidate, active, capture, LUT and in-flight retired outputs |
| Referenced source plus lighting allocations | <= 64 MiB per scene at maximum source size, including input retained during replacement |
| Manual/new-source latency | Admit by the next scene update tick when ready; publish within two ticks after admission |
| Automatic latency | <= interval plus two scene update ticks when shaders/resources are ready |

Retirement counts against memory limits: delay admission while the retained
allocation budget is exhausted, coalescing the pending value. Do not allocate
another full set every rendered view. Report allocation backpressure separately
from unavailable source, compile pending, retry and ready. Latency bounds exclude
documented resource/backpressure failures, whose duration remains visible.

Qualify a 1920x1080 scene at the fixed envelope with one Sky Light. Use an
exclusive quiet GPU lane, 30 warm-up updates and 120 measured updates, repeated
three times; record median/p95/maximum, CPU admission/publication times, frame
peak and live image allocations. Measure the same scene with updates disabled
for steady overhead. Measure cold shader/pipeline startup wall time separately;
never include it in or hide it behind warm GPU timing. These are selected budgets,
not achieved measurements; Stage 6 must reduce quality or explicitly revise the
plan if they fail.

### Studio migration and validation sequence

Generate a 512x256 float panorama by evaluating the existing
`SampleStudioRadiance` function at panorama pixel-center directions, then import
it as an ordinary 128-face HDR cube at
`/Engine/Renderer/DefaultStudioCube.DefaultStudioCube`. This preserves the current
authored Studio radiance function, rather than attempting to recover radiance
from prefiltered `.iblbulk`. The normal package embeds the authoritative source;
the generator's temporary HDR file is not a Game dependency.

Scene templates/sample worlds that currently rely on implicit Studio lighting
receive a normal serialized SpecifiedCube Sky Light reference. Submission-local
material previews explicitly supply the same standard environment through their
existing environment input. Empty worlds use neutral fallback; Renderer never
loads Studio by a fixed path. On the first Game frame the source or LUT may be
pending, so rendering uses neutral contribution until the first complete GPU
generation is ordered for consumption.

After all checked-in references and fixtures migrate, remove the old Studio
package, `.iblbulk`, obsolete type, serializer and bake target in Stage 5. External
legacy packages are intentionally incompatible: diagnose the removed asset type
with instructions to reimport the original HDR as a TextureCube and assign a
Sky Light. A redirector cannot convert asset class or recover source radiance;
do not add a permanent dual loader. Publish these migration instructions before
removing the loader.

Stage 1 should begin with `TextureTests` and `TextureCookIntegrationTests`,
adding coverage for HDR reconstruction, source-independent rebuild, serialization,
DDC separation and GPU sampling. Discover additional selections with `test list`
as the implementation grows. Stage 2 needs CPU selection/lifecycle contracts;
Stages 3–4 need offscreen Vulkan correctness and registered qualification cases.
Stage 5 requires clean-cache Cook and packaged Game. Stage 6 additionally records
editor workflows, forward/deferred agreement and the quiet-lane measurements.

### Stage 0 handoff (2026-09-10)

Inspected the concrete code paths named above and the authoritative cube,
cloud-selection, global-shader, RDG, frame-preparation and resource-recovery
contracts. `DevTool status` reports clean recovery state in the configured
Windows Editor profile. `DevTool test list texture` resolves `TextureTests` and
`TextureCookIntegrationTests`; no native tests or GPU workloads were run for
this decision-only stage. All later implementation and qualification gates
remain open.

## Implementation Stages

### Stage 0: Freeze the sky source and rendering contracts

- [x] Inspect SkyBox, cloud, scene selection, RDG, global shader, and resource
  recovery integration; record concrete reuse points and missing capabilities.
- [x] Select and document a bounded procedural sky model with animated sun
  direction. If no reusable atmosphere exists, include a minimal analytic sky
  provider in Stage 3; do not substitute HDR rotation for procedural-sky acceptance.
- [x] Freeze coordinate/seam rules, linear color space, irradiance normalization,
  roughness-to-mip convention, sky tint/exposure, sun-disc energy treatment,
  selection/fallback semantics, and capture-origin assumptions.
- [x] Select supported RHI/profile coverage, initial resolution/sample limits,
  refresh interval bounds, GPU capability failure behavior, and numeric peak
  update-time/memory budgets on a named qualification device.
- [x] Define the default Studio migration and initial-Game-frame fallback,
  including how old checked-in assets and external legacy references are handled.

Gate: record executable decisions and measurement limits in this plan before
implementing dependent stages. Any expansion to a full atmosphere or cloud
capture is a separate scope decision, not implicit work.

### Stage 1: Preserve HDR through ordinary cube platform data

Depends on Stage 0.

- [x] Add an explicit HDR-preserving cube output recipe using the existing
  float panorama source; preserve current LDR behavior and define old asset defaults.
- [x] Carry float format through projection, mip generation, serialization,
  validation, upload, Cook, and editor previews. Display mapping belongs to the
  preview/main display path, not stored lighting radiance.
- [x] Bump affected recipe/schema versions as required; changing build settings
  or reimporting must invalidate the correct texture derived data.
- [x] Verify pixels above one survive import, rebuild without the original file,
  save/load, and Cook; verify directional face markers and cube seams.

Gate: an ordinary cooked HDR cube supplies finite linear radiance to GPU sampling
without destructive tone mapping or a new external source dependency.

#### Stage 1 handoff (2026-09-10)

Added explicit LDR/HDR output, preserving old-package LDR defaults. HDR bypasses
RGBA8 scratch faces and produces RGBA32F ordinary mips directly from the retained
panorama. Standard reimport preserves output settings. Builder/projection versions
are 4/3; TXPL adds stable format 13 without changing schema 2 record layout.
The import dialog exposes the output choice; viewport and thumbnail display
mapping remain after sampling. The implemented contract is in
[Cube Textures](../../../Runtime/Rendering/CubeTextures.md).

Validation on Win64-Debug-DurinEditor:

- `DevTool test TextureTests --timeout 600`: 107/107 passed, including HDR
  rebuild after deleting the original source, exact all-mip package reload,
  deterministic Cook and source-free cooked loading. Receipt:
  `Build/.agent-state/logs/20260910-150051-066075-33284-TextureTests.log`.
- `DevTool test SkyBoxVulkanIntegrationTests --timeout 600`: passed. The HDR
  route now uses real RGBA32F, reads back every face/mip exactly, and checks
  display-mapped GPU sampling along the principal axes. Receipt:
  `Build/.agent-state/logs/20260910-150136-071828-35016-SkyBoxVulkanIntegrationTests.log`.
- `DevTool test affected --timeout 600`: passed after adding a constant-radiance
  all-mip test. Receipt:
  `Build/.agent-state/logs/20260910-150348-672408-31708-ctest.log`.

Initial test failures were stale recipe-version goldens and test fixture
save/mount cleanup mistakes; corrected without weakening comparisons. Full editor
`all` build remains required before the final feature handoff. No dynamic-sky
performance claim is made by these texture correctness results.

### Stage 2: Introduce independent Sky Light scene ownership

Depends on Stage 0; HDR-source validation completes with Stage 1.

- [x] Add reflected component/actor settings, source modes, refresh controls,
  finite parameter bounds, diagnostics, and explicit recapture entry point.
- [x] Implement immutable proxy publication, deterministic selection, source
  texture reference recovery, and clean unregister/world teardown behavior.
- [x] Add editor placement and Details integration with clear source eligibility
  and pending/ready/failure status; preserve save/load and undo/redo semantics.
- [x] Establish the neutral startup fallback and shared forward/deferred binding
  interface without making a second lighting owner.

Gate: component changes and scene lifecycle produce deterministic ownership;
invalid or unavailable inputs never publish partial lighting resources.

### Stage 3: Capture a bounded dynamic sky into linear radiance

Depends on Stages 1 and 2.

- [x] Implement/reuse the Stage 0 procedural sky provider and share its evaluator
  between visible sky and capture; expose bounded sun-direction/sky parameters.
- [x] Add six-face HDR capture using one frozen provider snapshot; provide the
  specified-cube input route without unnecessary scene rendering.
- [x] Exclude unsupported clouds, fog, scene geometry, display mapping, and
  recursive lighting. Explain unsupported capture content in source diagnostics.
- [x] Verify face orientation/seams, camera-translation independence for the
  chosen sky model, HDR energy, and agreement between visible and captured sky.

Gate: runtime sky-parameter changes produce a valid changed radiance cube;
neither a main viewport nor editor services are required for the capture contract.

### Stage 4: Generate and publish GPU environment lighting

Depends on Stage 3.

- [x] Add RDG/global-shader diffuse convolution and GGX specular prefilter passes
  with explicit mip/face resource access, bounds, and shared BRDF conventions.
- [x] Supply a shared BRDF LUT independently of environment capture and refresh.
- [x] Implement candidate/active generations, one-job admission, request
  coalescing, manual/timed updates, and publication authority checks.
- [x] Bind the same complete generation to deferred and retained forward
  consumers; apply intensity and specified-cube rotation consistently.
- [x] Integrate retry, device invalidation, resource retirement, and source
  replacement; expose update cost, generation age, and failure diagnostics.

Gate: changing the procedural sky changes diffuse and roughness-dependent
specular lighting in Game, with no synchronous CPU readback, mixed generations,
unbounded queued jobs, or starvation under continuous animation.

#### Runtime implementation handoff (2026-09-10)

Independent reflected actors/components publish immutable native snapshots with
persisted selection GUIDs, exact removal tokens, bounded authoring, recapture,
and Details diagnostics. Captured mode requires an eligible analytic provider;
specified mode requires an ordinary linear HDR cube. Missing inputs bind black.
The renderer no longer loads the fixed Studio environment at startup.

Visible sky and six-face capture import the same linear analytic evaluator.
World Tick drives capture without a view. RDG records exact face/mip writes and
extracts complete irradiance/prefilter generations together. One GPU timing
query per scene gates subsequent admission; animation coalesces without cancelling
admitted work. Forward and deferred paths share intensity, rotation, and mip
controls. The shared LUT is generated independently of source changes.

Validation:

- Full Editor `DevTool build --target all` passed:
  `Build/.agent-state/logs/20260910-153306-678503-16740-cmake.log`.
- `DevTool test SkyBoxVulkanIntegrationTests --timeout 600` passed, including
  viewport-free World Tick, HDR capture, changed sun, source disable, constant
  `E = pi*C` across six faces, constant prefilter across all eight mips, and
  manual refresh while automatic refresh is disabled:
  `Build/.agent-state/logs/20260910-153928-355797-27224-SkyBoxVulkanIntegrationTests.log`.
  One sampled update took 2.48259 ms; this is not the Stage 6 distribution claim.
- `DevTool test affected --timeout 600` passed all selected targets:
  `Build/.agent-state/logs/20260910-154104-602210-2760-ctest.log`.

Initial failures exposed cube-face storage view selection and fresh dynamic
uniform-buffer state assumptions. Both were corrected in the actual submission
path. Persistent-ID default-object exceptions now match the existing cloud
identity convention. No test assertions were weakened.

### Stage 5: Migrate content and remove the bespoke IBL path

Depends on Stage 4.

- [x] Produce the default Studio as an ordinary HDR cube and configure normal
  scene/component defaults without the fixed DEnvironmentLighting load path.
- [x] Migrate checked-in consumers, fixtures, and generation tools; remove
  `.iblbulk` path derivation, external Cook dependency declarations, serializer,
  obsolete DEnvironmentLighting type/data, and obsolete fixed-size contracts.
- [x] Ensure the shared LUT no longer relies on the old environment payload.
- [x] Verify clean-cache Cook packages source cubes and shaders correctly, and
  packaged Game starts and updates lighting without editor/build providers or
  the original imported HDR files.
- [x] Record the chosen legacy compatibility behavior and migration instructions;
  do not retain an undocumented permanent dual loader.

Gate: production environment lighting uses ordinary assets and component state;
no active source/content/test path requires `.iblbulk`.

### Stage 6: Qualify the complete workflow and publish contracts

Depends on Stage 5.

- [x] Validate a constant-radiance input against the documented irradiance
  convention and a directional HDR hotspot across specular roughness levels;
  use a bounded reference comparison to detect energy/orientation errors.
- [x] Exercise animated procedural sky, manual/timed refresh, rapid source
  replacement, missing sources, multiple components/worlds, teardown during
  work, recoverable resource failure, and device-generation invalidation.
- [x] Record GPU update-frame peak, steady-frame overhead, memory peak, and
  update latency against Stage 0 budgets. If they fail, reduce the supported
  quality envelope or explicitly revise scope before declaring completion.
- [x] Verify editor import/reimport, undo/redo, scene reopening, clean-cache
  Cook, and packaged Game; record forward/deferred visual agreement and the
  unsupported-cloud-capture limitation.
- [x] Update authoritative asset, lighting, sky, resource-lifecycle, and editor
  workflow documents; remove superseded active contract statements and close
  this plan only after every required gate has recorded evidence.

Gate: repeatable dynamic sky-to-lighting behavior is demonstrated within the
declared capability/performance envelope and all bespoke IBL dependencies are gone.

#### Final integration evidence (2026-09-10)

World Tick now queues scene work; Launch drains it after `RHIBeginFrame`,
including scenes without a viewport. World-driven scenes admit only at this
boundary; extra views reuse the result rather than moving refresh admission
into another part of the same frame. Device invalidation clears inactive
scene generations immediately. Exported lighting images are counted by actual
backend allocations, including externally retained generations; an ordered
retirement query prevents releasing resources still used by prior views.
Vulkan timing queries are retained by the recording context until native
submission transfers ownership to the timing manager. A release-before-submit
regression test covers this handoff, including both RHI execution modes.
Allocation admission coalesces requests under a distinct backpressure status,
keeps the last complete generation, and recovers when consumers release it.

Studio is now the ordinary `/Engine/Renderer/DefaultStudioCube` source. The
Sandbox and RoadWeaver checked-in levels, new-level defaults, asset previews,
and thumbnails use regular Sky Light actors. External legacy packages fail
with the serialized unknown-class name; migration instructions live in
[Sky Lighting](../../../Runtime/Rendering/SkyLighting.md).

Correctness and workflow receipts:

- `test affected --preset Win64-Debug-DurinEditor --timeout 600`: all 87 native
  targets passed, including HDR import/reimport, retained-source rebuild, Cook,
  shader/material contracts, previews and resource recovery.
  `Build/.agent-state/logs/20260910-171832-853986-6080-ctest.log`.
- `test SkyBoxVulkanIntegrationTests --preset Win64-Debug-DurinEditor --timeout 600`:
  passed real HDR face/mip readbacks, constant `E=pi*C`, analytic hotspot
  irradiance and roughness response, visible/captured agreement, multiple Worlds,
  retained-generation backpressure/recovery, manual/timed updates, missing and
  replaced sources, teardown during work, and inactive-scene device invalidation.
  `Build/.agent-state/logs/20260910-164748-805366-30812-SkyBoxVulkanIntegrationTests.log`.
- `test AssetPackageReloadTests --preset Win64-Debug-DurinEditor --timeout 600`:
  11 tests passed, including Sky Light undo/redo, source/intensity persistence,
  and stable identity after package reopening.
  `Build/.agent-state/logs/20260910-164830-698203-32732-AssetPackageReloadTests.log`.
- `test DirectionalShadowBaselineVulkanTests --preset Win64-Debug-DurinEditor
  --mode qualification --timeout 600`: all three cases passed. Actual lit
  receivers consume dynamic IBL in both forward and deferred, preserving the
  existing display-error bounds; disabling Sky Light changes their output.
  `Build/.agent-state/logs/20260910-165944-569156-32904-ctest.log`.
  Seventy-five image goldens and two exact motion counts were re-recorded for
  neutral black without an authored Sky Light. Representative planar, disabled
  shadow and defective-gap images were visually checked; exact hash checks and
  all shadow quality bounds remain. Background sky is covered separately from
  the valid-receiver parity test.
- Clean isolated Cook DDC, then `cook --preset Win64-Debug-DurinEditor
  --project Sandbox/Sandbox.dproject --output Engine/Binaries/Win64/Release/Runtime/DurinGame
  --target win64 --target-profile game --root /Game/Levels/GrayboxStage15
  --root /Game/Models/GrayboxPawn --root /Engine/Materials/DefaultMaterial
  --no-incremental`: 8 packages, 0 hits, 0 failures, 5,981,190 published bytes.
  `Build/.agent-state/logs/20260910-170132-175167-14992-DurinAssetTool.log`.
  Cooked Game maps virtual mounts to its read-only Cook root before loading;
  cooked material programs validate their runtime ABI without a compiler provider.

#### Final performance and handoff (2026-09-10)

`run --preset Win64-Release-DurinGame --project Sandbox/Sandbox.dproject --args
--hidden-window --sky-lighting-runtime-smoke --exit-after-ticks=8000` passed on
GTX 1060 6 GB / driver 582.66 / Vulkan at 1920x1080, with an exclusive quiet GPU
lane, 30 warm-up updates, then three batches of 120 animated updates. No
ShaderBuild, TextureBuild or importer module is present in the Game closure.
Receipt: `Build/.agent-state/logs/20260910-172140-809100-2044-DurinGame.log`.

| Batch | GPU median (ms) | GPU p95 (ms) | GPU maximum (ms) | Steady CPU p95 (ms) |
| --- | --- | --- | --- | --- |
| 1 | 2.499680 | 2.860224 | 3.156192 | 0.0040 |
| 2 | 2.604960 | 2.819488 | 2.979776 | 0.0036 |
| 3 | 2.475936 | 2.834304 | 3.315264 | 0.0045 |

- Initial GPU update including the shared LUT: 3.007552 ms, below 20 ms.
- Cold application startup to the first completed authored-cube update:
  2.784109 s, logged independently of warm GPU timings. The isolated Cook DDC
  and application pipeline cache were cleared; this is not a GPU-driver-cache reset.
- Lighting image peak: 4,481,024 backend allocation bytes (4.27 MiB), including
  active/retired generations and LUT, below 16 MiB. The 64 MiB admission check
  separately includes retained and incoming source image allocations.
- Same scene with automatic updates disabled, after 30 settling frames and at
  least 120 measured frames: scene-update GPU p95 0.000736 ms, below 0.10 ms.
- Update-frame scene GPU busy peak: 8.917952 ms. This is the sum of update-service
  and scene-view graph GPU intervals in the same frame; it excludes window UI,
  presentation, and CPU queue gaps. The convolution maximum above is the
  separately budgeted 8 ms quantity. CPU admission/publication p95: 3.4579 ms.
- Maximum automatic capture interval: 0.267418 s at a 0.25 s policy; publication
  missed at most one scene tick after the deadline. Manual recapture published
  after two scene ticks. The gate counts actual ticks, not an assumed exact
  60 Hz wall-clock conversion.

The frame-local timing scopes avoid synchronous window submission boundaries.
Qualification caught a Vulkan recorded-query ownership gap at diagnostic
shutdown; the backend now retains those queries until native submission, and
`VulkanRHIIntegrationTests` passes release-before-submit coverage in both RHI
execution modes. World-driven capture admission is once per frame; preview-only
scenes retain their view-driven route. Unsupported clouds/fog/geometry remain
outside capture as specified in Stage 0, with author-facing documentation and
source diagnostics.

Final full builds passed for Editor and Game:
`Build/.agent-state/logs/20260910-171725-371278-36396-cmake.log` and
`Build/.agent-state/logs/20260910-171639-337610-36596-cmake.log`.
Final affected regression passed all 87 targets:
`Build/.agent-state/logs/20260910-171832-853986-6080-ctest.log`.

## Validation and Execution

Follow [agent build/run guidance](../../../Agents/BuildAndRun.md) before configuring or
running targets and [agent testing guidance](../../../Agents/Testing.md) before selecting
native tests. Select exact commands and test targets after Stage 0 identifies
affected modules; this plan does not claim existing tests cover the new feature.
Each implementation handoff records commands, outcomes, visual/performance
evidence, remaining limitations, and the next stage. Stage completion requires
its gate, not merely code compilation.

## Related Code and Contracts

- `Engine/Source/Runtime/Engine/Public/Components/SkyLightComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/ProceduralSkyComponent.h`
- `Engine/Source/Programs/StudioCubeGenerate/Main.cpp`
- [Sky lighting](../../../Runtime/Rendering/SkyLighting.md)
- `Engine/Source/Developer/TextureBuild/Private/Texture/TextureCubeBuilder.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkyBoxComponent.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/EnvironmentLightingResources.cpp`
- [Cube textures](../../../Runtime/Rendering/CubeTextures.md)
- [Forward lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [Package bulk data](../../../Runtime/Assets/BulkData.md)
- [Renderer frame preparation](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer resource recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Volumetric cloud scene contract](../../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Geometry submission refactor](../../GeometrySubmissionRefactor.md)
