# RHI Graphics State and Bindings Plan

Summary: Complete the portable graphics pipeline, draw, vertex-instancing, and reflected binding contracts, then bound Vulkan descriptor and pipeline caching with persistent driver-cache support.

Last reviewed: 2026-08-10

Status: Archived
Completed: 2026-08-10

## Current Status

M0 capability/startup, M1 resource transitions, and M2 resource views/transfers
are complete. M2 supplies counted buffer and texture views, exact attachment
views, retained command payloads, and one Vulkan state authority. M3 can define
graphics bindings in terms of those resources without reintroducing raw native
handles or a second lifetime model.

The roadmap entry audit is complete against baseline `2b539627`. Graphics PSO
creation is bounded to nine consumer families: static mesh, sky box, post
process, editor grid, gizmo, overlay line, overlay icon, Mona ImGui, and Texture
Preview. The Renderer families obtain `FPipelineLayoutDesc` from merged shader
reflection; ImGui and Texture Preview use the same reflected layout path. Static
mesh already varies rasterizer, depth, blend, shader, vertex declaration, and
render-target identity, while the other families build a small fixed set of
PSOs around their pass layouts. No consumer constructs a separate handwritten
descriptor layout.

Reflection carries set, binding, type, and array size through
`FShaderParameterBinding`, `FPipelineLayoutDesc`, and Vulkan descriptor-set
layout creation. Submission is still scalar: `SetShaderParametersImpl` emits
one `FRHIShaderParameterResource` per metadata member and Vulkan always writes
array element zero with descriptor count one. Missing bindings and stale
bindings are not rejected against the active pipeline layout before draw.
`FRHIBindingSet`, `BindingSetDesc`, and `FBindingSetItem` are unused placeholders
and have no factory, command-list operation, or backend implementation.

The graphics baseline is likewise partial. Rasterization exposes fill/line,
none/back culling, and front face; depth exposes only less comparison; stencil
is absent; one blend state is replicated across all color attachments; vertex
declarations always lower streams as per-vertex; and the public command list
only records non-instanced indexed draws. Vulkan pipeline layout caching is
structural but unbounded, graphics pipelines are created per request without a
driver cache, and per-pipeline descriptor snapshots use a linear, frame-cleared
cache with no public bounds or hit/miss/creation statistics.

Stage 5 is complete. Stage 3 removed the unused binding-set surface, added fixed
C++ resource-array metadata and element-wise lowering, and made the active PSO
layout authoritative for update and pre-draw completeness validation. Pending
and cached snapshots retain canonical views/resources, and Vulkan writes exact
array elements. `RHICommandListTests` passed 54/54,
`RenderShaderContractTests` passed 27/27, and
`VulkanRHIIntegrationTests` passed 26/26 including a two-element hardware
descriptor-array draw on the windows-msvc-x64 Debug Editor profile. Stage 4
added collision-checked indexed descriptor snapshots with per-context
512-entry/8192-value LRU bounds, 256 structural-layout and 2048 graphics-PSO
cache bounds, cache-only eviction, one device-owned persisted Vulkan driver
cache, and resettable public cache statistics. `RHICommandListTests` passed
54/54, `RenderShaderContractTests` passed 27/27,
`VulkanRHIIntegrationTests` passed 26/26, and
`RendererResourceReloadVulkanTests` passed 1/1 on the same profile. Stage 5
audited the nine consumer families: every PSO request reaches canonical RHI
key construction and every resource-bearing draw uses reflected
`SetShaderParameters`; no renderer-local descriptor path remains. The hardware
consumer pass found and fixed raw vertex buffers whose stride is intentionally
owned by the vertex declaration. `SkyBoxVulkanIntegrationTests` passed 1/1,
`StaticMeshRenderPreparationVulkanTests` passed 1/1, `EditorRenderingTests`
passed 38/38, and `MaterialTests` passed 78/78. The reload workload also proved
an identical forced shader rebuild hits the PSO cache without another native
creation; graphics PSO and structural-layout occupancy stayed below capacity
with zero eviction. `VulkanRHIIntegrationTests` passed 26/26 with a combined
two-color-attachment, D24S8 depth/stencil draw and color readback in addition
to the descriptor-array, blend, wireframe, non-indexed, indexed, and instanced
coverage.

Stage 6 published the lasting
[Graphics State and Bindings](../../../Runtime/Rendering/GraphicsStateAndBindings.md)
contract and recorded the M3 handoff in the RHI/Vulkan roadmap. Final
qualification passed the complete native aggregate and full
`Win64-Debug-DurinEditor` `all` build. The Sandbox hidden-window runtime ran
three ticks and completed orderly shutdown with no assertion, error, or Vulkan
validation diagnostic. That smoke initially exposed a partial stage-update
lifetime gap for transient canonical texture views; Vulkan pending state now
constructs the replacement owner set before releasing the previous one, and
the Vulkan hardware test reproduces this update ordering explicitly.

## Frozen M3 Contract

Stage 0 selected the following exact public vocabulary. Rasterizer state carries
polygon mode, none/front/back culling, front face, depth-clamp enable, depth-bias
enable plus constant/clamp/slope factors, and line width. Multisample state
carries raster samples and alpha-to-coverage. Depth/stencil state carries depth
test/write and the standard eight compare operations, stencil enable, independent
front/back compare and fail/pass/depth-fail operations, compare/write masks, and
reference. Blend state is an eight-entry color-attachment array; every entry has
independent enable, color/alpha factors and operations, and RGBA write mask.
Vertex elements carry stream stride and `Vertex` or unit-divisor `Instance`
input rate. Draw submission uses `FRHIDrawArguments` and
`FRHIDrawIndexedArguments`, including count, instance count, first location,
signed base vertex where applicable, and first instance.

Canonical graphics identity contains shader content hashes and frequencies,
render-target compatibility, structural vertex elements, merged reflected
layout, topology, rasterizer, multisample, depth/stencil, and exactly the active
blend entries. Inactive attachments and disabled depth, stencil, blend, bias,
and non-line fields canonicalize to their published defaults before hashing.
Hash equality is never sufficient for reuse; structural equality confirms every
hit. Validation order is structural enums/counts, shader stages and reflected
interfaces, render-target/sample compatibility, vertex streams and attributes,
then capability/limit admission. Backend creation is attempted only after all
five classes pass.

One resolved descriptor value is identified by set, binding, array element, and
type. Fixed C++ arrays flatten in element order; scalars use element zero.
Updates are last-write-wins for the same complete location. Before draw, the
active layout is walked in set/binding/element order and every declared element
must have one non-null, type-compatible value. Dynamic offsets are validated and
ordered by that same walk but excluded from immutable descriptor identity.
Changing layouts selects a distinct snapshot and cannot inherit values from an
incompatible layout.

The unused `FRHIBindingSet`, `BindingSetDesc`, `FBindingSetItem`, resource-type
enumerator, and reference alias have no source, test, or documentation consumers
and are selected for removal. Compute will reuse the stage-neutral reflected
location and cache-statistics vocabulary; M4 may replace frame generations with
completion evidence without changing binding identity.

M3 uses conservative bounds above the inventoried nine-family baseline: 512
descriptor snapshots and 8,192 descriptor values per command context and frame
pool generation, 256 structural layouts per device, and 2,048 graphics PSOs per
device. All use deterministic LRU eviction, with monotonically increasing access
serial and canonical-key order as the tie-breaker; an entry with external owners
is skipped. Statistics accumulate for the device lifetime and can be explicitly
reset without clearing caches. The snapshot exposes capacity, occupancy, hits,
misses, native creations, evictions, pool expansions/allocations, and failed
candidates. These bounds provide more than 4x headroom over the audited fixed
consumer families and expected material/state variants; Stage 5 captures must
remain below 75% occupancy without sustained eviction.

The optional driver cache uses schema version 1 at
`Saved/Vulkan/PipelineCache-v1.bin`, capped at 16 MiB. Compatibility requires the
Vulkan header version, vendor ID, device ID, pipeline-cache UUID, and the M3 key
schema. Missing, stale, corrupt, oversized, read-only, or driver-rejected data is
one nonfatal diagnostic per load/save boundary. Publication uses the repository
atomic byte-file replacement contract; failure preserves the previous file.

## Goal

Make every graphics pipeline and draw required by current and near-term
renderer consumers expressible through one validated RHI contract, including
per-attachment state, vertex and instance streams, descriptor arrays, and
complete draw variants, while ensuring Vulkan pipeline and descriptor reuse is
bounded, observable, failure-atomic, and independent of raw-handle identity.

## Scope

- Complete baseline rasterizer, depth, stencil, multisample, per-color-
  attachment blend, color-mask, topology, and vertex-input state.
- Canonical immutable graphics PSO identity and backend-neutral validation
  against capabilities, render-target layouts, shader reflection, and vertex
  declarations.
- Per-vertex and per-instance vertex streams plus non-indexed, indexed, and
  instanced draw recording/replay.
- Explicit reflected binding snapshots with descriptor-array elements, exact
  resource/view types, retained lifetimes, deterministic replacement, and
  complete pre-draw validation.
- Removal of the unused counted `FRHIBindingSet` placeholder in favor of the
  command-list binding snapshot already consumed by current renderers.
- Vulkan lowering for the expanded state, vertex input, draws, and descriptor
  arrays.
- Bounded descriptor snapshot, structural-layout, and graphics-pipeline
  caches with queryable capacity, occupancy, hit, miss, creation, eviction,
  and failure counters.
- One optional persistent Vulkan driver pipeline cache with compatibility
  validation, atomic publication, retention bounds, and nonfatal fallback.
- Migration and qualification of all nine inventoried graphics consumers.

## Non-Goals

- Compute pipelines, dispatch, or compute binding submission; those remain in
  the Compute Shader Pipeline roadmap and will consume the shared layout and
  binding vocabulary established here.
- Bindless descriptors, descriptor indexing, update-after-bind, variable
  descriptor counts, or stable bindless handles.
- Dynamic rendering, mesh/task shaders, ray tracing, tessellation, geometry
  shaders, indirect draws, or multi-draw without a selected consumer.
- Dynamic fixed state beyond viewport and scissor; M3 keeps pipeline identity
  explicit and immutable unless measured PSO pressure justifies a later split.
- Queue-family ownership, asynchronous execution, timeline scheduling, or
  completion-token retirement.
- Replacing frame-index descriptor-pool reuse with GPU completion evidence;
  M4 owns that retirement boundary.
- Material sorting, renderer pass classification, vertex-factory redesign, or
  a render graph.
- Shipping Vulkan validation layers or making persistent-cache availability a
  startup requirement.

## Design Decisions and Invariants

### Graphics state and PSO identity

- `FGraphicsPipelineStateInitializer` remains the complete portable source for
  immutable graphics state. Its canonical identity includes shader content
  identity, render-target compatibility, structural vertex declaration and
  input rates, pipeline layout, topology, rasterizer, multisample, depth,
  front/back stencil, and every active color attachment's blend/write state.
- Rasterizer state adds front culling, depth clamp when capability-backed,
  depth bias enable/factors, and explicit line width admission. Unsupported
  non-solid fill, wide lines, or depth clamp reject before backend creation;
  they never silently lower to a different state.
- Depth comparison publishes the standard never/less/equal/less-or-equal/
  greater/not-equal/greater-or-equal/always vocabulary. Stencil publishes
  independent front/back compare and fail/pass/depth-fail operations, compare
  and write masks, and reference. Stencil is valid only with a stencil-capable
  attachment.
- Blend state is an exact array indexed by active color attachment. Each entry
  carries independent enable, color/alpha factors and operations, and write
  mask. Inactive entries are canonical zero/default state and do not affect
  identity. Logic operations, dual-source blending, and advanced blend modes
  remain unsupported until selected by a consumer.
- Multisample identity includes raster samples and alpha-to-coverage. Raster
  samples must equal the render-target layout; sample shading and alpha-to-one
  remain unsupported without a consumer and capability contract.
- PSO validation is deterministic and backend-neutral: structural enums and
  counts, shader stages/reflection, target compatibility, vertex attributes,
  feature/limit admission, then backend creation. Invalid work publishes no
  cache entry and leaves existing renderer resources unchanged.

### Vertex input and draw commands

- A vertex stream has one stride and one input rate (`Vertex` or `Instance`),
  with a positive instance divisor fixed to one for M3. All elements sharing a
  stream must agree on stride and rate; duplicate attributes, out-of-range
  streams, overlapping/overflowing elements, and shader-interface mismatches
  reject before PSO creation.
- Vertex and index buffer binding remains explicit recorded command state.
  Draw validation requires every declaration stream to be bound with compatible
  usage, stride, offset, and accessible range; indexed draws additionally
  require a supported index format.
- Public draw arguments cover non-indexed and indexed draws with vertex/index
  count, instance count, first vertex/index, signed base vertex, and first
  instance as applicable. Zero vertex/index or instance count is an explicit
  no-op; checked arithmetic rejects ranges that exceed bound buffers.
- Regular and immediate command lists own the same typed command payloads and
  replay identically inline or on the dedicated RHI thread. No draw command
  inserts transitions, waits for the GPU, or infers missing bindings.

### Reflected bindings and arrays

- M3 removes `FRHIBindingSet`, `BindingSetDesc`, `FBindingSetItem`, and their
  unused alias. The selected public model is a recorded binding snapshot built
  by `SetShaderParameters`, because it already preserves shader-stage metadata,
  command ownership, and incremental renderer call sites without introducing
  a second counted descriptor lifetime.
- Each resolved binding names set, binding, array element, type, and one
  counted resource/view plus any dynamic offset. Metadata arrays lower every
  element explicitly; a scalar is exactly an array of size one. Duplicate
  updates use deterministic last-write-wins semantics for the same complete
  location within one active PSO state.
- The active PSO layout is authoritative. Before a draw, every statically
  required array element must be present exactly once with matching type and
  valid range/usage; out-of-layout sets, bindings, array elements, null required
  resources, and wrong shader ownership reject at the RHI boundary before
  Vulkan descriptor allocation or update.
- Binding snapshots and descriptor-cache entries retain counted resources and
  views. Dynamic uniform offsets are submission data and are excluded from
  immutable descriptor identity while remaining ordered exactly as the Vulkan
  layout requires.
- Graphics stages may update their own reflected members independently, but
  overlapping locations must have layout-compatible type/count and resolve to
  one descriptor value. Binding a new PSO selects state keyed by the complete
  PSO identity and cannot inherit an incompatible prior layout.
- Bindless and partially-bound arrays remain out of scope. M3 arrays are fixed
  size, reflection-declared, fully populated, and bounded by M0 device limits.

### Cache ownership, bounds, and persistence

- Structural layout and PSO keys use canonical portable fields and stable
  shader/resource identities; Vulkan handles and object addresses are never
  persistent identity. Hashes are fast rejects only and equality confirms a
  hit.
- Descriptor snapshots are per command context, PSO layout, and current frame
  pool generation. They have a configured hard entry/descriptor budget and a
  deterministic least-recently-used eviction policy. Entries retain their
  resources until eviction or pool-generation reset. M4 may replace generation
  safety with completion tokens without changing public binding semantics.
- Structural descriptor-layout and graphics-PSO caches are per device and
  bounded. Cache lookup/creation is failure-atomic; a failed candidate changes
  neither occupancy nor the current renderer PSO. Entries held only by the
  cache are eligible for deterministic least-recently-used eviction.
- Each cache exposes capacity, occupancy, hits, misses, native creations,
  evictions, allocation/pool expansions where applicable, and failed
  candidates through one RHI/Vulkan statistics snapshot. Counters use defined
  reset/accumulation semantics and never require a GPU wait.
- One Vulkan `vk::PipelineCache` is owned by the device and supplied to every
  graphics pipeline creation. Its persisted blob lives under runtime writable
  `Saved/` state, is keyed by schema plus Vulkan pipeline-cache header/device
  compatibility, is size-bounded, and publishes with the repository atomic
  byte-file contract. Missing, stale, corrupt, oversized, read-only, or
  rejected data logs one owned diagnostic and falls back to an empty cache.
- Persistent cache save occurs only at an established flush/shutdown boundary
  after successful device use. Loading or saving the cache cannot make Vulkan
  startup or orderly shutdown fail.

### Failure, threading, and downstream boundaries

- Public validation happens before recording or native allocation whenever
  all required information is available. Replay-only state mismatches fail
  before the first descriptor write or draw and do not mutate cache identity.
- All Vulkan pipeline, descriptor allocation/update, and driver-cache access
  remains on the established RHI thread. Inline mode follows the same ordering
  and validation contract.
- M3 does not add a device-idle wait. Native pipeline and descriptor lifetime
  continues through the existing deferred/frame ownership until M4 introduces
  GPU-completion tokens.
- Compute later reuses the same reflected layout, array element, validation,
  and cache-stat vocabulary; M3 does not add graphics-only alternatives where
  the concept is stage-neutral.

## Current Foundations and Gaps

| Area | Foundation | M3 gap |
| --- | --- | --- |
| Consumer inventory | Nine bounded families create graphics PSOs; all use merged shader reflection for layouts. | Identity construction is duplicated at call sites and no conformance inventory proves every state combination. |
| Fixed state | Fill/line, none/back cull, front face, less-depth, one blend state, topology, viewport, and scissor work. | Front cull, full compares, stencil, depth bias, per-attachment blend/write masks, alpha-to-coverage, and capability admission are incomplete. |
| Vertex input | Structural declarations map streams and attributes; buffers bind explicitly. | All streams lower as per-vertex; stream consistency and shader-interface validation are incomplete. |
| Draw commands | Indexed, single-instance draw records and replays. | Non-indexed, instanced, first-instance, complete range validation, and representative consumers are absent. |
| Reflection | Set/binding/type/array size merge across shader stages into pipeline layouts. | C++ submission lowers one member to one scalar descriptor and does not flatten fixed arrays. |
| Binding state | Commands retain canonical views/resources; Vulkan accumulates pending values per PSO. | Required completeness, array elements, stale-state rejection, and layout validation occur too late or not at all. |
| Binding-set abstraction | `FRHIBindingSet` types exist. | They are unreachable placeholders and compete with the established reflected snapshot path. |
| Descriptor reuse | Frame-indexed pools are bounded in count and reset; per-PSO snapshots can hit within a frame. | Snapshot lookup is linear, entries lack explicit budgets/statistics, and cache resources/array identity are incomplete. |
| Pipeline reuse | Render-pass and descriptor layouts have structural maps; renderer owners retain created PSOs. | Graphics PSOs are recreated per request, structural maps are unbounded, and no driver pipeline cache is loaded or saved. |
| Failure handling | PSO/layout candidates publish complete-or-null and renderer creation slots preserve current resources. | Cache insertion/eviction, persistent blobs, array updates, and full-state rejection lack focused failure evidence. |

## Implementation Stages

### Stage 0: Freeze state, binding, identity, and cache contracts

- [x] Inventory all graphics PSO consumers, their state dimensions, reflected
  layouts, draw forms, and renderer ownership boundaries.
- [x] Freeze the exact enum/descriptor shapes for rasterizer, multisample,
  depth/stencil, per-attachment blend, vertex input rate, and draw arguments.
- [x] Freeze canonical PSO and descriptor-snapshot identity, shader-interface
  validation, required-binding completeness, array lowering, and rejection
  order.
- [x] Confirm removal of the unused counted binding-set placeholder at all
  source, test, and documentation call sites.
- [x] Select concrete cache entry/byte budgets, eviction rules, statistics
  reset semantics, persistent-cache path/key/version, and corruption/oversize
  behavior from representative workload captures.

#### Acceptance Gate

- Every public field has a selected consumer or baseline Vulkan requirement;
  the nine-family inventory is reproducible; cache bounds are finite and
  justified; compute reuse and M4 retirement handoffs are explicit; no
  unresolved state or binding alternative is presented as simultaneous work.

### Stage 1: Complete portable graphics state and PSO validation

- [x] Expand public fixed-state descriptors and canonical defaults, including
  front/back stencil and per-active-attachment blend/write state.
- [x] Add structural equality/hash helpers and full immutable PSO key creation
  without raw native-handle identity.
- [x] Validate shader stages/interfaces, render-target compatibility, samples,
  vertex declarations, state enums, feature/limit admission, and inactive
  canonical fields before backend creation.
- [x] Add focused RHI unit tests for valid identity differences, canonical
  equivalence, overflow/limit rejection, attachment mismatches, and stable
  diagnostic order.

#### Acceptance Gate

- Every selected graphics state has one portable representation and complete
  identity; unsupported state fails before Vulkan calls; equivalent
  initializers canonicalize to equal keys and distinct behavior cannot alias.

### Stage 2: Add vertex instancing and complete draw commands

- [x] Add vertex/instance input-rate semantics and validate stream stride,
  rate, divisor, attributes, formats, and shader interface.
- [x] Add recorded non-indexed and complete indexed draw arguments, including
  instance count and first instance, to context and regular/immediate lists.
- [x] Retain bound vertex/index buffers and validate usage, alignment, checked
  ranges, required streams, and render-pass/PSO ordering before replay.
- [x] Implement Vulkan vertex-input rates and exact `draw`/`drawIndexed`
  lowering with no hidden state or synchronization.
- [x] Add fake-context inline/threaded parity tests and hardware-backed output
  coverage for non-indexed, indexed, base-vertex, and instanced draws.

#### Acceptance Gate

- Vertex and instance streams drive expected geometry through public RHI
  commands in inline and threaded modes; invalid ranges or missing streams
  cannot issue a backend draw; existing indexed consumers remain unchanged.

### Stage 3: Make reflected bindings and descriptor arrays complete

- [x] Remove the unused `FRHIBindingSet` surface and extend resolved parameter
  payloads with explicit array elements and counted ownership.
- [x] Flatten fixed-size C++ resource arrays from metadata, preserve reflection
  count/type, and record deterministic full-location updates.
- [x] Add active-layout validation for set/binding/element/type, required
  completeness, dynamic-offset order/alignment, view usage, and cross-stage
  overlap before draw.
- [x] Implement Vulkan descriptor-array writes with stable backing storage and
  exact destination array elements/counts.
- [x] Add tests for full and partial updates, replacement, missing/null/wrong
  resources, out-of-range elements, layout switches, cross-stage overlap,
  retained lifetime, and inline/threaded parity.

#### Acceptance Gate

- Fixed reflection-declared arrays and scalars bind through one portable
  snapshot model; every required element is validated before allocation/write;
  stale or mismatched state cannot reach a draw; recorded resources survive
  caller release through replay.

### Stage 4: Bound Vulkan descriptor and pipeline caching

- [x] Replace linear descriptor-snapshot lookup with the selected bounded key,
  collision equality, deterministic eviction, retained-resource ownership, and
  pool-generation invalidation.
- [x] Bound structural layout and graphics-PSO caches using canonical keys and
  safe eviction of cache-only entries.
- [x] Route all graphics pipeline creation through one device-owned Vulkan
  driver cache; validate/load compatible blobs and atomically publish bounded
  data at the selected lifecycle boundary.
- [x] Expose the selected cache, pool, allocation, hit/miss, creation, eviction,
  failure, and persistence counters without a GPU wait.
- [x] Add failure injection and stress tests for hash collisions, capacity,
  eviction/recreation, pool expansion, resource replacement, corrupt/stale/
  oversized blobs, unwritable storage, and shutdown.

#### Acceptance Gate

- Repeated compatible work produces observable cache hits; adversarial unique
  work remains within every configured bound; failed candidates and invalid
  blobs publish no partial state; eviction and frame reset retain resources for
  the required current lifetime without a new idle wait.

### Stage 5: Migrate representative consumers and prove the baseline

- [x] Migrate all nine inventoried families to canonical PSO creation and
  complete binding validation without renderer-local descriptor escape hatches.
- [x] Add representative opaque, masked, translucent/blended, depth-only or
  depth/stencil, MRT, wireframe/line, non-indexed, indexed, instanced, and
  descriptor-array render cases.
- [x] Verify current static mesh, sky box, post process, editor assistance,
  ImGui, and Texture Preview resource replacement/failure behavior.
- [x] Capture cache occupancy/hit/miss/creation evidence from representative
  editor frames and confirm the Stage 0 budgets do not churn normal workloads.

#### Acceptance Gate

- Every inventoried consumer renders expected output; the roadmap's opaque,
  blended, depth/stencil, MRT, instanced, and array-binding cases pass; binding
  mismatches fail at the RHI boundary; normal workload captures remain below
  bounds with attributable cache behavior.

### Stage 6: Qualify M3 and publish lasting contracts

- [x] Run focused RHI, RenderCore, Renderer, editor-rendering, and Vulkan suites
  in inline and threaded modes where supported.
- [x] Run graphics, material, texture, MRT/MSAA/depth, viewport, failure,
  replacement, and shutdown regressions through DurinDevTool.
- [x] Run the required native aggregate, full Debug Editor `all` build, and
  validation-clean hidden-window runtime smoke because M3 is user-visible
  editor rendering work spanning multiple native targets.
- [x] Publish lasting graphics pipeline, draw, binding, and cache behavior under
  `Documentation/Runtime/Rendering/` and update direct related contracts.
- [x] Update the RHI/Vulkan roadmap with M3 completion evidence and the stable
  M5 handoff; do not activate M4 or M5 from this stage without its entry gate.

#### Acceptance Gate

- The complete baseline is portable, replay-equivalent, bounded, observable,
  validation-clean, and consumed by current graphics paths; lasting documents
  own shipped behavior; M5 conformance no longer depends on temporary Vulkan
  descriptor or pipeline details.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| State/unit | Canonical PSO identity; enum/count/limit checks; shader/target/vertex compatibility; full depth/stencil/blend/write-mask state; deterministic rejection. |
| Recording | Vertex/index binding ownership, non-indexed/indexed/instanced argument ranges, active-pass/PSO ordering, binding arrays, retained resources, and inline/threaded parity. |
| Reflection/bindings | Scalar/array layout merge, cross-stage overlap, full required occupancy, type/usage/dynamic-offset validation, replacement, layout switches, and no stale inheritance. |
| Vulkan graphics | Exact fixed-state and vertex-rate mapping, draw variants, descriptor-array writes, opaque/blended/depth-stencil/MRT/line/instanced output, and validation-clean execution. |
| Cache/failure | Collision equality, hard bounds, LRU eviction, frame generations, pool pressure, counters, candidate rollback, driver-cache compatibility/corruption/oversize/read-only fallback, and atomic save. |
| Consumers | Static mesh, sky box, post process, editor grid/gizmo/overlays, ImGui, Texture Preview, resource replacement, and recoverable creation failure. |
| Qualification | Focused suites, full native aggregate, full Debug Editor build, hidden-window runtime smoke, stable representative cache statistics, and orderly shutdown. |

All build, test, and runtime commands follow
[Build and Run](../../../Development/Build/BuildAndRun.md) and
[Native Tests](../../../Development/Build/NativeTests.md). Each stage handoff records
the exact profile, targets, filters, hardware/driver identity where relevant,
and observed outcomes.

## Definition of Done

- Every required fixed graphics state and active color attachment has exact,
  validated portable identity and Vulkan lowering.
- Per-vertex and per-instance streams plus non-indexed, indexed, and instanced
  draws record and replay identically inline or threaded.
- Reflection-declared scalar and fixed-array bindings retain exact views and
  fail missing, null, out-of-layout, wrong-type, or stale state before Vulkan
  descriptor mutation.
- The unused counted binding-set placeholder is removed; graphics and later
  compute use one reflected binding-location vocabulary.
- Descriptor, structural-layout, and graphics-PSO caches have hard bounds,
  collision-safe identity, owned lifetime, deterministic eviction, and
  queryable counters.
- The optional Vulkan driver cache validates compatibility, loads/saves
  atomically under a byte bound, and never gates startup or shutdown success.
- Representative opaque, blended, depth/stencil, MRT, line/wireframe,
  non-indexed, indexed, instanced, and descriptor-array paths produce expected
  validation-clean output.
- All nine inventoried consumers preserve transactional resource replacement
  and orderly teardown without raw Vulkan descriptor or pipeline escape paths.
- Lasting Runtime documentation replaces this plan as the shipped contract and
  the roadmap records the M3 handoff.

## Deferred Follow-ups

- GPU-completion-token descriptor/resource retirement, allocation placement,
  staging reuse, and memory pressure in `VulkanMemoryTransferAndRetirement`.
- Compute PSOs/dispatch and shared compute-stage binding consumption in the
  Compute Shader Pipeline roadmap.
- Bindless/partially-bound/update-after-bind descriptors after measured
  descriptor pressure satisfies C2's entry gate.
- Dynamic rendering or additional dynamic state after a selected consumer or
  PSO-cardinality measurement justifies the compatibility change.
- Indirect/multi-draw, non-unit instance divisors, tessellation, geometry,
  mesh/task, ray tracing, logic ops, dual-source blend, and advanced blend
  modes until concrete consumers select their full contracts.
- Pipeline-cache merging, offline prewarming, or shared cache distribution
  until representative creation-time evidence justifies added tooling.

## Related Documentation

- [RHI and Vulkan Backend Evolution Roadmap](../../../Roadmaps/RHIAndVulkanEvolution.md)
- [RHI Capabilities and Vulkan Startup](../../../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Resource Views and Transfers](../../../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Shader Cache](../../../Runtime/Rendering/ShaderCache.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Compute Shader Pipeline Roadmap](../../../Roadmaps/ComputeShaderPipeline.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHIShaderParameters.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/`
- `Engine/Source/Runtime/MonaImGui/Private/ImGuiRHIImpl.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/TexturePreview.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDescriptorSets.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDescriptorSets.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Tests/Native/RHITests/`
- `Engine/Tests/Native/RenderCoreTests/`
- `Engine/Tests/Native/EngineTests/`
- `Engine/Tests/Native/VulkanRHITests/`
