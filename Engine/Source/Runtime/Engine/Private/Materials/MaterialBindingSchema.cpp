#include "Materials/MaterialBindingSchema.h"
#include "MaterialPreparedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Shader/MaterialShader.h"
#include "Shader/ShaderParameterMetadataBuilder.h"

namespace Durin
{
    auto BuildMaterialBaseParameterSchema(const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections) -> std::expected<FShaderParameterSchema, std::string>
    {
        FShaderParameterSchema Schema;
        auto Add = [&](std::string Name, std::string Path, std::string Type, ERHIBindingType Kind) -> FShaderOperationResult {
            FShaderParametersMetadataBuilder Builder;
            auto Added = Builder.AddResourceParameter(Name, Kind, 1, true, Type, "Material");
            if (!Added) return std::unexpected(Added.error());
            auto Metadata = Builder.Build("MaterialParameters");
            if (!Metadata) return std::unexpected(Metadata.error());
            auto Projected = BuildShaderParameterSchema((*Metadata)->Metadata);
            if (!Projected) return std::unexpected(Projected.error());
            if (Projected->Fields.size() != 1) return std::unexpected("Material parameter projection requires one field");
            auto Field = std::move(Projected->Fields.front()); Field.QualifiedName = std::move(Path);
            Schema.Fields.push_back(std::move(Field));
            return {};
        };
        if (auto Result = Add("Material", "Material.Uniform", "ConstantBuffer<MaterialUniform>", ERHIBindingType::UniformBuffer); !Result)
            return std::unexpected(Result.error());
        Schema.Fields.back().ConstantBufferSize = Layout.UniformPayloadSize;
        if (Layout.UniformPayloadSize) {
            Schema.Fields.back().ConstantMembers.push_back({"Reserved", 0, 16});
            for (uint32 I = 0; I < Layout.UniformFieldCount; ++I)
                Schema.Fields.back().ConstantMembers.push_back({std::format("Value{}", I), 16 + 16 * I, 16});
        }
        for (const auto& Field : Layout.Fields)
        {
            if (Field.Storage != EMaterialRenderFieldStorage::Resource) continue;
            if (auto Result = Add(GetMaterialTextureParameterName(Field.CompactIndex), "Material.Texture." + Field.ParameterId.ToString(),
                Field.Type == EMaterialRenderValueType::TextureCube ? "TextureCube<float4>" : "Texture2D<float4>", ERHIBindingType::Texture); !Result)
                return std::unexpected(Result.error());
            Schema.Fields.back().ProviderIndex = Field.CompactIndex;
            if (auto Result = Add(GetMaterialSamplerParameterName(Field.CompactIndex), "Material.Sampler." + Field.ParameterId.ToString(),
                "SamplerState", ERHIBindingType::Sampler); !Result) return std::unexpected(Result.error());
            Schema.Fields.back().ProviderIndex = Field.CompactIndex;
        }
        for (uint32 Index = 0; Index < Collections.size(); ++Index) {
            if (auto Result = Add(GetMaterialCollectionParameterName(Index), "Material.Collection." + Collections[Index].CollectionId.ToString(),
                std::format("ConstantBuffer<MaterialCollectionUniform{}>", Index), ERHIBindingType::UniformBuffer); !Result)
                return std::unexpected(Result.error());
            Schema.Fields.back().ProviderIndex = Index;
            Schema.Fields.back().ConstantBufferSize = Collections[Index].UniformLayout.UniformPayloadSize;
            Schema.Fields.back().ConstantMembers.push_back({"Reserved", 0, 16});
            for (uint32 I = 0; I < Collections[Index].UniformLayout.UniformFieldCount; ++I)
                Schema.Fields.back().ConstantMembers.push_back({std::format("Value{}", I), 16 + 16 * I, 16});
        }
        return CanonicalizeShaderParameterSchema(std::move(Schema));
    }

    auto BuildMaterialParameterSchema(std::string_view ShaderType, const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections) -> std::expected<FShaderParameterSchema, std::string>
    {
        auto Base = BuildMaterialBaseParameterSchema(Layout, Collections);
        if (!Base) return std::unexpected(Base.error());
        auto Schema = std::move(*Base);
        const auto& Types = FShaderType::GetTypeList();
        const auto Type = std::ranges::find_if(Types, [&](const auto* T) { return T->GetName() == ShaderType; });
        if (Type == Types.end()) return std::unexpected("Registered parameter owner is missing");
        const auto* Description = FindMaterialShaderEntry(ShaderType);
        const auto* FactoryMetadata = Description && Description->VertexFactory
            ? Description->VertexFactory()->GetShaderDescription().ParametersMetadata : nullptr;
        for (const auto* Metadata : {(*Type)->GetParametersMetadata(), FactoryMetadata})
        {
            if (!Metadata) continue;
            auto Owned = BuildShaderParameterSchema(*Metadata);
            if (!Owned) return std::unexpected(Owned.error());
            for (auto Field : Owned->Fields)
            {
                if (Field.OwnerName.empty() || Field.ShaderType.empty())
                    return std::unexpected("Material shader parameters require owning scope and shader type metadata");
                Field.QualifiedName = Field.OwnerName + "." + Field.QualifiedName;
                const auto Existing = std::ranges::find(Schema.Fields, Field.ShaderName, &FShaderParameterField::ShaderName);
                if (Existing != Schema.Fields.end())
                {
                    if (Existing->Type != Field.Type || Existing->Kind != Field.Kind || Existing->ArraySize != Field.ArraySize
                        || Existing->OwnerName != Field.OwnerName || Existing->ResourceShape != Field.ResourceShape
                        || Existing->ShaderType != Field.ShaderType)
                        return std::unexpected("Composed shader parameter declarations conflict");
                    Existing->RuntimeType = Field.RuntimeType; Existing->bOptional = Field.bOptional; Existing->Scope = Field.Scope;
                }
                else Schema.Fields.push_back(std::move(Field));
            }
        }
        return CanonicalizeShaderParameterSchema(std::move(Schema));
    }

    auto DescribeMaterialShaderBindings(const FShaderReflectionData& Reflection, const FShaderParameterSchema& Schema)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>
    {
        const auto Canonical = CanonicalizeShaderParameterSchema(Schema);
        if (!Canonical || Canonical->Identity != Schema.Identity || Canonical->Fields != Schema.Fields)
            return std::unexpected("Material parameter schema identity is invalid");
        if (Reflection.ResourceBindings.size() > 256 || !Reflection.PushConstantRanges.empty())
            return std::unexpected("Unsupported material parameter layout");
        std::vector<FMaterialShaderBinding> Result;
        std::set<std::pair<uint32, uint32>> Seen;
        std::set<std::string> Names;
        for (const auto& Binding : Reflection.ResourceBindings)
        {
            const auto Field = std::ranges::find(Schema.Fields, Binding.Name, &FShaderParameterField::ShaderName);
            if (Field == Schema.Fields.end() || Binding.Type != Field->Type || Binding.ArraySize != Field->ArraySize
                || (Field->ResourceShape != EShaderResourceShape::Unknown && Binding.ResourceShape != Field->ResourceShape)
                || (Field->ConstantBufferSize && (Binding.ConstantBufferSize != Field->ConstantBufferSize || Binding.ConstantMembers != Field->ConstantMembers))
                || !Seen.emplace(Binding.SetIndex, Binding.BindingIndex).second || !Names.insert(Binding.Name).second)
                return std::unexpected("Compiled parameter name, kind, array or allocation is incompatible with its schema");
            Result.push_back({Binding.Name, Field->QualifiedName, Field->OwnerName, Field->ProviderIndex, Binding.SetIndex,
                Binding.BindingIndex, Binding.Type, Field->RuntimeType, Binding.ArraySize, Field->Scope,
                Binding.ResourceShape, Binding.ConstantBufferSize});
        }
        for (const auto& Field : Schema.Fields)
            if (!Field.bOptional && !Names.contains(Field.ShaderName)) return std::unexpected("Required shader parameter was optimized out or omitted");
        std::ranges::sort(Result, {}, &FMaterialShaderBinding::QualifiedName);
        return Result;
    }

    auto ValidateMaterialShaderBindings(const FShaderReflectionData& Reflection, std::string_view ShaderType,
        EShaderFrequency Frequency, const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>
    {
        auto Schema = BuildMaterialParameterSchema(ShaderType, Layout, Collections);
        if (!Schema) return std::unexpected(Schema.error());
        return ValidateMaterialShaderBindings(Reflection, *Schema, Frequency, Layout, Collections);
    }

    auto ValidateMaterialShaderBindings(const FShaderReflectionData& Reflection, const FShaderParameterSchema& Schema,
        EShaderFrequency Frequency, const FMaterialRenderLayout& Layout,
        std::span<const FMaterialParameterCollectionLayout> Collections)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>
    {
        const auto Stage = Frequency == EShaderFrequency::Vertex ? EShaderStageFlags::Vertex : EShaderStageFlags::Fragment;
        if ((Frequency != EShaderFrequency::Vertex && Frequency != EShaderFrequency::Fragment)
            || std::ranges::any_of(Reflection.ResourceBindings, [&](const auto& B) { return B.StageFlags != Stage; }))
            return std::unexpected("Material parameter stage is incompatible");
        auto Result = DescribeMaterialShaderBindings(Reflection, Schema);
        if (!Result) return Result;
        for (auto& B : *Result)
        {
            for (const auto& F : Layout.Fields)
                if (F.Storage == EMaterialRenderFieldStorage::Resource
                    && (B.Name == GetMaterialTextureParameterName(F.CompactIndex) || B.Name == GetMaterialSamplerParameterName(F.CompactIndex)))
                    B.ResourceIndex = F.CompactIndex;
            for (uint32 I = 0; I < Collections.size(); ++I)
                if (B.Name == GetMaterialCollectionParameterName(I)) B.ResourceIndex = I;
        }
        return Result;
    }
    auto GetMaterialParameterMapIdentity(const FMaterialShaderEntryRequest& Entry, const FShaderCode& Code) -> FXxHash128
    {
        FXxHash128Builder Hash;
        const auto Product = Entry.Mesh ? GetMaterialShaderIdentityHash(*Entry.Mesh) : GetMaterialShaderIdentityHash(Entry.Shader);
        Hash.UpdateValue(Product.HashLow); Hash.UpdateValue(Product.HashHigh);
        Hash.UpdateValue(Entry.ParameterSchemaIdentity.HashLow); Hash.UpdateValue(Entry.ParameterSchemaIdentity.HashHigh);
        Hash.UpdateValue(Code.CodeHash.HashLow); Hash.UpdateValue(Code.CodeHash.HashHigh);
        Hash.UpdateValue(Code.Target.SettingsDigest.HashLow); Hash.UpdateValue(Code.Target.SettingsDigest.HashHigh);
        auto Text = [&](std::string_view Value) { Hash.UpdateValue(static_cast<uint32>(Value.size())); Hash.Update(Value); };
        for (const auto& B : Entry.Bindings) {
            Text(B.Name); Text(B.QualifiedName); Text(B.OwnerName);
            Hash.UpdateValue(B.ResourceIndex); Hash.UpdateValue(B.SetIndex); Hash.UpdateValue(B.BindingIndex);
            Hash.UpdateValue(static_cast<uint8>(B.Type)); Hash.UpdateValue(static_cast<uint8>(B.RuntimeType));
            Hash.UpdateValue(B.ArraySize); Hash.UpdateValue(static_cast<uint8>(B.Scope));
            Hash.UpdateValue(static_cast<uint8>(B.ResourceShape)); Hash.UpdateValue(B.ConstantBufferSize);
            const auto Reflected = std::ranges::find(Code.Reflection.ResourceBindings, B.Name, &FShaderResourceBinding::Name);
            if (Reflected != Code.Reflection.ResourceBindings.end()) {
                Hash.UpdateValue(static_cast<uint32>(Reflected->ConstantMembers.size()));
                for (const auto& M : Reflected->ConstantMembers) { Text(M.Name); Hash.UpdateValue(M.Offset); Hash.UpdateValue(M.Size); }
            }
        }
        return Hash.Finalize();
    }

}
