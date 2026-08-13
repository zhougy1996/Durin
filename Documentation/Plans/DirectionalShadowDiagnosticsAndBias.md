# Directional Shadow Diagnostics And Bias Plan

Summary: Add causal directional-shadow diagnostics and a bounded texel-scale-aware bias policy without changing filter width or shadow-map topology.

Last reviewed: 2026-08-13

Status: Completed
Completed: 2026-08-13

## Current Status

Stages 0-5 and Q0 are complete. The checked-in qualification package now
retains the 13-image fixed-policy entry baseline, 13 selected-policy Lit
captures, seven causal diagnostic captures, and exact disabled/Unlit
diagnostic references. `DirectionalShadowBaselineVulkanTests` reproduces all
23 selected-policy captures through the production Vulkan path and verifies
exact hashes, paired valid/defective geometry, Masked/Opaque identity,
disabled/Unlit fallback, per-view diagnostic identity, and sub-texel motion.

The selected texel/orientation policy restores visible contact in the frozen
0.12-world-unit fixture while preserving the intentionally authored modular
gap. The valid and defective modular images remain distinct, Masked and Opaque
controls remain byte-identical, and the three tolerance-2 motion comparisons
change 22, 18, and 58 of 66,049 pixels, improving on the 26, 21, and 67-pixel
entry observations and remaining below the frozen 132-pixel limit.

StaticMesh/SplineMesh, SkeletalMesh, Terrain, Masked material, shader ABI,
resource reload/invalidation, view isolation, `fast-all`, full Debug `all`,
and an eight-second editor smoke all pass. On the RTX 3090 qualification fixture,
the selected Lit tier measures an 11,776 ns combined median increment and the
Classification view adds 64 ns of median Scene Color time; logical and backend
shadow storage remain 16,777,216 bytes. Fragment outcomes are owned by exact
diagnostic capture statistics rather than GPU atomic counters, preserving the
ordinary descriptor and synchronization contract; view counters report mode,
prepared bias fallback/clamp state, resources, draws, bytes, failures, and timings.

### Stage 1 frozen contract

The Stage 0 evidence selects the following single contract. All vectors and
distances below are finite world-space values unless stated otherwise.

| Identity | Encoded diagnostic evidence |
| --- | --- |
| `Lit` | Unchanged production color; diagnostic-disabled reference |
| `ShadowDepthCoverage` | Red means no stored caster coverage, grayscale is stored forward depth, green means covered |
| `ReceiverUnbiased` | Green/red is the raw `receiverDepth <= storedDepth` result before receiver terms; blue means invalid or outside |
| `ReceiverBiased` | Green/red is the comparison after receiver depth bias but before normal offset; blue means invalid or outside |
| `ReceiverNormalOffset` | Green/red is the final comparison after normal offset; blue means invalid or outside |
| `TexelGrid` | One-pixel cyan texel boundaries, yellow two-texel valid guard, magenta outside projection |
| `BiasContributions` | RGB encodes normalized raster, receiver, and normal-offset separation; white means the total detachment bound clamped |
| `Classification` | Green shadowed, white lit, blue outside/invalid, red missing caster coverage, yellow receiver-comparison failure, orange excessive displacement |

Diagnostic identity is `EDirectionalShadowDiagnosticMode : uint8` copied in
`FSceneViewSettings` and then into `FPreparedDirectionalShadowView`; it is not
mutable process state. Diagnostics execute in the existing Scene Color
fragment path after the shadow-depth pass. `Lit` has numeric value zero and
preserves ordinary descriptor identity, filtering, output, and timing within
noise. Unlit and disabled/failed shadows ignore non-Lit requests and remain
fully lit.

The reflected forward ABI grows from 400 to 448 bytes. The shadow block remains
16-byte aligned and grows from 80 to 128 bytes: matrix at offset 0, control
`float4` at 64 (`enabled`, mode, finite-fallback, total-clamped), texel/bias
`float4` at 80 (texel X/Y world units, receiver world units, normal-offset
world units), raster `float4` at 96 (constant, slope, clamp, normalized raster
separation), and light/bounds `float4` at 112 (world-space direction XYZ,
valid-region guard in texels). Production world position and the final mapped
production surface normal already reach the shared forward helper for every
StaticMesh, SplineMesh, SkeletalMesh, and Terrain variant, so no new vertex
factory or shadow-only normal path is introduced. C++ size/offset assertions
and Slang reflection tests own this packing.

The selected bounded bias equations use
`t = max(texelWorldSize.x, texelWorldSize.y)`, normalized production normal
`n`, normalized surface-to-light direction `l = -lightDirection`, and
`g = 1 - saturate(abs(dot(n,l)))`:

- raster constant `C = clamp(1.0 + 2.0*t, 1.0, 1.5)` depth-bias units;
- raster slope `S = clamp(1.25 + 1.0*t, 1.25, 2.0)` slope units;
- raster clamp `K = clamp(2.0 + 8.0*t, 2.0, 4.0)` depth-bias units;
- receiver world bias `R = clamp(t*(0.05 + 0.10*g), 0.0005, 0.02)` world units;
- normal offset `N = clamp(t*(0.20 + 0.55*g), 0.0, 0.10)` world units;
- total receiver displacement `R + N` is clamped to
  `min(0.75*t, 0.10)` world units, reducing `N` first.

Receiver world bias is converted by transforming both `p` and `p + l*R`
through the selected world-to-shadow matrix and using the finite signed depth
difference. Normal offset transforms `p + n*N`; positive offsets move the
receiver toward its production normal before the forward-depth `LessOrEqual`
comparison. This replaces the unexplained normalized `0.0005` only after
Stage 2 diagnostics pass. Non-finite texel, normal, light, matrix, or projected
values select the named safe fallback `R=0.0005` normalized depth, `N=0`, and
the frozen raster `1.25/1.75/4.0`; an invalid projection stays fully lit.

The frozen candidate matrix evaluates `t` at 0.03125, 0.0625, 0.125, and 0.25
world units; world geometry scales 0.25, 1, 16, and 128; absolute `dot(n,l)` at
1, 0.5, 0.1, and 0; Opaque and Masked; and every Stage 0 fixture. Candidate A
is the selected equation above. Candidate B multiplies `R` and `N` by 0.75;
candidate C multiplies them by 1.25 but retains identical clamps. A candidate
is rejected if it changes a deliberately defective classification, exceeds
one unexplained valid-seam pixel, three contact-detachment pixels, 0.25%
planar-acne interior coverage, two Masked/Opaque edge pixels, 132 changed
motion pixels at channel tolerance two, or 0.20 ms diagnostic-disabled median
GPU delta on the frozen RTX 3090 fixture. Diagnostic captures use channel
tolerance two; classification colors and counters are exact.

The production baseline is one 2048x2048 D32 map covering at most 256 world
units. It already computes per-view shadow texel world size, but uses fixed
raster constant/slope/clamp bias `1.25/1.75/4.0`, fixed receiver comparison
bias `0.0005`, and one hardware linearly filtered comparison sample. Existing
counters explain light selection, fitting, caster preparation, resources,
draws, target bytes, failures, and GPU timing; they do not expose depth
coverage, comparison failure, texel position, individual bias contributions,
or artifact classification.

The completed
[Directional Shadow Pipeline Plan](Archive/2026-08/DirectionalShadowPipeline.md)
and lasting [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
contract remain the baseline. This plan may change the production bias only
after diagnostic views distinguish missing caster depth, receiver-comparison
failure, excessive displacement, filter footprint, and authored geometry.

## Goal

Make directional-shadow artifacts causally diagnosable and replace the fixed
receiver-bias policy with one bounded policy derived from shadow texel world
size and receiver/light orientation.

After this plan, valid coincident modular geometry has no unexplained light
seam in the qualified fixtures; intentionally defective geometry is identified
rather than hidden; acne and shadow detachment remain inside frozen image and
motion tolerances across supported geometry families, scales, light angles,
and views. Diagnostics remain per-view and development-only, counters
reconcile, disabled diagnostics preserve production output, and every failure
retains the existing complete fully lit fallback.

## Scope

- Deterministic valid and intentionally defective fixtures for acne,
  peter-panning, modular-floor seams, grazing angles, masked coverage, texel
  motion, and geometry-family parity.
- A named artifact taxonomy that separates shadow-depth coverage, receiver
  comparison, bias displacement, the existing filter footprint, and authored
  geometry.
- Development-only visual modes for shadow depth, caster coverage, receiver
  comparison, texel grid and valid bounds, individual bias contributions, and
  final classification.
- Per-view immutable diagnostic selection and the minimum Renderer/Slang ABI
  needed to carry receiver normal, texel scale, bias terms, and diagnostic
  identity through shared forward lighting.
- Separate bounded raster constant/slope/clamp, receiver-comparison, and normal
  offset responsibilities derived from stable physical inputs.
- StaticMesh, SplineMesh, SkeletalMesh, and Terrain Opaque/Masked parity,
  including mirrored, two-sided, deformed, and intentionally defective
  controls.
- Main, auxiliary, preview, present, offscreen, fixed-aspect, post-process,
  and editor-assistance isolation, plus retry, reload, invalidation, release,
  and shutdown behavior.
- Focused CPU, shader, RHI, Vulkan image, motion, counter, failure, build, and
  editor-smoke qualification.

## Non-Goals

- Wider PCF kernels, filter quality tiers, stochastic filtering, temporal
  history, blocker search, PCSS, or variable penumbrae.
- Cascades, texture arrays, atlases, split selection, cascade blending, or a
  larger single-map production tier.
- Contact shadows, point/spot shadows, persistent caching, virtual shadows,
  moment representations, or ray tracing.
- Repairing source meshes or concealing gaps, T-junctions, inconsistent
  deformation, or broken material coverage with unbounded bias.
- Changing caster visibility, selected-light ownership, material participation,
  vertex factories, deformation, or the fully lit failure contract.
- A public graphics-settings system or lasting user-facing diagnostic controls.
- Replacing the existing one-sample-linear filter; Q0 may diagnose its
  footprint but does not widen it.

## Design Decisions and Invariants

### Entry evidence precedes implementation

- Stage 0 records ordinary Lit-output baseline captures before diagnostic or
  bias code changes. Each fixture freezes scene facts, camera, light,
  transforms, map tier, current bias, capture path, and validity classification.
- Valid coincident geometry and intentionally defective controls are paired.
  A candidate cannot pass by making both appear continuously shadowed.
- Static captures alone are insufficient. At least one fixture records
  sub-texel camera translation, rotation, and light-direction motion with a
  fixed frame sequence and comparison rule.
- Stage 1 begins only after the Stage 0 acceptance gate is reviewed. Until
  then, the fixed production bias and output remain unchanged.

### Diagnostics establish cause

- Diagnostic identity is immutable per prepared view and cannot be stored as
  process-global mutable render state.
- The minimum required modes are final Lit output, raw shadow depth/coverage,
  receiver comparison before bias, receiver comparison after each selected
  bias contribution, texel grid/valid region, and classified result.
- Missing caster depth, an out-of-range receiver, a failed comparison,
  excessive displacement, existing linear-filter footprint, and authored
  geometry are distinct outcomes with named counters or capture evidence.
- Diagnostics may use development/editor controls needed for deterministic
  selection, but ordinary rendering with diagnostics disabled must preserve
  descriptor identity, shader output, timings within noise, and the existing
  fully lit fallback.

### Bias terms have separate bounded responsibilities

- Raster constant, slope, and clamp bias own caster-side depth separation.
  Receiver-comparison bias owns bounded comparison precision. Normal offset
  owns bounded receiver displacement along the production surface normal.
- Candidate equations use per-view shadow texel world size and finite
  surface/light orientation. World-space displacement is transformed through
  the selected world-to-shadow mapping instead of being treated as an
  unexplained normalized-depth constant.
- Stage 1 freezes the exact equation, units, sign convention, grazing-angle
  response, minima, maxima, and defaults before Stage 3 changes production
  behavior. Non-finite inputs choose a named bounded fallback or disable the
  shadow for that view; they never publish an unbounded term.
- The total effective separation is observable. Individual terms cannot
  silently accumulate beyond the frozen detachment limit.
- Values are evaluated at multiple world scales and shadow fitting extents so
  a candidate tuned to one fixture cannot become the default by familiarity.

### Ownership, participation, and failure remain shared

- `FDirectionalShadowRenderer` remains the private feature owner. This plan
  extends one prepared per-view shadow value and shared forward helper; it does
  not create a parallel shadow renderer or geometry/material pipeline.
- Opaque/Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain continue to
  use authoritative scene collections, material snapshots, culling, winding,
  and existing vertex deformation.
- Normal data added to the forward helper comes from each production base-pass
  surface contract and is not reconstructed from a shadow-only geometry copy.
- Every mode and candidate keeps complete texture/sampler bindings. Missing or
  failed resources, invalid math, disabled shadows, and Unlit views remain
  fully lit without stale state or a whole-device wait.
- Sequential views consume only their own prepared diagnostic and bias state.
  Resize, retry, reload, invalidation, scene release, and shutdown introduce no
  component/asset read, command-list flush, or `WaitIdle`.

## Current Foundations and Gaps

| Area | Existing foundation | Q0 gap |
| --- | --- | --- |
| View math | Per-view receiver/caster fitting, world-to-shadow matrix, texel world size, guard, and XY stabilization | Texel scale is not used by the production bias policy; no texel-grid diagnostic exists |
| Raster bias | D32 forward-depth PSOs use fixed constant/slope/clamp values across geometry families | Contributions are not reported or varied by stable scale/orientation evidence |
| Receiver sampling | One shared Slang helper validates projection and subtracts fixed receiver bias before `SampleCmpLevelZero` | No receiver normal, normal offset, pre/post-bias comparison, or classification output |
| Filtering | One linear `LessOrEqual` comparison sample with opaque-white border | Footprint can be observed in Q0 but wider PCF belongs to Q1 |
| Geometry | Opaque/Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain reuse production materials and deformation | No valid/invalid modular-seam matrix or diagnostic parity proof |
| Counters | Selection, fitting, casters, resources, draws, triangles, bytes, failures, and timings are per-view | No diagnostic mode, sample/comparison outcome, bias contribution, or artifact-class counters |
| Lifetime | Retry, shader/device invalidation, sequential views, scene release, and shutdown are qualified | Diagnostic resources and state do not yet exist and must preserve those contracts |
| Evidence | Baseline single-map performance and Vulkan images are qualified | Named Q0 entry fixtures, frozen image tolerances, and motion comparisons are absent |

## Implementation Stages

### Stage 0: Record entry fixtures and freeze the baseline

- [x] Create deterministic fixtures for planar acne, sloped acne, detached
  contact/peter-panning, valid coincident modular-floor seams, intentionally
  defective gaps or T-junctions, grazing-angle failure, Masked coverage, and
  sub-texel camera/light motion.
- [x] Pair every valid seam fixture with an intentionally defective control and
  record the owning geometry/material facts that justify the classification.
- [x] Cover representative StaticMesh, SplineMesh, SkeletalMesh, and Terrain
  paths; freeze the smaller entry subset and the full Stage 4 parity matrix.
- [x] Freeze camera projection and transform, light direction, geometry scale,
  material mode, map resolution/distance, current raster and receiver bias,
  output size, backend, warm-up, frame sequence, and capture naming for each
  fixture.
- [x] Record baseline Lit captures and motion sequences without diagnostic or
  bias changes, including current acne, seam, detachment, masked-edge, and
  shimmer observations.
- [x] Freeze measurement rules and tolerances for shadowed/lit pixel regions,
  maximum unexplained seam width, maximum contact detachment, acne coverage,
  and motion-frame difference; keep deliberately defective controls outside
  valid-geometry success metrics.
- [x] Record baseline test/build status and the current fully lit disabled/
  failed-shadow reference before implementation changes output.

#### Acceptance Gate

- Every required artifact is reproducible from named scene, view, light,
  quality, backend, and capture facts. Valid and defective seam controls are
  distinguishable, static and motion baselines are recorded, tolerances are
  reviewable, and no diagnostic or bias implementation has changed the
  production result. Passing this gate activates Stage 1 and Q0 implementation.

### Stage 1: Freeze the diagnostic taxonomy and bias contract

- [x] Define the exact artifact classification states and the evidence that
  separates missing depth coverage, invalid/outside receiver coordinates,
  unbiased/biased comparison failure, excessive displacement, current filter
  footprint, and authored geometry.
- [x] Freeze diagnostic mode identity, per-view ownership, render ordering,
  output encoding, capture interpretation, counters, and disabled behavior.
- [x] Inventory the minimum C++/Slang ABI changes for production surface normal,
  light orientation, texel scale, individual bias terms, and diagnostic output;
  assert alignment, field offsets, reflection bindings, and uniform limits.
- [x] Specify candidate raster constant/slope/clamp, receiver-comparison, and
  normal-offset equations with exact units, comparison sign, texel-scale input,
  grazing response, clamping, non-finite fallback, and total detachment bound.
- [x] Freeze candidate values and a comparison matrix spanning multiple fitted
  texel sizes, geometry scales, light angles, material modes, and the Stage 0
  fixtures before changing the default.
- [x] Freeze diagnostic image, motion, counter, shader, GPU-time, and failure
  acceptance rules; establish that diagnostic-disabled measurements compare
  against the Stage 0 baseline.

#### Acceptance Gate

- One unambiguous taxonomy, ABI, equation family, bounded candidate set,
  fixture matrix, and measurement contract exists. No two incompatible units,
  normal-offset spaces, comparison signs, or failure policies remain open, and
  the proposed diagnostic outputs can establish cause for every Stage 0
  artifact.

### Stage 2: Implement causal per-view diagnostics

- [x] Add immutable prepared diagnostic identity and values to the existing
  directional-shadow view without process-global camera or light state.
- [x] Extend the shared forward helper and production base-pass bindings with
  the minimum data needed to render raw coverage/depth, receiver comparison,
  texel grid/valid bounds, per-contribution bias, and classified output.
- [x] Preserve one shared equation across StaticMesh/SplineMesh, SkeletalMesh,
  and Terrain receivers; prove normal and material/deformation inputs match the
  corresponding production base pass.
- [x] Add per-view counters for selected diagnostic mode and prepared bias
  clamps/fallbacks; record comparison, out-of-range, and classification
  fragment outcomes as exact capture statistics without GPU atomics.
- [x] Capture every Stage 0 fixture in selected Lit output and one shared
  contact fixture in every diagnostic mode; demonstrate the outputs distinguish
  valid coverage, receiver/bias failure, and authored defects before selecting
  the production policy.
- [x] Verify diagnostics disabled, Unlit, absent/disabled light, resource
  failure, and sequential shadow-enabled/disabled views retain complete
  bindings and exact established fallback behavior.

#### Acceptance Gate

- Diagnostic images and counters causally classify every Stage 0 artifact,
  intentionally defective controls cannot pass as valid seams, geometry
  families agree on shared inputs, sequential views remain isolated, and
  disabling diagnostics matches the frozen baseline.

### Stage 3: Implement the bounded texel-scale-aware bias policy

- [x] Implement the Stage 1 raster, receiver-comparison, and normal-offset
  terms as separately named values with explicit clamps and observable totals.
- [x] Convert world/texel-scale receiver displacement through the selected
  view's world-to-shadow mapping and preserve the frozen forward-depth
  comparison direction and fully lit outside behavior.
- [x] Supply production surface normal and selected directional-light
  orientation through the shared forward path for StaticMesh, SplineMesh,
  SkeletalMesh, and Terrain without a shadow-only material or vertex pipeline.
- [x] Evaluate the selected candidate against the complete Stage 0 capture
  matrix and bracket alternatives against the frozen bounds; reject values that
  hide defective geometry, exceed contact-detachment tolerances, introduce
  grazing-angle instability, or regress Masked coverage.
- [x] Select and record one default bounded policy only after fixed-camera and
  motion evidence passes against the fixed-bias baseline; retain a named safe
  fallback for invalid inputs and resource failure.
- [x] Add focused CPU/shader tests for units, sign, orientation extremes,
  scale changes, clamps, non-finite inputs, matrix conversion, and exact
  C++/Slang packing.

#### Acceptance Gate

- The selected policy is derived from stable texel/orientation inputs, every
  contribution and clamp is diagnosable, valid fixtures meet frozen acne/seam/
  detachment tolerances, defective controls remain classified, motion remains
  stable, and no failure can publish an unbounded or stale bias value.

### Stage 4: Qualify geometry, view, counter, and lifetime parity

- [x] Complete the horizontal, vertical, sloped, grazing, thin, mirrored,
  two-sided, Masked, skinned, spline-deformed, Terrain, modular-valid, and
  modular-defective fixture matrix at representative scales and light angles.
- [x] Reconcile submitted, hidden, culled, invalid-bounds, prepared, resource,
  draw, triangle, clamp, failure, and retry counters without reusing camera
  visibility for caster visibility; reconcile comparison/classification through
  exact diagnostic capture statistics.
- [x] Render main, auxiliary, preview, present, offscreen, fixed-aspect,
  post-process, and editor-assistance views sequentially; pair different
  diagnostic modes and bias conditions in immutable-preparation coverage and
  prove no cross-view state reuse.
- [x] Exercise camera/light transform changes, animation, material replacement,
  terrain revision, resize, enable toggles, selected-light changes, scene
  release, and renderer shutdown.
- [x] Exercise target/view/sampler/shader/PSO/material/palette failure, manual
  retry, shader reload, and device invalidation; verify complete fully lit
  fallback, later recovery, and balanced recorded-resource lifetime.
- [x] Record diagnostic-disabled GPU and memory deltas against the completed
  single-map baseline and diagnostic-enabled development cost separately.

#### Acceptance Gate

- All geometry families and supported views satisfy the selected policy and
  taxonomy, counters reconcile, diagnostic/bias state is view-local, recovery
  never samples stale state, ordinary rendering introduces no whole-device
  wait, and logical/backend bytes and GPU-time observations are complete.

### Stage 5: Qualify Q0 and publish lasting contracts

- [x] Run focused shadow math, forward-lighting ABI/helper, scene/view,
  material/deformation, counter, failure, RHI, and Vulkan integration coverage
  selected according to the repository testing guidance.
- [x] Run StaticMesh/SplineMesh, SkeletalMesh, Terrain, Masked material,
  multi-view, resource invalidation/reload, and Stage 0 image/motion
  qualification with Vulkan validation clean.
- [x] Run the required affected targets, `fast-all`, full Debug `all` build,
  any affected Shipping qualification, and a representative editor smoke under
  the repository build/run guidance.
- [x] Record final fixture results, selected bias equation and constants,
  comparison tolerances, counter reconciliation, disabled-output parity,
  logical/backend bytes, GPU-time deltas, failures/retries, and target hardware.
- [x] Publish lasting diagnostic, bias, ownership, ABI, view, failure, and
  measurement behavior in
  [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md).
- [x] Update the Shadow System Evolution roadmap with Q0 completion and the Q1
  entry evidence; do not activate wider PCF until coverage, geometry, and bias
  defects have been separated by Q0 results.

#### Acceptance Gate

- The roadmap Q0 exit gate passes: valid coincident modular geometry has no
  unexplained light seam; defective geometry remains identified; acne and
  detachment meet frozen tolerances across required families, scales, angles,
  motion, and views; diagnostics and counters reconcile; disabled/failed
  shadows retain the established output; required builds, tests, Vulkan
  validation, captures, target evidence, documentation, and editor smoke pass.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Entry reproducibility | Named deterministic scene/view/light/material/quality/backend facts reproduce every Stage 0 static and motion artifact |
| Artifact classification | Raw depth/coverage, receiver comparison, bias contribution, texel-grid, and final modes distinguish valid coverage, bias defects, current filter footprint, and authored geometry |
| Bias math | CPU and shader tests prove units, sign, texel-scale conversion, orientation response, minima/maxima, total bound, and non-finite fallback |
| Shader ABI | C++ offsets/size and Slang reflection agree; all base-pass families bind complete normal, shadow texture, sampler, and diagnostic values |
| Valid/defective seams | Valid coincident modular boundaries meet frozen seam tolerance while gaps and T-junction controls remain classified rather than hidden |
| Geometry/material parity | Opaque/Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain retain matching position, normal, mask, winding, two-sided, and deformation meaning |
| Motion stability | Fixed frame sequences for sub-texel translation, rotation, light changes, animation, and resize remain within frozen difference and popping tolerances |
| View isolation | Main, auxiliary, preview, present, offscreen, fixed-aspect, post-process, and editor-assistance sequences consume only their prepared diagnostic/bias state |
| Counters | Caster, draw, comparison, clamp, classification, resource, failure, retry, byte, and timing outcomes reconcile per view |
| Failure/lifetime | Partial resource/shader/draw failure, retry, reload, invalidation, release, and shutdown remain fully lit, recoverable, balanced, and free of stale bindings or whole-device waits |
| Performance | Diagnostic-disabled GPU/memory deltas are measured against the completed single-map tier; development diagnostic cost is reported separately |
| Handoff | Focused targets, bounded integration tests, `fast-all`, required builds, Vulkan validation, image/motion evidence, target measurements, and editor smoke pass before Q0 completion |

## Definition of Done

- Stage 0 entry fixtures and measurement rules are deterministic, named, and
  preserved with paired valid and intentionally defective controls.
- Diagnostic modes and counters causally distinguish depth coverage, receiver
  comparison, bias displacement, existing filter footprint, and authored
  geometry without changing diagnostic-disabled output.
- The production default uses one documented, bounded texel-scale-aware bias
  policy with separately observable raster, receiver, and normal-offset terms.
- Valid modular seams have no unexplained light leak, defective seams remain
  classified, and acne/detachment/motion tolerances pass at representative
  scales and light angles.
- StaticMesh, SplineMesh, SkeletalMesh, and Terrain retain shared Opaque/Masked
  material, normal, winding, and deformation semantics.
- Supported views, failure, retry, reload, invalidation, release, and shutdown
  preserve per-view isolation, complete bindings, fully lit fallback, and no
  whole-device wait.
- Required focused, integration, Vulkan image/motion, counter, performance,
  build, and editor-smoke qualification passes.
- Lasting behavior is published under Runtime Rendering, Q0 is marked complete
  in the roadmap, and Q1 remains gated on the recorded Q0 evidence.

## Deferred Follow-ups

- Deterministic 3x3 tent PCF and low/medium/high filter tiers belong to
  `DirectionalShadowPCFQualityTiers` after Q0 proves remaining edge artifacts
  are filtering problems.
- Three-cascade fitting, resource layout, selection, blending, and per-cascade
  bias/filter scale belong to `CascadedDirectionalShadows` after Q1.
- Screen-space contact shadows activate only if Q0/Q2 evidence shows necessary
  bounded bias still loses short-range contact after geometry and resolution
  causes are ruled out.
- Mesh repair and build validation own gaps, T-junctions, deformation mismatch,
  and invalid material coverage identified by Q0 diagnostics.
- Public diagnostic/settings UX waits for qualified production quality tiers;
  Q0 controls remain internal development/editor mechanisms.

## Related Documentation

- [Shadow System Evolution Roadmap](../Roadmaps/ShadowSystemEvolution.md)
- [Directional Shadow Pipeline Plan](Archive/2026-08/DirectionalShadowPipeline.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [RHI Diagnostics and Conformance](../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Shaders/Slang/Lighting/DirectionalShadow.slang`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceInvalidationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
