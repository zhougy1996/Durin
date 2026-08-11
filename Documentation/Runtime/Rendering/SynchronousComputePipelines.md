# Synchronous Compute Pipelines

Summary: Define portable compute PSO identity, recorded dispatch, reflected binding, synchronization, and Vulkan execution.

Modules: RHI, RenderCore, VulkanRHI

Durin exposes direct compute dispatch through the same recorded command-list
timeline and Vulkan immediate queue used by graphics. Compute pipeline selection
does not create an asynchronous queue, overlap work, or introduce queue-family
ownership transfers.

## Pipeline Creation

`FComputePipelineStateInitializer` contains one compute shader and one complete
`FPipelineLayoutDesc`. `BuildComputePipelineStateKey` rejects null or non-compute
shaders, non-compute descriptor visibility, duplicate bindings, malformed
arrays, and invalid or overlapping push-constant ranges. It sorts reflected
bindings and push-constant ranges before producing a key from the shader content
hash and complete layout.

`RHICreateComputePipelineState` returns one complete `FRHIComputePipelineState`
or null. Debug names label native objects but do not participate in identity.
Vulkan reuses equal keys in a bounded LRU cache, keeps failed candidates out of
the cache, and permits a later retry. Compute and graphics PSO statistics are
reported separately by `RHIGetPipelineCacheStatistics`; descriptor and
structural-layout statistics remain shared.

## Recording and Dispatch

A command list records the following sequence outside a render pass:

1. `SwitchPipeline(ERHIPipeline::Compute)`
2. `SetComputePipelineState(...)`
3. compute-stage `SetShaderParameters(...)` and optional compute push constants
4. `Dispatch(GroupCountX, GroupCountY, GroupCountZ)`

All three group counts must be nonzero and no greater than the corresponding
`FRHICapabilities::MaxComputeWorkGroupCount` value. The recorder retains the
PSO, shader, views, their parent resources, and push bytes until replay
completes. Draw commands remain graphics-only and dispatch remains
compute-only.

Inline and dedicated-thread execution replay the same typed commands. Both
graphics and compute resolve to the admitted shared immediate context; `None`
removes the active replay context.

## Binding and State

RenderCore reflection is the source of the pipeline layout. Typed compute shader
parameters may include storage buffers, storage images, sampled textures,
samplers, and uniform ranges. Vulkan keeps compute pending state separate from
graphics dynamic and draw state, binds descriptors with the compute bind point,
and clears an incompatible snapshot on a compute PSO change or frame reset.

Every reflected descriptor element must be populated before dispatch. The
active shader must match the compute PSO's shader content hash. Uniform and
sampled resources require compute-readable tracked access; storage resources
require `ComputeShaderReadWrite`. Image layout and Vulkan stage/access masks are
derived exclusively from the shared RHI resource-state mapping.

Compute push constants require compute-only visibility and must fit one
reflected range owned by the active PSO.

## Synchronization and Consumers

Transitions are pipeline-neutral and remain outside render passes. A compute
write is made visible to its next consumer only by an explicit transition:

- compute to compute: `ComputeShaderReadWrite -> ComputeShaderReadWrite`
- compute to graphics: storage write to `GraphicsShaderRead` before a draw
- compute to copy: storage write to `TransferRead`
- compute to CPU: transition through the existing copy/readback contract

Queue order alone is not the memory dependency, and device idle is not required
between dispatch and a consumer. GPU-idle remains a lifecycle/debugging
operation rather than part of compute correctness.

## Related Documentation

- [RHI command execution](RHICommandExecution.md)
- [RHI resource transitions](RHIResourceTransitions.md)
- [Shader parameters](ShaderParameters.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [Graphics state and bindings](GraphicsStateAndBindings.md)
