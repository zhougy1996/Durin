#pragma once

#include "Shader/MaterialShader.h"

namespace
{
	using namespace Durin;
    class FM15VFParameterOwner {
    public:
        DURIN_BEGIN_SHADER_PARAMETERS(FM15VFParameterOwner)
#include "Tests/M15VFParameters.inl"
        DURIN_END_SHADER_PARAMETERS();
        static auto GetParametersMetadata() -> const FShaderParametersMetadata*
        { return GetShaderParametersMetadataOrNull<FM15VFParameterOwner>(); }
    };
	std::atomic<bool> UseAlternateFactory = false;
	std::atomic<bool> UseAlternateShaderMacro = false;
	std::atomic<bool> UseAlternateShaderType = false;
	// A registered extension reuses Local geometry code without changing compiler selection.
	auto GetM15TestFactory() -> const Durin::FVertexFactoryType*
	{
		using namespace Durin;
		static const FVertexFactoryType Type("M15TestVertexFactory", EVertexFactoryFlags::None, nullptr,
			{.ShaderModules = {"VertexFactory.LocalVertexFactory"}, .MaterialEntryInclude = "Tests/M15MaterialEntry.slang",
			 .ShouldCompile = [](const FShaderPermutationParameters& P) { return P.Target == VulkanShaderTarget && P.PermutationId == 37; },
			 .ModifyEnvironment = [](const FShaderPermutationParameters& P, FShaderCompileOptions& O) {
				O.Macros.emplace_back("DURIN_M15_VF_PROOF", std::to_string(P.PermutationId));
			 }, .ParametersMetadata = FM15VFParameterOwner::GetParametersMetadata()});
		if (UseAlternateFactory.load()) {
			static const FVertexFactoryType Alternate("M15AlternateVertexFactory", EVertexFactoryFlags::None, nullptr, Type.GetShaderDescription());
			return &Alternate;
		}
		return &Type;
	}
	class FM15TestVertexShader final : public Durin::FMeshMaterialShader
	{
	public:
		static auto GetMaterialCompilationDescription() -> Durin::FMaterialShaderEntryDescriptor
		{ return {.Entry = Durin::EMaterialShaderEntry::LocalVertex, .GeometryUsage = 8, .PassUsage = 1,
			.PermutationId = 37, .VertexFactory = &GetM15TestFactory}; }
		static auto ShouldCompileMaterialPermutation(const Durin::FMaterialShaderPermutationParameters& P) -> bool
		{
			return P.Domain == Durin::EMaterialDomain::Surface && P.Quality == Durin::EMaterialQualityLevel::Low
				&& P.FeatureLevel == Durin::ERHIFeatureLevel::SM5 && P.PermutationId == 37
				&& ((P.ShaderType->GetName() == "FM15AlternateVertexShader") == UseAlternateShaderType.load())
				&& (P.Properties.GeometryUsage & 8) && P.VertexFactoryType == GetM15TestFactory();
		}
		static auto ModifyMaterialCompilationEnvironment(const Durin::FMaterialShaderPermutationParameters& P,
			Durin::FShaderCompileOptions& O) -> void
		{
			O.Macros.emplace_back("DURIN_M15_POLICY_PROOF", std::to_string(uint8(P.Quality)));
			if (UseAlternateShaderMacro.load()) O.Macros.emplace_back("DURIN_M15_ALTERNATE", "1");
		}
        DURIN_BEGIN_SHADER_PARAMETERS(FM15TestVertexShader)
#include "Tests/M15VFParameters.inl"
#include "Material/ViewParameters.inl"
#include "Material/GPUSceneParameters.inl"
        DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_MESH_MATERIAL_SHADER(FM15TestVertexShader, Durin::FMeshMaterialShader,
			"/Engine/StaticMeshBasePass", Durin::EShaderFrequency::Vertex, "VertexMain");
	};
	DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FM15TestVertexShader);

}
