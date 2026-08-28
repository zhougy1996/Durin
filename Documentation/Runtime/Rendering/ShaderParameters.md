# Shader Parameters

Summary: Define reflected shader-parameter layouts, binding, validation, and RHI publication.

Modules: RHI, RenderCore

This document describes the current typed shader parameter path used by `RenderCore`, `RHI`, and `VulkanRHI`.

## Goals

The old prototype exposed descriptor-facing details to callers:

- callers had to provide descriptor set indices
- callers had to provide binding indices
- callers had to provide resource binding types

The current design moves those responsibilities into the shader system:

- shader code reflection remains the source of truth for resource bindings
- shader classes declare typed parameter structs
- RenderCore resolves parameter fields to reflected bindings once during shader initialization
- RHI callers submit typed parameter structs instead of descriptor-oriented binding records
- Vulkan materializes descriptor sets lazily at draw time and caches the result for the current frame

## High-Level Flow

1. A shader type optionally declares parameter metadata.
2. Shader compilation produces `FShaderReflectionData::ResourceBindings`.
3. `FShader::InitializeParameterBindings()` resolves metadata fields to reflected bindings and stores the resolved result on the shader instance.
4. Callers build a typed `FParameters` struct and call the typed `SetShaderParameters(...)` helper.
5. The helper converts the typed struct into compact resolved `FRHIShaderParameterResource` entries.
6. `FVulkanCommandListContext` merges those entries into the active graphics or compute pending bindings.
7. `Draw()`, `DrawIndexed()`, or `Dispatch()` materializes the complete descriptor snapshot for the active pipeline layout.
8. Vulkan binds the descriptor sets at the matching graphics or compute bind point and submits the command.

## Public Types

The public low-level types live in `Engine/Source/Runtime/RHI/Public/RHIShaderParameters.h`.

### `FShaderParameterMetadata`

Describes a parameter field declared by a shader class:

- `Name`: field name expected to match reflection
- `Offset`: byte offset inside the shader `FParameters` struct
- `Type`: expected RHI binding type
- `ArraySize`: expected reflected array size

This is declaration-time metadata, not a runtime binding.

### `FShaderParameterBinding`

Describes a resolved runtime binding:

- `Name`
- `Offset`
- `SetIndex`
- `BindingIndex`
- `ArrayElement`
- `Type`
- `ArraySize`

This is the bridge between shader-declared fields and backend descriptor layout.

### `FRHIShaderParameterResource`

This is now an internal-style resolved submission record rather than a user-authored descriptor description. It contains:

- `Resource`
- `SetIndex`
- `BindingIndex`
- `Type`
- `Offset` and `Size` for compatibility buffer-range submissions

Callers are not expected to fill this by hand in the normal path.

Resource submission is canonicalized while the RHI command is recorded.
Buffer and texture entries become counted `FRHIBufferView` or
`FRHITextureView` objects, including canonical whole-resource views when the
compatibility scalar form is used. The recorded command retains those views,
and Vulkan descriptor writes consume their exact native interpretation. See
[RHI resource views and transfers](RHIResourceViewsAndTransfers.md).

## RenderCore Responsibilities

RenderCore owns shader parameter declaration, validation, and cached binding resolution.

### Shader Type Declaration

`FShaderType` accepts an optional `std::span<const FShaderParameterMetadata>` in its constructor. This allows a shader type to publish parameter field metadata alongside its virtual path, frequency, and entry point.

Example pattern:

```cpp
class FExampleShader : public FShader
{
public:
    using FShader::FShader;

    struct FParameters
    {
        FRHITexture* ExampleTexture = nullptr;
        FRHISampler* ExampleSampler = nullptr;
    };

    static auto GetParametersMetadata() -> std::span<const FShaderParameterMetadata>
    {
        static const std::array Parameters = {
            DURIN_SHADER_PARAMETER(ExampleTexture, ERHIBindingType::Texture),
            DURIN_SHADER_PARAMETER(ExampleSampler, ERHIBindingType::Sampler)
        };
        return Parameters;
    }
};
```

The helper macro `DURIN_SHADER_PARAMETER(...)` converts a field into metadata using `offsetof(FParameters, ...)`.

### Binding Resolution

`BuildShaderParameterBindings(...)` compares declared metadata against `FShaderReflectionData::ResourceBindings`.

It validates:

- parameter name exists in reflection
- reflected binding type matches declared type
- reflected array size matches declared array size

On success it emits resolved `FShaderParameterBinding` entries.

### Cached Per-Shader Bindings

Each `FShader` stores a `std::vector<FShaderParameterBinding>` as `ParameterBindings`.

This is built once via `FShader::InitializeParameterBindings(...)` during shader map initialization. That means runtime parameter submission does not need repeated string lookup or repeated reflection walking.

### Typed Submission Helper

`RenderCore/Public/Shader/Shader.h` provides:

```cpp
template<typename ShaderType>
auto SetShaderParameters(
    FRHICommandListBase& RHICmdList,
    const TShaderRef<ShaderType>& Shader,
    const typename ShaderType::FParameters& Parameters
) -> void;
```

This helper:

- reads the cached `FShaderParameterBinding` list from the shader instance
- uses each binding's byte offset to read the matching resource pointer from the typed `FParameters` struct
- emits resolved `FRHIShaderParameterResource` entries
- forwards them to the low-level RHI command list

The helper intentionally keeps the public callsite free from descriptor set and binding knowledge.

### Render Graph Composed Submission

Shaders used by a composed graph pass mark graph-backed declarations with
`DURIN_SHADER_PARAMETER_GRAPH_TEXTURE`,
`DURIN_SHADER_PARAMETER_GRAPH_STORAGE_IMAGE`, or
`DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER`. The marker is cached on
`FShaderParameterBinding`; it does not change reflection, shader bytecode, or
the compatibility manual submission path.

The pass calls the overload that accepts its callback-lifetime
`FRenderGraphShaderParameters` scope. For every reflected binding marked as a
graph resource, RenderCore locates the uniquely named composed graph member,
validates binding type and array extent, checks graphics/compute domain and
graph access, and resolves the member through the active pass resolver. It
creates an exact counted texture view or supplies the exact buffer range only
after all structural checks have succeeded. Samplers, uniform-buffer ranges,
and other ordinary fields can be supplied from the existing typed shader
parameter struct in the same call; graph-backed fields in that compatibility
struct are not read.

Selected shaders may consume subsets of a pass object. An optional graph field
may therefore be absent when its binding is not present in active reflection.
Once reflection requires the binding, absence is an initialization error rather
than a null or partially bound descriptor. The complete resolved resource list
is forwarded to RHI atomically, preserving the existing pipeline-ownership,
descriptor-occupancy, replay-retention, and Vulkan snapshot contracts.

## RHI Responsibilities

RHI now exposes a single low-level resource-submission path:

- `FRHICommandListBase::SetShaderParameters(FRHIShader*, span<FRHIShaderParameterResource>)`

At this layer, bindings are already resolved. RHI no longer has a byte-blob
parameter path and no longer expects callers to hand-build descriptor details
in normal usage. Graphics updates accept vertex or fragment shaders owned by
the active graphics PSO; compute updates accept only the compute shader owned by
the active compute PSO. Both paths retain canonical views and parent resources
through replay and require every reflected descriptor element before draw or
dispatch.

## Vulkan Responsibilities

Vulkan owns draw-time descriptor materialization and frame-local snapshot
caching. Native descriptor-pool reuse is independently gated by GPU completion.

### Why Mutable Descriptor State Moved Out Of PSO

The old prototype stored mutable descriptor state directly on `FVulkanGraphicsPipelineState`. That caused bad behavior when parameter state changed across calls because the PSO is conceptually immutable pipeline state, while descriptor contents are per-draw mutable state.

The new design keeps:

- pipeline and pipeline layout on the PSO
- pending shader resources and descriptor cache on `FVulkanCommandListContext`

### Pending Shader Resource State

`FVulkanCommandListContext::RHISetShaderParameters(...)` merges incoming resolved resources into `PendingShaderResources`.

Merge rule:

- if a `(SetIndex, BindingIndex, ArrayElement)` location is not present, append it
- if the location already exists, overwrite the existing record

This allows multiple `SetShaderParameters(...)` calls before a draw. Later calls replace only the exact binding they touch; unrelated bindings remain live.

### Descriptor Set Cache

The command context stores a frame-generation-local indexed cache:

- resource hash
- sorted resource records
- allocated descriptor sets
- retained resource owners
- least-recently-used serial

`GetOrCreateDescriptorSetsForDraw()` performs:

1. validate complete occupancy against the active PSO layout
2. sort pending resource records by `(SetIndex, BindingIndex, ArrayElement)`
3. compute a hash from binding coordinates, binding type, view/resource identity, and immutable range data
4. search the indexed hash candidates and confirm complete equality
5. reuse descriptor sets on hit
6. allocate descriptor sets and write descriptors on miss

The cache lifetime is intentionally tied to the frame-pool generation.
`RHIBeginFrame()` clears it before selecting a completion-eligible descriptor
pool batch; a native pool resets only after its maximum use token completes.
One command context is bounded to 512 entries and 8192 descriptor values;
least-recently-used eviction releases retained resources when either bound is
reached.

### Draw-Time Materialization

`Draw()` and `DrawIndexed()` do three things in order:

1. prepare non-descriptor pipeline state such as viewport and scissor
2. fetch cached or newly-built descriptor sets for the current pending bindings
3. bind descriptor sets and submit the draw call

This means descriptor allocation and update happen only when a draw actually needs them.

### Material Representation Binding

Material resolution is completed before a Renderer draw reaches this typed
shader-parameter path. Engine publishes an immutable
`FMaterialRenderRepresentation` containing the validated layout identity,
uniform bytes, and counted texture-reference resources. The current
`FMaterialRenderBinding` decoder accepts only the exact v3 field table and
turns that compact payload into the PBR constants, UV transforms, sampler
states, and eight texture roles required by the material shaders.

`FStaticMeshRenderer` does not perform material parameter GUID or `FName`
lookup, inspect reflected material objects, or read ad-hoc fixed fields from a
material snapshot. It builds the existing `FStaticMeshMaterialUniform` ABI
from the decoded binding and submits the typed shader parameters together
with the binding's counted texture references and shared environment-lighting
resources. A layout mismatch is reported as a
Renderer `ShaderBinding` diagnostic and is replaced by a complete default
ErrorMaterial before shader-map, pipeline, or descriptor selection. The
Renderer-owned white, black, or flat-normal texture remains the final
role-specific fallback when a validated resource slot is null or not ready;
the environment set falls back atomically to black cube/LUT resources.

### Stable Descriptor Write Inputs

Descriptor writes are built from local vectors of:

- `vk::DescriptorBufferInfo`
- `vk::DescriptorImageInfo`
- `vk::WriteDescriptorSet`

These vectors live for the duration of `updateDescriptorSets(...)`, which fixes the earlier lifetime problem caused by mutable PSO-owned temporary state that could be invalidated across multiple parameter submissions.

## Example: ImGui Migration

`MonaImGui` is the first migrated caller.

Its fragment shader now uses a typed parameter struct:

```cpp
struct FParameters
{
    FRHITexture* fontTexture = nullptr;
    FRHISampler* fontSampler = nullptr;
};
```

The metadata names intentionally match Slang reflection names from `Engine/Shaders/Slang/ImGui.slang`:

- `fontTexture`
- `fontSampler`

At draw time the backend now does:

```cpp
FImGuiFragmentShader::FParameters ShaderParameters;
ShaderParameters.fontTexture = *TextureRefPtr;
ShaderParameters.fontSampler = GBackendState.LinearSampler;
SetShaderParameters(CommandList, GBackendState.FragmentShader, ShaderParameters);
```

No descriptor set index or binding index appears at the callsite.

## Tests

`RenderCoreTests` currently cover:

- successful metadata-to-reflection binding resolution
- missing reflection binding rejection
- reflected type mismatch rejection
- shader instance caching of resolved parameter bindings
- composed owner identity, graph access/type validation, exact texture views
  and buffer ranges, required optionals, arrays, shader subsets, and domains
- stable graph field-to-shader binding capture evidence

These tests live in:

- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`

## Current Limitations

This remains a staged design rather than the final compute/bindless end-state.

- parameter structs currently model resource bindings only
- uniform bytes are not yet modeled as a typed parameter block
- push constants remain a separate manual path
- bindless and partially-bound arrays are not supported
- descriptor snapshot invalidation remains frame-local while native pool reuse
  follows the completed-token contract

## Practical Guidance

When adding a new shader to this system:

1. create a shader subclass with an `FParameters` struct
2. declare metadata with the typed `DURIN_SHADER_PARAMETER_*` macros; use a
   `*_GRAPH_*` macro when a migrated graph pass must supply that binding
3. pass that metadata into the `FShaderType` constructor
4. make sure the field names match reflected shader resource names
5. call the typed `SetShaderParameters(...)` helper, passing the exact graph
   shader scope for a composed pass

If binding resolution fails during shader-map initialization, treat it as a contract mismatch between shader code reflection and the declared `FParameters` struct.
