# Shader Parameters

Summary: Define reflected shader-parameter layouts, binding, validation, and RHI publication.

Modules: RHI, RenderCore

This document describes the current typed shader parameter path used by `RenderCore`, `RHI`, and `VulkanRHI`.

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
`FRDGShaderParameterScope` scope. For every reflected binding marked as a
graph resource, RenderCore locates the uniquely named composed graph member,
validates binding type and array extent, checks graphics/compute domain and
graph access, and resolves the member through the active pass resolver. It
creates an exact counted texture view or supplies the exact buffer range only
after all structural checks have succeeded. Samplers, uniform-buffer ranges,
and other ordinary fields can be supplied from the existing typed shader
parameter struct in the same call; graph-backed fields in that compatibility
struct are not read.

Composed submission uses the pass allocation's immutable graph-parameter
layout. Its validated name-sorted binding table identifies the leaf group, and
the group's precomputed element offsets enumerate fixed arrays. Execution does
not rebuild a composed-member list, recount names, construct paths, or recurse
through graph metadata; shader reflection remains authoritative for descriptor
coordinates and required array extent.

Selected shaders may consume subsets of a pass object. An optional graph field
may therefore be absent when its binding is not present in active reflection.
Once reflection requires the binding, absence is an initialization error rather
than a null or partially bound descriptor. The complete resolved resource list
is forwarded to RHI atomically, preserving the existing pipeline-ownership,
descriptor-occupancy, replay-retention, and Vulkan snapshot contracts.

Render Graph captures expose composed authoring before descriptor publication.
Each submitted graph field records its full parameter path, engagement,
canonical resource ID when present, shader binding name/type, and declared
graph capability. The normalized use record carries the same pass declaration
index and field path after compiler range partitioning. Inspect these records
together when diagnosing reflection drift: the field record answers what the
pass granted, while the use and dependency records answer what the compiler
scheduled. Descriptor coordinates remain shader-reflection evidence and are
not copied into graph identity.

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

## Backend and Material Boundaries

`FVulkanCommandListContext` owns pending per-draw resources and descriptor
materialization; the PSO owns immutable pipeline/layout state. Binding updates
merge by exact `(SetIndex, BindingIndex, ArrayElement)`, replacing that location
while retaining unrelated bindings. Descriptor write vectors remain alive
through `updateDescriptorSets(...)`.

Snapshot validation, cache identity, bounds, eviction, and GPU-completion-gated
pool reuse are defined by [Graphics State and Bindings](GraphicsStateAndBindings.md).
[Material System](MaterialSystem.md#renderer-surface-execution) owns material
representation decoding, role fallbacks, and the resolved surface packet before
it reaches this typed submission boundary.

## Example: ImGui Submission

`MonaImGui` declares a typed fragment parameter struct:

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

At draw time the backend submits:

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
6. assert capture correlation for the graph field, canonical resource,
   normalized use, and reflected binding in the owning RenderCore or Renderer
   contract test

If binding resolution fails during shader-map initialization, treat it as a contract mismatch between shader code reflection and the declared `FParameters` struct.
