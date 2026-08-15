# Terrain Draw Submission Scalability Plan

Summary: Batch compatible finite-Terrain patches with direct hardware instancing so LOD triangle reduction also produces bounded CPU preparation, resource binding, and draw-submission cost.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

Completed on 2026-08-14. Terrain now retains all logical patch visibility,
LOD, adjacency, stitch, triangle, sorting, and overlay records while executing
eligible opaque, masked, wireframe, and directional-shadow work as stable
direct-instanced batches within one immutable proxy. The frozen chunk ceiling
is 256 instances and the versioned instance ABI is one tightly packed unsigned
`uint2` heightmap sample origin. Translucent Terrain remains scalar.

The RTX 3090 Win64 Debug Vulkan-validation qualification retained 256 logical
patches but reduced the homogeneous base pass from 256 hardware draws and 256
resource preparations to one batch, one 2,048-byte origin allocation, and one
draw. `ForceLOD0` retained 2,097,152 triangles and measured 12.94 ms CPU median
/ 13.39 ms p95 plus 0.243 ms GPU median / 0.244 ms p95. Automatic flat-far
retained exactly 512 triangles and measured 12.99 ms CPU. This exceeds the
relative 10x gate by more than 100x against the recorded same-host T3 median.

The Release qualification recorded 1.46 ms CPU median / 1.70 ms p95 and
0.243 ms GPU median, comfortably inside the 16.67 ms frame budget. Focused
Terrain, shader, RHI, Vulkan, and skeletal-storage tests, `fast-all`, full
Debug/Release Editor/Game builds, and 30-tick normal-exit hidden-window smokes
for all four configurations passed. Lasting ownership is published in Terrain
Rendering. The development-only unbatched switch is disabled by default and
production failure rejects a complete batch without scalar fallback.

## Goal

Make one fully visible 1025x1025 finite Terrain interactive in the editor by
turning compatible opaque, masked, wireframe, and directional-shadow patch
work into a bounded number of hardware-instanced indexed draws, while
preserving exact height sampling, selected LODs, stitch topology, material
output, visibility, diagnostics, and lifecycle behavior.

For the homogeneous 1025x1025 qualification fixtures, the required outcome is:

- 256 logical visible patches remain independently culled, LOD-selected,
  adjacency-resolved, counted, and available to the diagnostic overlay;
- all full-size patches sharing one resolved topology key submit as one batch
  per compatible base or shadow pass rather than 256 hardware draws;
- automatic flat-far output remains exactly 512 selected triangles and
  `ForceLOD0` remains exactly 2,097,152 selected triangles;
- measured Terrain CPU preparation/resource/submission work improves by at
  least 10x against the Stage 0 same-machine baseline;
- Win64 Debug with Vulkan validation records at most 150 ms median and 250 ms
  p95 Terrain CPU work for both homogeneous fixtures after warm-up;
- Win64 Release without validation sustains at least 30 editor viewport frames
  per second for the named single-Terrain interaction fixture, with Terrain
  CPU work at most 16.67 ms p95 and no GPU regression above the existing
  50 ms Scene Color ceiling.

If Stage 0 proves that the named CPU boundaries cannot isolate the Terrain
work or that the absolute gates are invalid on the qualification host, it must
record replacement boundaries and budgets with same-host evidence before
Stage 1 begins. The relative 10x improvement and logical-to-submitted work
conservation remain mandatory.

## Scope

- CPU timing and counters that separate logical patch preparation, batch
  construction, resource preparation, and hardware draw submission.
- A prepared Terrain batch representation for compatible opaque, masked,
  wireframe, and directional-shadow patches.
- Per-instance immutable patch sample origins consumed by the Terrain vertex
  shader through instance identity.
- Direct indexed instancing using the existing RHI `InstanceCount` and
  `FirstInstance` contract.
- Reuse of one Transform, Terrain-wide spacing/height, material, height
  texture, sampler set, pipeline, and topology binding per compatible batch.
- Deterministic batching, chunking, counters, failure diagnostics, render-mode
  behavior, and device/resource lifecycle.
- Structural, shader, Vulkan, image/parity, shadow, editor-interaction,
  performance, memory, Cook/Game, and shutdown validation at the existing
  1025x1025 finite component ceiling.

## Non-Goals

- Raising the 1025x1025 Terrain component ceiling, streaming height data,
  world partition, origin rebasing, or changing patch dimensions.
- GPU-selected LOD, compute culling, occlusion/HZB, mesh shaders, tessellation,
  indirect draw generation, or a general renderer-wide GPU-driven pipeline.
- Changing LOD errors, selection threshold, adjacency resolution, stitch-mask
  semantics, topology generation, height normalization, normals, UVs, bounds,
  winding, or collision.
- Batching translucent patches when doing so changes exact patch-level
  back-to-front order. Translucent Terrain retains the scalar draw path in
  this plan.
- Cross-component batching. The first implementation batches only within one
  immutable Terrain proxy so transform, height payload, spacing, height scale,
  material revision, and lifecycle identity remain naturally shared.
- Material-system redesign, bindless resources, descriptor indexing, pipeline
  cache redesign, or optimization of StaticMesh and SkeletalMesh submission.
- Hiding scalability regressions by weakening visibility, triangle, resource,
  draw, validation-layer, or image-correctness gates.

## Design Decisions and Invariants

### Logical patches remain authoritative

- Visibility, requested/resolved LOD, adjacency, stitch mask, topology key,
  triangle count, translucent distance, and overlay geometry remain computed
  per logical patch from the immutable proxy and submitted view.
- Batching is a lossless execution transform after those decisions. It cannot
  merge patch bounds, select a shared LOD, discard a culled neighbor from
  adjacency, or change the stable Y-major patch identity.
- Existing patch counters continue to describe logical work. New batch and
  hardware-draw counters describe submitted work; neither silently changes
  the meaning of the other.

### Direct instancing is the selected first optimization

- The batch key contains every fact that can change bound state or indexed
  geometry: pass, effective pipeline key, material representation/binding,
  immutable height payload identity, Terrain proxy identity, exact topology
  key `(CellCountX, CellCountY, LODStep, StitchMask)`, render mode, and shadow
  resource identity where applicable.
- One batch binds the existing shared topology and height texture, uploads a
  tightly packed array of patch sample origins, and issues one
  `DrawIndexed({IndexCount, InstanceCount, FirstIndex, VertexOffset,
  FirstInstance})`.
- Instance order is stable patch order within an exact batch key. Batch order
  follows the existing opaque/masked sort policy with a deterministic complete
  key; pointer or unordered-container iteration cannot affect output or
  diagnostics.
- Direct instancing is preferred over indexed indirect submission because the
  CPU already owns visibility/LOD decisions and the RHI already exposes the
  required instance fields. Indirect drawing may be reconsidered only after
  this plan if measured direct batch recording remains dominant.

### Per-instance data is minimal and bounded

- The first per-instance ABI contains only the unsigned heightmap sample
  origin required to replace `Terrain.SamplePatch.zw`. Width/height and
  spacing/height values remain batch uniforms because cross-component batching
  is out of scope.
- The shader indexes the bound instance-origin storage range using the
  canonical instance semantic. CPU and shader layouts use explicit fixed-width
  fields, alignment assertions, checked byte counts, and a versioned shader
  contract test.
- Batches are chunked deterministically at the minimum of the RHI instance
  count limit, dynamic storage-range limit, and a frozen Renderer ceiling.
  Stage 0 records those capabilities and selects the ceiling before any ABI is
  published. Chunking must preserve every logical patch exactly once.
- Per-view instance bytes, allocations, chunks, and peaks are counted. Failed
  allocation or invalid range rejects the affected batch atomically; it does
  not partially draw instances or silently fall back to hundreds of scalar
  production draws.

### Common state is prepared once per batch

- Transform, Terrain-wide values, material uniform data, texture/sampler
  resolution, height texture, topology, shader, and pipeline are resolved once
  for a compatible batch. Patch preparation stores stable identities and does
  not perform repeated resource-cache lookup for identical members.
- Resource creation remains transactional: an incomplete topology, height
  texture, shader, pipeline, sampler set, or instance buffer publishes no
  ready batch and does not disturb an existing complete cache entry.
- Recorded command lists retain all referenced buffers, textures, pipelines,
  and shader maps until execution completes. Device invalidation and Renderer
  shutdown release batch-owned transient and cached resources through existing
  ordered lifecycle boundaries.

### Pass correctness has priority over batching

- Opaque and Masked base passes, solid and wireframe rasterization, and opaque
  or masked directional-shadow passes may batch when their complete keys
  match.
- Translucent Terrain preserves the existing patch-level back-to-front order
  and scalar execution. Its logical and submitted counts remain equal.
- Directional-shadow views build their own batches from their own resolved
  LOD/stitch results; camera batches never supply shadow selection state.
- Mirrored transforms, two-sided materials, depth bias, opacity masks,
  directional-shadow resources, default-material fallbacks, and environment
  bindings remain part of the established pipeline/binding contracts.

### Diagnostics distinguish patches, batches, and commands

- `visible + culled = candidates`, requested/resolved histogram sums equal
  candidates, stitch-mask sums equal visible prepared logical patches, and
  selected triangles retain their current meanings.
- New counters include prepared batches, batch chunks, instance count/bytes,
  resource-attempted/successful/rejected batches, submitted hardware draws,
  and scalar translucent draws. For every executed view, successful batch
  instance totals plus successful scalar-patch totals reconcile with
  successfully submitted logical patches.
- A named unbatched comparison mode is test/development-only and disabled by
  default. It preserves the prior scalar path long enough to provide structural
  and image parity, but production resource failure never activates it.
- CPU measurements use explicit scopes around logical preparation, batching,
  resource preparation, and command recording; a whole-frame number alone is
  insufficient evidence for completion.

## Current Foundations and Gaps

| Area | Existing foundation | Gap this plan owns |
| --- | --- | --- |
| RHI draw contract | `FRHIDrawIndexedArguments` already carries `InstanceCount` and `FirstInstance`; Vulkan maps indexed direct draws | No Terrain instance semantic, instance-storage binding, or focused nonzero `FirstInstance` Terrain coverage |
| Dynamic data | Renderer/RHI already allocate dynamic uniform and storage ranges | Terrain allocates three dynamic uniforms per patch and has no bounded per-view instance upload |
| Preparation | Exact per-patch visibility, LOD, adjacency, material classification, topology key, sorting, and counters | Common proxy/material facts are repeated and prepared patches are not grouped by a complete compatibility key |
| Resources | Height textures, topologies, samplers, shaders, and pipelines are cached with ordered invalidation | Cache lookups and bindings occur per patch; readiness and failures are counted only as logical draws |
| Shader | Terrain vertex shader exactly loads R16 heights using a uniform sample origin | Sample origin is draw-uniform state; shader input has no instance identity or storage range |
| Passes | Opaque, Masked, Translucent, wireframe, and directional shadows are qualified | Opaque/Masked/shadow paths submit one command per patch; Translucent must remain ordered |
| Performance evidence | Frozen 1025x1025 Debug validation measurements expose CPU/GPU and draw/triangle counts | No phase timings, batch counters, Release interactive gate, or proof that hardware draws scale with compatibility groups |

## Implementation Stages

### Stage 0: Reproduce, partition, and freeze the batching contract

- [x] Add bounded CPU timing scopes and counters for logical patch selection,
  material classification/sorting, batch construction, resource preparation,
  dynamic allocation, and command recording without changing rendering.
- [x] Reproduce `ForceLOD0` and automatic flat-far 1025x1025 results on the
  named Debug Vulkan validation profile, including warm-up policy, median/p95,
  256 patch/draw conservation, triangle totals, CPU phase split, GPU time,
  retained resources, and dynamic-uniform allocation/byte counts.
- [x] Record a Win64 Release editor-interaction baseline with fixed viewport,
  camera path, resolution, material, light/shadow configuration, frame count,
  warm-up, and whole-frame plus Terrain CPU/GPU measurements.
- [x] Query and freeze the maximum instance count, dynamic storage range,
  alignment, and selected Renderer instances-per-chunk ceiling; prove checked
  arithmetic for the maximum visible logical-patch count admitted in one view.
- [x] Specify the exact batch key, stable batch ordering, minimal instance ABI,
  counter meanings, atomic rejection behavior, and unbatched comparison switch
  in tests before changing the shader or prepared representation.

#### Acceptance Gate

- Same-machine results reproduce the existing order of magnitude and show
  named CPU phase costs that explain the flat-far/`ForceLOD0` similarity.
- The frozen batch key distinguishes every state/resource/topology difference;
  fixtures demonstrate both valid merges and required splits.
- Instance/chunk ceilings and every timing gate are explicit, finite, and
  enforceable by automated tests or the qualification executable.

### Stage 1: Separate logical patch decisions from execution batches

- [x] Refactor Terrain preparation into immutable logical patch records plus
  per-proxy common render facts so material resolution, transform derivation,
  and shared identities are not recomputed for every patch.
- [x] Build deterministic opaque/masked/shadow batches from the frozen complete
  key after visibility, LOD adjacency, pass classification, and sorting.
- [x] Preserve scalar translucent records and exact back-to-front ordering.
- [x] Add deterministic chunking and checked logical-patch-to-instance
  mappings without allocating GPU resources during logical preparation.
- [x] Extend counters and invariants so candidates, logical patches, batches,
  chunks, instances, triangles, and eventual hardware draws reconcile.
- [x] Keep overlay generation sourced from logical patch records rather than
  batch bounds or instance ranges.

#### Acceptance Gate

- CPU-only tests cover one homogeneous 256-patch merge, every legal LOD/stitch
  split, partial edge patches, multiple Terrain proxies, materials/passes,
  mirrored transforms, culling, chunk boundaries, and deterministic replay.
- Logical LOD, adjacency, stitch masks, triangle totals, sorting, and overlays
  are byte-for-byte or value-for-value equal to the unbatched oracle.
- No batch key merges incompatible height payloads, transforms, materials,
  topology, render modes, shadow resources, or pass state.

### Stage 2: Add the Terrain instance ABI and direct-instanced execution

- [x] Extend the Terrain vertex input/shader contract with canonical instance
  identity and a read-only storage range of patch sample origins.
- [x] Allocate and populate bounded per-view instance data with explicit
  alignment, lifetime retention, checked offsets, and chunk-local ranges.
- [x] Bind Transform, Terrain-wide, material, height, sampler, pipeline, and
  topology state once per batch and issue indexed direct draws with the exact
  instance count and first-instance contract.
- [x] Prepare resources once per batch and report atomic batch readiness;
  retain patch-level rejection attribution in bounded diagnostics.
- [x] Apply the same mechanism to Opaque, Masked, wireframe, opaque-shadow, and
  masked-shadow paths while retaining scalar Translucent execution.
- [x] Extend RHI/Vulkan tests only where existing direct-instancing coverage is
  insufficient, especially nonzero `FirstInstance`, storage-range binding,
  command-list retention, and validation-layer cleanliness.

#### Acceptance Gate

- Vulkan renders distinct patch origins from one indexed instanced draw and
  matches the scalar structural/image oracle for exact heights, normals, UVs,
  materials, winding, mirrored transforms, all stitch masks, and partial
  boundaries.
- The homogeneous 1025x1025 base pass submits one hardware draw when all 256
  patches share one key; automatic flat-far remains 512 triangles and
  `ForceLOD0` remains 2,097,152 triangles.
- Mixed LOD/stitch fixtures submit exactly one draw per deterministic batch
  chunk, and all counters reconcile without validation-layer diagnostics.

### Stage 3: Integrate lifecycle, shadows, failures, and comparison coverage

- [x] Qualify directional-shadow cascades/views independently from camera
  batches, including different visibility, LOD, stitch masks, depth bias,
  opaque/masked materials, and resource identities.
- [x] Cover material replacement, heightmap changed/no-op/failed reimport,
  visibility and transform edits, component removal, level reload, device
  invalidation, shader refresh, Renderer shutdown, and recorded-command
  lifetime with batches in flight.
- [x] Exercise instance allocation failure, chunk-limit boundaries, invalid
  key/range/offset data, topology/height/shader/pipeline failure, and recovery;
  prove affected batches reject atomically with bounded diagnostics.
- [x] Run batched-versus-unbatched comparison across camera motion, frustum
  boundaries, LOD equality, every stitch mask, solid/wireframe, Opaque/Masked,
  default/custom materials, signed height scale, and mirrored transforms.
- [x] Confirm collision identity, editor exact picking, Cooked Runtime proxy
  creation, and source/DDC independence are unchanged.

#### Acceptance Gate

- Camera and every active shadow view conserve their own logical patches,
  batches, instances, triangles, resources, and submitted hardware draws.
- Failure/recovery never publishes a partial batch, reads retired instance
  storage, activates a hidden scalar fallback, or leaks cached/transient RHI
  resources.
- Required scalar comparison images and structural facts match within the
  existing Terrain rendering tolerances, and collision/picking revisions are
  unaffected.

### Stage 4: Qualify interactive performance and publish lasting contracts

- [x] Run the frozen Debug validation and Release editor-interaction profiles
  for `ForceLOD0`, automatic flat-far, mixed-LOD/stitch, partial visibility,
  masked material, wireframe, and directional-shadow cases.
- [x] Record median/p95 by CPU phase, whole-frame rate, GPU Scene Color/shadow
  time, candidates/visible/culled patches, batches/chunks/instances/draws,
  triangles, dynamic bytes/allocations, retained resources, and validation
  diagnostics.
- [x] Meet the absolute and relative performance gates in this plan without
  changing the fixture, visibility, LOD threshold, output resolution, shadow
  policy, or validation setting after baseline capture.
- [x] Run focused Terrain/RHI/Vulkan suites, required aggregate native tests,
  full Debug and Release Editor/Game builds, and normal-exit editor/game smoke
  tests according to repository build and test guidance.
- [x] Update Terrain Rendering with implemented batch ABI, counter semantics,
  pass exclusions, lifecycle/failure behavior, and qualification evidence;
  update the Heightfield Terrain roadmap only if milestone sequencing or
  deferred boundaries materially change.
- [x] Mark this plan completed only after all gates pass and lasting behavior
  is owned outside the plan.

#### Acceptance Gate

- Same-host Debug Terrain CPU work improves by at least 10x and is at most
  150 ms median / 250 ms p95 for both homogeneous 1025x1025 fixtures.
- The frozen Release interaction sustains at least 30 viewport FPS with
  Terrain CPU work at most 16.67 ms p95; GPU time remains within the inherited
  50 ms Scene Color ceiling and shows no statistically meaningful regression
  against the Stage 0 median beyond normal run variance.
- Homogeneous opaque/masked/shadow hardware draws equal compatible batch
  chunks rather than visible logical patches, all validation layers remain
  clean, and focused/aggregate/build/smoke gates pass.

## Validation Matrix

| Contract | Required coverage | Required outcome |
| --- | --- | --- |
| Logical equivalence | Scalar versus batched visibility, LOD, adjacency, stitch, triangles, overlay | Identical per-patch decisions and totals |
| Batch key | Same/different proxy, height, transform, material, pass, pipeline, topology, render mode, shadow resource | Merge only complete-key matches; deterministic order |
| Instance ABI | Zero/nonzero first instance, chunk offsets, maximum range, alignment, overflow | CPU/shader origin identity; no out-of-range access |
| Surface output | Exact/asymmetric heights, normals, UVs, signed scale, mirrored transform, all stitch masks, partial edges | Scalar/batched structural and image parity |
| Materials/passes | Default/custom, Opaque, Masked, wireframe, Translucent | Eligible passes batch; Translucent retains exact scalar order |
| Shadows | Independent camera/shadow visibility and LOD, opaque/masked depth, bias, cascades | Per-view batches and output remain independent and correct |
| Counters | Candidates, visible/culled, histograms, masks, logical patches, batches, chunks, instances, draws, triangles | Every stated conservation equation holds |
| Failures | Dynamic allocation, invalid range, cache ceilings, topology/height/shader/pipeline creation | Atomic batch rejection, bounded diagnostic, clean recovery |
| Lifecycle | Reimport, edits, removal, reload, invalidation, refresh, shutdown, recorded commands | No stale identity, use-after-free, leak, or mixed revision |
| Runtime isolation | Cooked Game without source/DDC; collision and editor picking | Rendering optimization does not alter asset/collision/picking contracts |
| Performance | Frozen Debug validation and Release interaction profiles | Absolute and relative CPU gates, draw collapse, inherited GPU ceiling |

## Definition of Done

- Eligible Terrain patches execute through deterministic direct-instanced
  batches with bounded per-view instance storage and complete key separation.
- Homogeneous 1025x1025 Terrain reduces 256 base-pass hardware draws to one
  compatible batch draw while preserving all 256 logical patch decisions and
  exact selected-triangle totals.
- Opaque, Masked, wireframe, and directional-shadow output match the scalar
  oracle; Translucent ordering remains unchanged.
- Counters and diagnostics expose logical patches, execution batches, chunks,
  instances, bytes, and hardware draws without redefining historical facts.
- Failure, invalidation, reimport, Cook/Game, shutdown, collision, and picking
  gates pass with no hidden scalar production fallback.
- Debug and Release performance gates pass on frozen profiles, lasting
  contracts are updated, required validation succeeds, and the task changes
  are committed with plan/stage provenance.

## Deferred Follow-ups

- Cross-component Terrain batching after measurements prove that component
  boundaries, transform/material data, and lifetime ownership can be encoded
  without weakening deterministic sorting or failure isolation.
- Translucent batching using an explicitly accepted order-independent or
  sub-patch sorting policy; ordinary alpha blending remains scalar meanwhile.
- Indexed indirect submission, compute/GPU culling, occlusion/HZB, or GPU LOD
  only if post-instancing profiles show command generation remains dominant.
- Terrain streaming/world partition remains conditional T5 work and is not
  activated by finite-component draw-submission cost alone.
- Generalizing the Terrain-specific instance ABI into a renderer-wide
  primitive batching framework after at least one additional primitive family
  demonstrates compatible ownership and performance requirements.

## Related Documentation

- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Terrain Patch LOD Plan](TerrainPatchLOD.md)
- [Terrain Editor Workflow Plan](TerrainEditorWorkflow.md)
- [Heightfield Terrain Roadmap](../../../Roadmaps/HeightfieldTerrain.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Scene Representation](../../../Runtime/Rendering/SceneRepresentation.md)
- [Agent Build And Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Terrain/TerrainVertexFactory.h`
- `Engine/Source/Runtime/RenderCore/Private/Terrain/TerrainVertexFactory.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Shaders/Slang/VertexFactory/TerrainVertexFactory.slang`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderPrimitiveTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderQualificationTests.cpp`
