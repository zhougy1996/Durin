#include "Materials/MaterialBindingSchema.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
    namespace
    {
        constexpr FMaterialBindingDefinition Definitions[]{
#define DURIN_MATERIAL_BINDING_SCHEMA 1
#define DURIN_MATERIAL_BINDING(Role, Name, SlangType, Reflected, Runtime, Set, Slot, Stride, Stages, Entries) \
            {EMaterialBindingRole::Role, #Name, #SlangType, ERHIBindingType::Reflected, ERHIBindingType::Runtime, Set, Slot, Stride, Stages, Entries},
#include "Material/MaterialBindingSchema.inl"
#undef DURIN_MATERIAL_BINDING
#undef DURIN_MATERIAL_BINDING_SCHEMA
        };
        static_assert(std::size(Definitions) == size_t(EMaterialBindingRole::Count));
    }
    auto GetMaterialBindingDefinitions() -> std::span<const FMaterialBindingDefinition> { return Definitions; }
    auto GetMaterialBindingDefinition(EMaterialBindingRole Role) -> const FMaterialBindingDefinition&
    {
        const auto Found = std::ranges::find(Definitions, Role, &FMaterialBindingDefinition::Role);
        check(Found != std::end(Definitions)); return *Found;
    }
    auto GetMaterialBindingSlot(EMaterialBindingRole Role, uint32 Index) -> uint32
    { const auto& D = GetMaterialBindingDefinition(Role); return D.BindingIndex + D.Stride * Index; }
    auto GetMaterialRuntimeBindingType(EMaterialBindingRole Role, EMaterialDomain Domain) -> ERHIBindingType
    {
        const auto& D = GetMaterialBindingDefinition(Role);
        // Standalone fullscreen pipelines use reflected uniform descriptors;
        // typed mesh/pass maps apply the catalog's dynamic-uniform policy.
        return Domain == EMaterialDomain::PostProcess ? D.ReflectionType : D.RuntimeType;
    }
    auto GetMaterialBindingName(EMaterialBindingRole Role, uint32 Index) -> std::string
    { const auto& D = GetMaterialBindingDefinition(Role); return D.Stride ? std::format("{}{}", D.Name, Index) : std::string(D.Name); }
    auto GenerateMaterialBindingDeclaration(EMaterialBindingRole Role, uint32 Index, std::string_view Type) -> std::string
    {
        const auto& D = GetMaterialBindingDefinition(Role);
        return std::format("[[vk::binding({}, {})]] {} {};\n", GetMaterialBindingSlot(Role, Index), D.SetIndex,
            Type.empty() ? D.SlangType : Type, GetMaterialBindingName(Role, Index));
    }
    auto DescribeMaterialShaderBindings(const FShaderReflectionData& Reflection)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>
    {
        if (Reflection.ResourceBindings.size() > 256) return std::unexpected("Material binding description exceeds the supported bound.");
        std::vector<FMaterialShaderBinding> Result;
        std::set<std::pair<uint32, uint32>> Seen;
        for (const auto& B : Reflection.ResourceBindings)
        {
            if (!Seen.emplace(B.SetIndex, B.BindingIndex).second)
                return std::unexpected("Material binding array or duplicate slot is invalid.");
            bool Found = false;
            for (const auto& D : Definitions)
            {
                if (D.SetIndex != B.SetIndex || B.BindingIndex < D.BindingIndex) continue;
                const uint32 Offset = B.BindingIndex - D.BindingIndex;
                if (D.Stride ? Offset % D.Stride != 0 : Offset != 0) continue;
                const uint32 Index = D.Stride ? Offset / D.Stride : 0;
                const uint32 Limit = D.Role == EMaterialBindingRole::Collection ? MaterialParameterCollectionMaxPerMaterial
                    : D.Stride ? MaterialRenderMaxResourceCount : 1;
                if (Index >= Limit || B.ArraySize != D.ArraySize || B.Type != D.ReflectionType || B.Name != GetMaterialBindingName(D.Role, Index)) continue;
                Result.push_back({D.Role, uint16(Index)}); Found = true; break;
            }
            if (!Found) return std::unexpected("Material binding name, kind or allocation is incompatible with the logical schema.");
        }
        std::ranges::sort(Result, [](const auto& A, const auto& B) { return std::tie(A.Role, A.ResourceIndex) < std::tie(B.Role, B.ResourceIndex); });
        return Result;
    }
    auto ValidateMaterialShaderBindings(const FShaderReflectionData& Reflection, EMaterialShaderEntry Entry,
        EShaderFrequency Frequency, uint32 Resources, uint32 Collections)
        -> std::expected<std::vector<FMaterialShaderBinding>, std::string>
    {
        auto Result = DescribeMaterialShaderBindings(Reflection); if (!Result) return Result;
        const uint8 Stage = Frequency == EShaderFrequency::Vertex ? 1 : Frequency == EShaderFrequency::Fragment ? 2 : 0;
        const auto Flags = Frequency == EShaderFrequency::Vertex ? EShaderStageFlags::Vertex : EShaderStageFlags::Fragment;
        if (!Stage || !Reflection.PushConstantRanges.empty() || std::ranges::any_of(Reflection.ResourceBindings,
            [&](const auto& B) { return B.StageFlags != Flags; })) return std::unexpected("Material binding stage is incompatible.");
        for (const auto& B : *Result)
        {
            const auto& D = GetMaterialBindingDefinition(B.Role);
            if (!(D.Stages & Stage) || !(D.Entries & (1u << uint8(Entry)))
                || ((B.Role == EMaterialBindingRole::Texture || B.Role == EMaterialBindingRole::Sampler) && B.ResourceIndex >= Resources)
                || (B.Role == EMaterialBindingRole::Collection && B.ResourceIndex >= Collections))
                return std::unexpected("Material binding is outside the entry's logical resource contract.");
        }
        const auto Has = [&](EMaterialBindingRole Role) { return std::ranges::contains(*Result, Role, &FMaterialShaderBinding::Role); };
        if ((Entry == EMaterialShaderEntry::HitProxy && !Has(EMaterialBindingRole::HitProxy))
            || (Entry == EMaterialShaderEntry::PostProcess && (!Has(EMaterialBindingRole::SceneUniform)
                || !Has(EMaterialBindingRole::SceneInput0) || !Has(EMaterialBindingRole::SceneLinearSampler))))
            return std::unexpected("A mandatory material binding is missing.");
        return Result;
    }
}
