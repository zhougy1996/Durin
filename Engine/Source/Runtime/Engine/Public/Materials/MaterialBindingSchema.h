#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
    enum class EMaterialShaderEntry : uint8;
    enum class EMaterialDomain : uint8;
    enum class EMaterialBindingRole : uint8
    {
        View, Primitive, Material, Collection, Texture, Sampler, Lighting, LocalLights, EnvironmentIrradiance, EnvironmentPrefiltered, EnvironmentBrdfLut, EnvironmentSampler, DirectionalShadowTexture, DirectionalShadowSampler, GPUScenePrimitive, GPUSceneInstance, GPUScenePayload, GPUSceneInstanceIds, Spline, HitProxy, SceneUniform, SceneInput0, SceneDepth, ScenePointSampler, SceneLinearSampler, Count
    };
    // Accepted compiler locations persist with semantic provider references.
    struct FMaterialShaderBinding
    {
        EMaterialBindingRole Role = EMaterialBindingRole::View;
        uint16 ResourceIndex = 0;
        uint32 SetIndex = 0;
        uint32 BindingIndex = 0;
        ERHIBindingType Type = ERHIBindingType::UniformBuffer;
        uint32 ArraySize = 1;
        auto operator==(const FMaterialShaderBinding&) const -> bool = default;
    };
    struct FMaterialBindingDefinition
    {
        EMaterialBindingRole Role;
        std::string_view Name;
        std::string_view SlangType;
        ERHIBindingType ReflectionType;
        ERHIBindingType RuntimeType;
        uint32 SetIndex, BindingIndex, Stride;
        uint8 Stages, Entries;
        uint32 ArraySize = 1;
    };
    inline constexpr uint32 MaterialBindingSchemaVersion = 2;
    ENGINE_API auto GetMaterialBindingDefinitions() -> std::span<const FMaterialBindingDefinition>;
    ENGINE_API auto GetMaterialBindingDefinition(EMaterialBindingRole Role) -> const FMaterialBindingDefinition&;
    ENGINE_API auto GetMaterialBindingSlot(EMaterialBindingRole Role, uint32 Index = 0) -> uint32;
    ENGINE_API auto GetMaterialRuntimeBindingType(EMaterialBindingRole Role, EMaterialDomain Domain) -> ERHIBindingType;
    ENGINE_API auto GetMaterialBindingName(EMaterialBindingRole Role, uint32 Index = 0) -> std::string;
    ENGINE_API auto GenerateMaterialBindingDeclaration(EMaterialBindingRole Role, uint32 Index = 0,
        std::string_view SlangTypeOverride = {}) -> std::string;
    // Reject unknown slots, wrong names/kinds/arrays and duplicates before exposing logical roles.
    ENGINE_API auto DescribeMaterialShaderBindings(const FShaderReflectionData& Reflection)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>;
    ENGINE_API auto ValidateMaterialShaderBindings(const FShaderReflectionData& Reflection,
        EMaterialShaderEntry Entry, EShaderFrequency Frequency, uint32 Resources, uint32 Collections)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>;
    inline auto IsMaterialSceneBinding(EMaterialBindingRole Role) -> bool
    { return Role >= EMaterialBindingRole::SceneUniform && Role <= EMaterialBindingRole::SceneLinearSampler; }
    // Provider lifetime classification is independent of compiled descriptor locations.
    inline auto IsMaterialPassBinding(EMaterialBindingRole Role) -> bool
    {
        using R = EMaterialBindingRole;
        return Role != R::Primitive && Role != R::Material && Role != R::Collection
            && Role != R::Texture && Role != R::Sampler && Role != R::Spline && Role != R::HitProxy;
    }
    inline auto IsMaterialPassParameter(std::string_view Name) -> bool
    {
        return std::ranges::any_of(GetMaterialBindingDefinitions(), [&](const auto& Definition) {
            return Definition.Name == Name && IsMaterialPassBinding(Definition.Role);
        });
    }
}
