# Shader Parameters

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
6. `FVulkanCommandListContext` merges those entries into pending draw-state bindings.
7. `RHIDrawIndexed()` asks Vulkan to find or create descriptor sets for the current pending bindings and current pipeline layout.
8. Vulkan binds the descriptor sets and submits the draw.

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
- `Type`
- `ArraySize`

This is the bridge between shader-declared fields and backend descriptor layout.

### `FRHIShaderParameterResource`

This is now an internal-style resolved submission record rather than a user-authored descriptor description. It contains:

- `Resource`
- `SetIndex`
- `BindingIndex`
- `Type`

Callers are not expected to fill this by hand in the normal path.

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

## RHI Responsibilities

RHI now exposes a single low-level resource-submission path:

- `FRHICommandListBase::SetShaderParameters(FRHIShader*, span<FRHIShaderParameterResource>)`

At this layer, bindings are already resolved. RHI no longer has a byte-blob parameter path and no longer expects callers to hand-build descriptor details in normal usage.

## Vulkan Responsibilities

Vulkan owns draw-time descriptor materialization and frame-local caching.

### Why Mutable Descriptor State Moved Out Of PSO

The old prototype stored mutable descriptor state directly on `FVulkanGraphicsPipelineState`. That caused bad behavior when parameter state changed across calls because the PSO is conceptually immutable pipeline state, while descriptor contents are per-draw mutable state.

The new design keeps:

- pipeline and pipeline layout on the PSO
- pending shader resources and descriptor cache on `FVulkanCommandListContext`

### Pending Shader Resource State

`FVulkanCommandListContext::RHISetShaderParameters(...)` merges incoming resolved resources into `PendingShaderResources`.

Merge rule:

- if a `(SetIndex, BindingIndex)` pair is not present, append it
- if the pair already exists, overwrite the existing record

This allows multiple `SetShaderParameters(...)` calls before a draw. Later calls replace only the exact binding they touch; unrelated bindings remain live.

### Descriptor Set Cache

The command context stores a frame-local linear cache:

- layout hash
- resource hash
- sorted resource records
- allocated descriptor sets

`GetOrCreateDescriptorSetsForDraw()` performs:

1. read the current pipeline layout hash
2. sort pending resource records by `(SetIndex, BindingIndex)`
3. compute a hash from layout hash + binding coordinates + binding type + resource pointer identity
4. search the per-frame cache for an exact match
5. reuse descriptor sets on hit
6. allocate descriptor sets and write descriptors on miss

The cache lifetime is intentionally frame-local. `RHIBeginFrame()` clears it, and the descriptor pool is also reset per frame.

### Draw-Time Materialization

`RHIDrawIndexed()` does three things in order:

1. prepare non-descriptor pipeline state such as viewport and scissor
2. fetch cached or newly-built descriptor sets for the current pending bindings
3. bind descriptor sets and submit the draw call

This means descriptor allocation and update happen only when a draw actually needs them.

### Stable Descriptor Write Inputs

Descriptor writes are built from local vectors of:

- `vk::DescriptorBufferInfo`
- `vk::DescriptorImageInfo`
- `vk::WriteDescriptorSet`

These vectors live for the duration of `updateDescriptorSets(...)`, which fixes the earlier lifetime problem caused by mutable PSO-owned temporary state that could be invalidated across multiple parameter submissions.

## Example: ImGui Migration

`MonaImGuiBackend` is the first migrated caller.

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

These tests live in:

- `Engine/Source/Programs/Tests/RenderCoreTests/Private/ShaderFoundationTests.cpp`

## Current Limitations

This is still a staged design, not the final end-state.

- parameter structs currently model resource bindings only
- uniform bytes are not yet modeled as a typed parameter block
- push constants remain a separate manual path
- compute pipeline support has not been wired into this submission model
- array bindings beyond the existing reflection validation path are not yet expanded into higher-level typed helpers
- descriptor cache eviction is frame-local and linear, which is simple but not yet optimized for very large descriptor churn
- caching uses resource pointer identity; it assumes stable object identity for the bound RHI resources during a frame

## Practical Guidance

When adding a new shader to this system:

1. create a shader subclass with an `FParameters` struct
2. declare metadata with `DURIN_SHADER_PARAMETER(...)`
3. pass that metadata into the `FShaderType` constructor
4. make sure the field names match reflected shader resource names
5. call the typed `SetShaderParameters(...)` helper from rendering code

If binding resolution fails during shader-map initialization, treat it as a contract mismatch between shader code reflection and the declared `FParameters` struct.
