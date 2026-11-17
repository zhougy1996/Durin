# Shader Parameters

Summary: Define compiler-owned resource allocation, typed binding and graph/RHI submission boundaries.

Modules: RHI, RenderCore

Last reviewed: 2026-11-18

## High-Level Flow

Declare `FParameters` with `DURIN_BEGIN_SHADER_PARAMETERS` and typed resource
macros; register it through the shader type. Compilation owns descriptor
coordinates. Shader-map initialization admits reflection and caches
`FShaderParameterBindings`. Ordinary `SetShaderParameters` reads those offsets;
RHI receives resolved resources and retains their views/ranges through replay.

## Fixed-layout boundaries

Ordinary shader declarations and generated emitters omit `vk::binding` and HLSL
resource `register(t/s/u/bN)` coordinates. Do not add resource-role slot tables
or share an allocation merely because names/source modules match. Storage image
formats, vertex/output locations and push constants are separate constraints.

`FShaderSourcePolicyTests.FixedLocationsRequireExactBoundaryExceptions` in
`ShaderReflectionTests.cpp` scans the Shaders/Source/Tests/Plugins trees of every
`Durin.dworkspace` project, including inline sources and emitters. Third-party
and build/output trees are excluded. Exact file/test exceptions specify allowed
coordinates and reasons; stale or changed exceptions fail. This is a source
syntax guard; numeric provider changes still require review against reflection.

Approved exceptions are independent low-level RHI/native fixtures and controlled
Material/ContactShadow relocation cases. Native MSL buffer/texture/sampler
indices and synthetic reflection/negative inputs are not production conventions.
The executable exception table is authoritative; there is no directory exemption.

## Detached semantic schemas and allocation candidates

`FShaderParametersMetadata` describes names, C++ offsets, kinds, array extents,
optionality, owners and Draw/Pass scope. C++ offsets never choose GPU locations.
Included owners preserve names; nested structures prefix emitted names.
Metadata and referenced member storage must remain immutable and outlive users.
`FShaderParametersMetadataBuilder` owns data-driven names/member storage; nested
metadata still requires an independent owner.

`FShaderType` collects its canonical schema and host-offset records once at
registration. Compilation, Cook request identity and instance binding reuse them.
`FShaderParameterBindings::Bind(Type, Reflection)` still admits each compiled
product, then applies its locations to the validated host records. Independently
supplied metadata/maps keep their own validation. The registration cache contains
no descriptor map.

`FShaderParameterMap` owns compiler locations. Admission rejects unknown/duplicate
resources or locations, missing required fields, incompatible kind/shape/array/
stage, and declared constant-layout mismatches. Optional fields may be optimized
out. Dynamic-uniform metadata accepts uniform-buffer reflection while retaining
its submission policy. Failure clears bindings; complete maps publish atomically.

Detached `FShaderParameterSchema` excludes C++ offsets and device resources.
`BuildShaderParameterAllocationMap` adds exact target/type/VF/permutation/stage/
compile-key context around the same map. External schemas and allocation
candidates retain canonical identity validation. Both schema/envelope versions
are 2. Material-specific composition and Cook acceptance belong to
[material binding ownership](../Materials/MaterialShaderStages.md#logical-bindings-and-runtime-projection).

Registered graphics requests apply `DURIN_STAGE_PARAMETER_SPACES=1`: default-space
vertex resources use set 0 and fragment resources set 1. Explicit nonzero spaces
remain unchanged; collisions reject. Mixed compute entries keep compiler
allocation. Raw compiler fixtures opt in explicitly. The policy runs before
Metal translation and is idempotent across direct, prepared and Cook requests.

## Typed Submission Helper

Use `SetShaderParameters(CommandList, ShaderRef, Parameters)` with the same
retained shader-map generation used to build the pipeline. Required resources
must be present; optional metadata permits an absent compiled allocation, not a
null resource when reflection requires one. Arrays must match reflected extent.
The reflection adapter `BuildShaderParameterBindings` shares map/binding logic.

Uniform payloads use explicit `FRHIUniformBufferRange` snapshots; host parameter
structs are not byte-upload blocks. `DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_LAYOUT`
can declare expected payload size and named constant members. Compiler layouts
remain independent of the enclosing C++ resource struct.

`DURIN_SHADER_PARAMETER_TEXTURE_VIEW` and `...STORAGE_IMAGE_VIEW` retain exact
counted texture views. Use them for explicit mip/layer/face selection. Graph
callbacks first resolve declared ranges; writable cube faces use 2D/array views,
while sampling preserves cube interpretation.

## Render Graph Composed Submission

Mark shader-backed graph resources with `DURIN_SHADER_PARAMETER_GRAPH_TEXTURE`,
`...GRAPH_STORAGE_IMAGE` or `...GRAPH_STORAGE_BUFFER`. Supply the callback-local
`FRDGShaderParameterScope` to the composed submission overload. The graph layout
supplies capability/range; shader reflection supplies actual descriptor positions.

Each shader instance's bindings cache the static name/type/extent join against
an immutable graph-layout identity; rebinding replaces that cache. Cache entries
retain identity tokens so recycled layout addresses cannot select stale joins.
No graph handles, resource values or descriptor locations from another product
are cached. Domain, callback scope, engagement, backing, range and access remain
execution checks. A required reflected resource cannot use an absent optional.
Ordinary fallback storage is permitted only for non-graph bindings. Temporary
resource/view storage preserves exact-view lifetime and atomic publication; the
existing immutable RHI batch is a replay owner, not reusable resolution scratch.
Buffer-only submissions allocate no exact-view staging.

The scope borrows frozen parameters and the active resolver; do not retain it
outside the callback. Captures expose authoring fields, normalized uses and
parameter paths; descriptor coordinates stay in shader reflection. Graph
capabilities and optional-wrapper address handling belong to
[Render Graph](../Pipeline/RenderGraph.md#reflected-shader-composition).

## Scene resource bindings

Local/Spline graphics bind named Primitive/Instance/payload/selected-ID storage
ranges at accepted locations. Attributes belong to scene/submission owners; IDs
belong to draw domains. Cached commands never retain graph handles. Unsupported
vertex factories retain the explicit `MaterialPrimitive` uniform path. Scene
packing, draw parameters and retirement belong to [GPUScene](../Scene/GPUScene.md).

## Backend and Material Boundaries

`FRHICommandListBase::SetShaderParameters` receives resolved
`FRHIShaderParameterResource` records. Ordinary feature code uses typed providers.
Graphics/compute updates must target shaders owned by the active pipeline.
Every reflected descriptor element is required before draw/dispatch.

RHI canonicalizes and retains exact buffer/texture views and parent resources.
Vulkan merges snapshots by `(SetIndex, BindingIndex, ArrayElement)`; Metal consumes
the compiler-agreed compact map. Native indices do not become portable resource
roles. See [binding snapshots](../RHI/GraphicsStateAndBindings.md) and
[views/ranges](../RHI/RHIResourceViewsAndTransfers.md). Cache versions, stale-data
rejection and source-free loading belong to [Shader Cache](ShaderCache.md).

## Limits and Validation

Typed parameter structs currently describe resources. Inline values, root-constant
blocks, bindless and partially-bound arrays are unsupported; push constants use
a separate path. These are capability limits, not reasons to invent fixed slots.

Use `RenderShaderContractTests` for schema/binding/relocation/source-policy
coverage, cache/library/Cook targets for transport, and the affected feature GPU
case for output. Keep rejection, lifetime and source-free replay assertions;
avoid tests that only restate metadata member order or historical coordinates.
