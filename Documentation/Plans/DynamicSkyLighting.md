# Dynamic Sky Lighting Plan

Summary: Replace the bespoke IBL asset payload with ordinary HDR cube sources and an independent Sky Light component that supports bounded GPU updates from a dynamic sky.

Last reviewed: 2026-09-10

Status: Active
Completed:

## Current Status

Planning only; implementation has not started. Repository inspection confirms
that `DEnvironmentLighting` loads an authored `.iblbulk`, while its cooked
projection already uses `FBulkData`. Renderer loads the default Studio asset
from a fixed path. The Cube panorama source already retains linear RGBA32F
HDR pixels; the existing projection recipe tone maps those values to RGBA8.
The required texture change is an HDR-preserving derived output path, not a
second source-storage system.

The inspected Engine, Renderer, and shader paths contain SkyBox rendering but
no identified SkyAtmosphere implementation. Stage 0 must resolve the procedural
sky foundation before implementation estimates or shader work. Do not assume
UE's atmosphere implementation exists here. The active Geometry Submission
Refactor may touch shared renderer integration points; coordinate against its
current state without coupling this plan to unrelated geometry changes.

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

## Implementation Stages

### Stage 0: Freeze the sky source and rendering contracts

- [ ] Inspect SkyBox, cloud, scene selection, RDG, global shader, and resource
  recovery integration; record concrete reuse points and missing capabilities.
- [ ] Select and document a bounded procedural sky model with animated sun
  direction. If no reusable atmosphere exists, include a minimal analytic sky
  provider in Stage 3; do not substitute HDR rotation for procedural-sky acceptance.
- [ ] Freeze coordinate/seam rules, linear color space, irradiance normalization,
  roughness-to-mip convention, sky tint/exposure, sun-disc energy treatment,
  selection/fallback semantics, and capture-origin assumptions.
- [ ] Select supported RHI/profile coverage, initial resolution/sample limits,
  refresh interval bounds, GPU capability failure behavior, and numeric peak
  update-time/memory budgets on a named qualification device.
- [ ] Define the default Studio migration and initial-Game-frame fallback,
  including how old checked-in assets and external legacy references are handled.

Gate: record executable decisions and measurement limits in this plan before
implementing dependent stages. Any expansion to a full atmosphere or cloud
capture is a separate scope decision, not implicit work.

### Stage 1: Preserve HDR through ordinary cube platform data

Depends on Stage 0.

- [ ] Add an explicit HDR-preserving cube output recipe using the existing
  float panorama source; preserve current LDR behavior and define old asset defaults.
- [ ] Carry float format through projection, mip generation, serialization,
  validation, upload, Cook, and editor previews. Display mapping belongs to the
  preview/main display path, not stored lighting radiance.
- [ ] Bump affected recipe/schema versions as required; changing build settings
  or reimporting must invalidate the correct texture derived data.
- [ ] Verify pixels above one survive import, rebuild without the original file,
  save/load, and Cook; verify directional face markers and cube seams.

Gate: an ordinary cooked HDR cube supplies finite linear radiance to GPU sampling
without destructive tone mapping or a new external source dependency.

### Stage 2: Introduce independent Sky Light scene ownership

Depends on Stage 0; HDR-source validation completes with Stage 1.

- [ ] Add reflected component/actor settings, source modes, refresh controls,
  finite parameter bounds, diagnostics, and explicit recapture entry point.
- [ ] Implement immutable proxy publication, deterministic selection, source
  texture reference recovery, and clean unregister/world teardown behavior.
- [ ] Add editor placement and Details integration with clear source eligibility
  and pending/ready/failure status; preserve save/load and undo/redo semantics.
- [ ] Establish the neutral startup fallback and shared forward/deferred binding
  interface without making a second lighting owner.

Gate: component changes and scene lifecycle produce deterministic ownership;
invalid or unavailable inputs never publish partial lighting resources.

### Stage 3: Capture a bounded dynamic sky into linear radiance

Depends on Stages 1 and 2.

- [ ] Implement/reuse the Stage 0 procedural sky provider and share its evaluator
  between visible sky and capture; expose bounded sun-direction/sky parameters.
- [ ] Add six-face HDR capture using one frozen provider snapshot; provide the
  specified-cube input route without unnecessary scene rendering.
- [ ] Exclude unsupported clouds, fog, scene geometry, display mapping, and
  recursive lighting. Explain unsupported capture content in source diagnostics.
- [ ] Verify face orientation/seams, camera-translation independence for the
  chosen sky model, HDR energy, and agreement between visible and captured sky.

Gate: runtime sky-parameter changes produce a valid changed radiance cube;
neither a main viewport nor editor services are required for the capture contract.

### Stage 4: Generate and publish GPU environment lighting

Depends on Stage 3.

- [ ] Add RDG/global-shader diffuse convolution and GGX specular prefilter passes
  with explicit mip/face resource access, bounds, and shared BRDF conventions.
- [ ] Supply a shared BRDF LUT independently of environment capture and refresh.
- [ ] Implement candidate/active generations, one-job admission, request
  coalescing, manual/timed updates, and publication authority checks.
- [ ] Bind the same complete generation to deferred and retained forward
  consumers; apply intensity and specified-cube rotation consistently.
- [ ] Integrate retry, device invalidation, resource retirement, and source
  replacement; expose update cost, generation age, and failure diagnostics.

Gate: changing the procedural sky changes diffuse and roughness-dependent
specular lighting in Game, with no synchronous CPU readback, mixed generations,
unbounded queued jobs, or starvation under continuous animation.

### Stage 5: Migrate content and remove the bespoke IBL path

Depends on Stage 4.

- [ ] Produce the default Studio as an ordinary HDR cube and configure normal
  scene/component defaults without the fixed DEnvironmentLighting load path.
- [ ] Migrate checked-in consumers, fixtures, and generation tools; remove
  `.iblbulk` path derivation, external Cook dependency declarations, serializer,
  obsolete DEnvironmentLighting type/data, and obsolete fixed-size contracts.
- [ ] Ensure the shared LUT no longer relies on the old environment payload.
- [ ] Verify clean-cache Cook packages source cubes and shaders correctly, and
  packaged Game starts and updates lighting without editor/build providers or
  the original imported HDR files.
- [ ] Record the chosen legacy compatibility behavior and migration instructions;
  do not retain an undocumented permanent dual loader.

Gate: production environment lighting uses ordinary assets and component state;
no active source/content/test path requires `.iblbulk`.

### Stage 6: Qualify the complete workflow and publish contracts

Depends on Stage 5.

- [ ] Validate a constant-radiance input against the documented irradiance
  convention and a directional HDR hotspot across specular roughness levels;
  use a bounded reference comparison to detect energy/orientation errors.
- [ ] Exercise animated procedural sky, manual/timed refresh, rapid source
  replacement, missing sources, multiple components/worlds, teardown during
  work, recoverable resource failure, and device-generation invalidation.
- [ ] Record GPU update-frame peak, steady-frame overhead, memory peak, and
  update latency against Stage 0 budgets. If they fail, reduce the supported
  quality envelope or explicitly revise scope before declaring completion.
- [ ] Verify editor import/reimport, undo/redo, scene reopening, clean-cache
  Cook, and packaged Game; record forward/deferred visual agreement and the
  unsupported-cloud-capture limitation.
- [ ] Update authoritative asset, lighting, sky, resource-lifecycle, and editor
  workflow documents; remove superseded active contract statements and close
  this plan only after every required gate has recorded evidence.

Gate: repeatable dynamic sky-to-lighting behavior is demonstrated within the
declared capability/performance envelope and all bespoke IBL dependencies are gone.

## Validation and Execution

Follow [agent build/run guidance](../Agents/BuildAndRun.md) before configuring or
running targets and [agent testing guidance](../Agents/Testing.md) before selecting
native tests. Select exact commands and test targets after Stage 0 identifies
affected modules; this plan does not claim existing tests cover the new feature.
Each implementation handoff records commands, outcomes, visual/performance
evidence, remaining limitations, and the next stage. Stage completion requires
its gate, not merely code compilation.

## Related Code and Contracts

- `Engine/Source/Runtime/Engine/Public/EnvironmentLighting/EnvironmentLighting.h`
- `Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLighting.cpp`
- `Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLightingBuild.cpp`
- `Engine/Source/Programs/EnvironmentLightingBake/Main.cpp`
- `Engine/Source/Developer/TextureBuild/Private/Texture/TextureCubeBuilder.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkyBoxComponent.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/EnvironmentLightingResources.cpp`
- [Cube textures](../Runtime/Rendering/CubeTextures.md)
- [Forward lighting](../Runtime/Rendering/ForwardLighting.md)
- [Package bulk data](../Runtime/Assets/BulkData.md)
- [Renderer frame preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Volumetric cloud scene contract](../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Geometry submission refactor](GeometrySubmissionRefactor.md)
