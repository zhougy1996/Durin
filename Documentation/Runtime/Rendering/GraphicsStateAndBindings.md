# Graphics State and Bindings

Summary: Define portable graphics-pipeline identity, draw submission, reflected bindings, and bounded Vulkan cache behavior.

Modules: RHI, RenderCore, VulkanRHI, Renderer

## Graphics Pipeline Contract

`FGraphicsPipelineStateInitializer` is the complete portable graphics state.
It combines shader content identity, render-target compatibility, structural
vertex input, reflected pipeline layout, primitive topology, rasterizer,
multisample, depth/stencil, and one blend descriptor per active color
attachment. `BuildGraphicsPipelineStateKey` validates and canonicalizes this
state before backend creation. Disabled features and inactive attachments are
reduced to canonical defaults, so equivalent behavior has equal identity and
different behavior cannot alias.

Rasterizer state includes polygon mode, front/back/none culling, winding,
depth clamp, depth bias, and line width. Multisample state includes sample
count and alpha-to-coverage. Depth/stencil state includes the standard compare
operations, independent front/back stencil operations and masks, and the
stencil reference. Each of the eight possible color attachments has
independent color/alpha blending and RGBA write masks.

Validation rejects invalid enums, attachment/sample mismatches, unsupported
device features, inconsistent vertex streams, and shader/layout mismatches
before a Vulkan pipeline is created. A nullable creation result is either one
complete PSO or null; renderer creation slots retain their previous complete
payload when a replacement fails.

## Vertex Input and Draws

Every `FVertexElement` declares a stream stride and `Vertex` or unit-divisor
`Instance` input rate. A bound vertex buffer must carry vertex usage. A
nonzero buffer descriptor stride must equal the declaration stride; stride
zero means the declaration owns the interpretation. Offsets, required stream
presence, index alignment/type, and checked vertex, index, and instance ranges
are validated before a backend draw.

`FRHIDrawArguments` and `FRHIDrawIndexedArguments` carry exact vertex/index
count, instance count, first vertex/index, signed base vertex, and first
instance. Regular and immediate command lists retain referenced buffers and
replay the same typed arguments. Vulkan lowers these records directly to
`draw` and `drawIndexed` without hidden count or instance defaults.

## Reflected Binding Snapshots

Merged shader reflection is authoritative for every graphics PSO layout.
Typed C++ parameter metadata supports scalars and fixed `std::array` resource
members. RenderCore flattens arrays into deterministic
`FRHIShaderParameterResource` records containing set, binding, and exact array
element. Recorded commands retain canonical buffer/texture views, samplers,
and their parent resources through replay.

Updates must belong to a shader in the active PSO and must match reflected
set, binding, array extent, and type. Before descriptor allocation or the
first write, draw preparation requires every reflected array element and
scalar to be populated, validates view usage and ranges, and checks dynamic
uniform alignment. Dynamic offsets are ordered submission data and do not
participate in immutable descriptor identity. Binding a different PSO selects
state for that complete PSO identity, so an incompatible layout cannot inherit
stale values.

`FRHIBindingSet` is not part of the public model. Graphics and future compute
work share this reflected location and snapshot vocabulary.

## Bounded Vulkan Caches

Descriptor snapshots are scoped to one command context, PSO, and current
frame-pool generation. Hash lookup is a fast reject and complete resource
equality confirms a hit. Each context is bounded to 512 snapshots and 8192
descriptor values. Deterministic least-recently-used eviction releases retained
resources; beginning the corresponding frame generation clears snapshots
before another generation uses them. Native descriptor pools belong to bounded
completion-token batches and reset only after their maximum use token completes.

Each device also owns bounded structural-layout and graphics-PSO caches:

- structural layouts: 256 entries;
- graphics PSOs: 2048 entries.

Both use canonical structural keys and least-recently-used eviction. Only an
entry held solely by its cache can be evicted. Candidate creation publishes a
complete cache entry or fails without replacing the renderer's current PSO.

`RHIGetGraphicsCacheStatistics` returns capacity, live occupancy, hits,
misses, native creations, evictions, failed candidates, descriptor allocation
and pool-expansion counts, and persistence counters without waiting for the
GPU. Counters accumulate for the device lifetime.
`RHIResetGraphicsCacheStatistics` clears accumulated counters while preserving
capacities and live occupancy.

## Vulkan Driver Pipeline Cache

One device-owned `vk::PipelineCache` is supplied to every graphics-pipeline
creation. At startup VulkanRHI optionally reads
`Saved/Vulkan/PipelineCache-v1.bin`. The wrapper schema, payload size, vendor,
device, pipeline-cache UUID, and Vulkan cache header must all match, and the
complete file must remain within 16 MiB. Missing, corrupt, stale, oversized,
or driver-rejected data is non-fatal and falls back to an empty cache.

Orderly device shutdown queries the cache only after successful device use and
publishes it with Core's atomic byte-file contract. Query or publication
failure emits one owned warning and never makes shutdown fail. Cache access
stays on the RHI thread and introduces no device-idle wait beyond established
shutdown ordering.

## Ownership and Deferred Work

Pipeline, descriptor allocation/update, and driver-cache mutation stay on the
RHI thread in threaded and inline execution modes. Descriptor snapshots own
their resources until eviction or frame-generation reset. Native pipelines,
descriptor pools, and their dependencies retire or reuse only after their exact
GPU completion token; clearing the CPU snapshot cache is not lifetime evidence.

## Related Documentation

- [RHI Command Execution](RHICommandExecution.md)
- [Shader Parameters](ShaderParameters.md)
- [RHI Resource Views and Transfers](RHIResourceViewsAndTransfers.md)
- [RHI Capabilities and Vulkan Startup](RHICapabilitiesAndVulkanStartup.md)
- [Viewport Rendering](ViewportRendering.md)
- [Vulkan Memory and GPU Completion](VulkanMemoryAndGPUCompletion.md)
