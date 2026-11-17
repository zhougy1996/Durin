#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Shader/ShaderParameterSchema.h"

namespace Durin
{
    struct FMaterialRenderLayout;
    struct FMaterialParameterCollectionLayout;
    struct FMaterialShaderEntryRequest;

    // Named semantic field and its accepted compiler allocation; no global resource roles.
    struct FMaterialShaderBinding
    {
        std::string Name;
        std::string QualifiedName;
        std::string OwnerName;
        uint32 ResourceIndex = 0;
        uint32 SetIndex = 0;
        uint32 BindingIndex = 0;
        ERHIBindingType Type = ERHIBindingType::UniformBuffer;
        ERHIBindingType RuntimeType = ERHIBindingType::UniformBuffer;
        uint32 ArraySize = 1;
        EShaderParameterBindingScope Scope = EShaderParameterBindingScope::Draw;
        EShaderResourceShape ResourceShape = EShaderResourceShape::Unknown;
        uint32 ConstantBufferSize = 0;
        auto operator==(const FMaterialShaderBinding&) const -> bool = default;
    };
    inline constexpr uint32 MaterialBindingSchemaVersion = 3;
    ENGINE_API auto BuildMaterialBaseParameterSchema(const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections) -> std::expected<FShaderParameterSchema, std::string>;
    ENGINE_API auto BuildMaterialParameterSchema(std::string_view ShaderType, const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections) -> std::expected<FShaderParameterSchema, std::string>;
    ENGINE_API auto DescribeMaterialShaderBindings(const FShaderReflectionData& Reflection,
        const FShaderParameterSchema& Schema) -> std::expected<std::vector<FMaterialShaderBinding>, std::string>;
    ENGINE_API auto ValidateMaterialShaderBindings(const FShaderReflectionData& Reflection,
        std::string_view ShaderType, EShaderFrequency Frequency, const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections) -> std::expected<std::vector<FMaterialShaderBinding>, std::string>;
    ENGINE_API auto GetMaterialParameterMapIdentity(const FMaterialShaderEntryRequest& Entry,
        const FShaderCode& Code) -> FXxHash128;
    inline auto GetMaterialTextureParameterName(uint32 Index) -> std::string { return std::format("MaterialTexture{}", Index); }
    inline auto GetMaterialSamplerParameterName(uint32 Index) -> std::string { return std::format("MaterialSampler{}", Index); }
    inline auto GetMaterialCollectionParameterName(uint32 Index) -> std::string { return std::format("MaterialCollection{}", Index); }
    inline auto GenerateShaderParameterDeclaration(std::string_view Type, std::string_view Name) -> std::string
    { return std::format("{} {};\n", Type, Name); }
}
