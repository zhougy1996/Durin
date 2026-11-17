#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/MeshVertexFactory.h"

#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/ViewPreparationMath.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/GBufferRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/MaterialResourceCacheKeyHash.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/MaterialShader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/LocalVertexFactory.h"

namespace Durin::RendererPrivate
{
    class FLocalVFParameterOwner {
    public:
        DURIN_BEGIN_SHADER_PARAMETERS(FLocalVFParameterOwner)
#include "Material/GPUSceneParameters.inl"
        DURIN_END_SHADER_PARAMETERS();
        static auto GetParametersMetadata() -> const FShaderParametersMetadata*
        { return GetShaderParametersMetadataOrNull<FLocalVFParameterOwner>(); }
    };
    class FSplineVFParameterOwner {
    public:
        DURIN_BEGIN_SHADER_PARAMETERS(FSplineVFParameterOwner)
#include "Material/GPUSceneParameters.inl"
#include "Material/SplineParameters.inl"
        DURIN_END_SHADER_PARAMETERS();
        static auto GetParametersMetadata() -> const FShaderParametersMetadata*
        { return GetShaderParametersMetadataOrNull<FSplineVFParameterOwner>(); }
    };
	inline auto GetLocalVertexFactoryShaderType() -> const FVertexFactoryType&
	{
		static const FVertexFactoryType Type("LocalVertexFactory",
			EVertexFactoryFlags::SupportsPSOPrecaching | EVertexFactoryFlags::SupportsPositionOnly,
			&FLocalVertexFactory::GetPSOPrecacheVertexFetchElements,
			{.ShaderModules = {"VertexFactory.LocalVertexFactory"}, .MaterialEntryInclude = "VertexFactory/LocalMaterialEntry.slang",
             .ParametersMetadata = FLocalVFParameterOwner::GetParametersMetadata()});
		return Type;
	}

	inline auto GetSplineVertexFactoryShaderType() -> const FVertexFactoryType&
	{
		static const FVertexFactoryType Type("SplineVertexFactory",
			EVertexFactoryFlags::SupportsPSOPrecaching | EVertexFactoryFlags::SupportsPositionOnly,
			&FLocalVertexFactory::GetPSOPrecacheVertexFetchElements,
			{.ShaderModules = {"VertexFactory.LocalVertexFactory", "VertexFactory.SplineMeshVertexFactory"}, .MaterialEntryInclude = "VertexFactory/SplineMaterialEntry.slang",
			 .ModifyEnvironment = [](const FShaderPermutationParameters&, FShaderCompileOptions& O) {
				if (!std::ranges::any_of(O.Macros, [](const auto& M) { return M.Name == "DURIN_SPLINE_MESH"; })) O.Macros.emplace_back("DURIN_SPLINE_MESH", "1");
			 }, .ParametersMetadata = FSplineVFParameterOwner::GetParametersMetadata()});
		return Type;
	}


	inline constexpr uint32 MaterialMeshPassForward = 0;
	inline constexpr uint32 MaterialMeshPassGBuffer = 1;
	inline constexpr uint32 MaterialMeshPassShadow = 2;
	inline constexpr size_t MaterialShaderMapCacheEntryBudget = 256;
	inline constexpr size_t MaterialPipelineCacheEntryBudget = 512;
	inline constexpr size_t MaterialShaderMapCacheByteBudget = 1024 * 1024;
	inline constexpr size_t MaterialPipelineCacheByteBudget = 2 * 1024 * 1024;

	inline auto InitializeMaterialShaderMap(
		const FShaderType& VertexType,
		const FShaderType& FragmentType,
		const FVertexFactoryType& VertexFactoryType,
		uint32 MeshPassKey,
		const FMeshMaterialShaderMapIdentity& Identity,
		const FRenderResourceGeneration& Generation,
		const FMaterialProgram* MaterialProgram,
		const FShaderCompileOptions& VertexCompileOptions,
		FMeshMaterialShaderMap& OutShaderMap) -> FShaderCompletion
	{
		if (MaterialProgram != nullptr)
		{
			if (!ValidateMaterialProgram(*MaterialProgram))
				return std::unexpected(std::string("Accepted material compiler result is invalid."));
			if (MaterialProgram->GetShaderMap().ShaderMapId != Identity.ProgramIdentity)
				return std::unexpected(std::format(
				"Compiled material program identity '{}' does not match requested identity '{}'.",
				(MaterialProgram->GetShaderMap().ShaderMapId.ToString()), (Identity.ProgramIdentity.ToString())));
			if (MaterialProgram->GetShaderMap().Layout.Identity != Identity.RenderLayout)
				return std::unexpected(std::format("Material layout '{}' version {} does not match '{}' version {}.",
				(MaterialProgram->GetShaderMap().Layout.Identity.Id.ToString()), (MaterialProgram->GetShaderMap().Layout.Identity.Version), (Identity.RenderLayout.Id.ToString()), (Identity.RenderLayout.Version)));
			if (MaterialProgram->GetShaderMap().PassContractVersion != CurrentMaterialPassContractVersion)
				return std::unexpected(std::format("Material pass contract {} does not match {}.", (MaterialProgram->GetShaderMap().PassContractVersion), (CurrentMaterialPassContractVersion)));
		}
		const auto Factory = FindMeshVertexFactory(VertexFactoryType.GetStableKey());
		if (!Factory)
		{
			return std::unexpected(std::format("Mesh Material shader type '{}' requires a Vertex Factory type.", (std::string(VertexType.GetName()))));
		}
		const std::array<const FShaderType*, 2> Types{
			&VertexType, &FragmentType};
		std::span<const FShaderCode> GeneratedStages = MaterialProgram
			? std::span<const FShaderCode>(MaterialProgram->GetShaderMap().CompiledShaders)
			: std::span<const FShaderCode>{};
		// A declared generated use must never fall through to a fixed-source
		// compilation when its geometry or pass was excluded at material build.
		if (MaterialProgram)
		{
			for (const auto Entry : {FragmentType.GetEntryPoint(),
				Factory->UsesGeneratedMaterialVertexStage(MeshPassKey) ? VertexType.GetEntryPoint() : std::string_view{}})
				if (!Entry.empty() && !std::ranges::any_of(GeneratedStages,
					[Entry](const auto& Stage) { return Stage.SourceEntryPoint == Entry; }))
					return std::unexpected(std::format(
				"Accepted material program has no {} stage for type '{}'.",
				(std::string(Entry)), (std::string{})));
		}
		std::span<const FMaterialShaderEntryRequest> GeneratedEntries = MaterialProgram ? std::span<const FMaterialShaderEntryRequest>(MaterialProgram->GetShaderMap().Entries) : std::span<const FMaterialShaderEntryRequest>{};
		std::vector<FMaterialShaderEntryRequest> FactoryCompatibleEntries;
		std::vector<FShaderCode> FactoryCompatibleStages;
		if (!Factory->UsesGeneratedMaterialVertexStage(MeshPassKey))
		{
			for (const auto& Stage : GeneratedStages)
				if (Stage.Frequency != EShaderFrequency::Vertex)
					FactoryCompatibleStages.push_back(Stage);
			GeneratedStages = FactoryCompatibleStages;
			for (const auto& Entry : GeneratedEntries)
				if (Entry.Shader.Frequency != EShaderFrequency::Vertex) FactoryCompatibleEntries.push_back(Entry);
			GeneratedEntries = FactoryCompatibleEntries;
		}
		return FMeshMaterialShaderMap::TryCompile({
			.Identity = Identity,
			.Generation = Generation,
			.Target = MaterialProgram ? MaterialProgram->GetShaderMap().Target
				: std::string("vulkan-spirv-1.5"),
			.VertexFactoryType = &VertexFactoryType,
			.MeshPassKey = MeshPassKey,
			.ShaderTypes = Types,
			.CompileOptions = VertexCompileOptions,
			.FixedShaderRuntimeRequest = Factory->GetRuntimeRequestName(MeshPassKey),
			.FixedFragmentRuntimeRequest = FragmentType.GetName() == "FSurfaceOpaqueShadowFragmentShader" ? "Surface.OpaqueShadow" : FragmentType.GetEntryPoint() == "HitProxyFragmentMain" ? "Surface.HitProxy" : "",
			.GeneratedStages = GeneratedStages,
			.CompiledProgramIdentity = MaterialProgram
				? MaterialProgram->GetShaderMap().ShaderMapId : FMaterialProgramIdentity{},
			.CompiledTarget = MaterialProgram
				? MaterialProgram->GetShaderMap().Target : std::string{},
			.GeneratedEntries = GeneratedEntries,
            .CompiledParameters = MaterialProgram ? MaterialProgram->ShaderMap : nullptr,
			.bCreateRHIShaders = true}, OutShaderMap);
	}

	// The factory supplies geometry parameters; pass/material owners supply their scopes.
	template<typename TShader>
	auto PrepareMeshVertexFactoryParameters(const TMaterialShaderRef<TShader>& Shader,
		const typename TShader::FParameters& Parameters) -> FMeshDrawShaderBindings
	{
		const auto* Content = Shader.GetShader();
		if (!Content || !Shader.GetRHIShader(false)) return {};
		std::vector<FShaderParameterBinding> Bindings(Content->GetParameterBindings().begin(), Content->GetParameterBindings().end());
		// View and material uniforms are supplied by pass/material composition.
		std::erase_if(Bindings, [](const auto& Parameter) {
			return Parameter.Scope == EShaderParameterBindingScope::Pass || Parameter.OwnerName == "Material";
		});
		FMeshDrawShaderBindings Result;
		Result.Parameters = ResolveShaderParameterResources(*TShader::GetParametersMetadata(), Bindings, &Parameters);
		if (std::ranges::any_of(Result.Parameters, [](const auto& Parameter) { return !Parameter.Resource; })) return {};
		Result.Shader = Shader.GetRHIShader(false);
		return Result;
	}

	inline auto PrepareMeshVertexMaterial(const FMeshVertexShaderBinding& Shader,
		FMeshDrawShaderBindings Base,
		const FResolvedSurfaceMaterial& Material, const FRHIUniformBufferRange& Uniform,
		const FRHIUniformBufferRange& View, std::span<const FRHIUniformBufferRange> Collections = {}, bool bStableOnly = false)
		-> FMeshDrawShaderBindings
	{
		if (!Base) return {};
		FMeshDrawShaderBindings Bindings(std::move(Base));
        auto Remaining = std::vector<FMaterialShaderBinding>(Shader.GetAcceptedParameterBindings().begin(), Shader.GetAcceptedParameterBindings().end());
        std::erase_if(Remaining, [&](const auto& B) {
            if (bStableOnly && B.Scope == EShaderParameterBindingScope::Pass) return true;
            return std::ranges::any_of(Bindings.GetResources(), [&](const auto& R) {
                return R.SetIndex == B.SetIndex && R.BindingIndex == B.BindingIndex;
            });
        });
        if (Remaining.empty()) return Bindings;
        FCompiledSurfaceBindingLayout Layout(Remaining);
		FMeshDrawShaderBindings Additional;
		if (!PrepareCompiledSurfaceMaterial(Shader.GetRHIShader(false), Layout, Material, Uniform,
			{}, {}, View, {}, Collections, Additional)) return {};
		std::vector<FRHIShaderParameterResource> Resources(Bindings.GetResources().begin(), Bindings.GetResources().end());
		Resources.insert(Resources.end(), Additional.GetResources().begin(), Additional.GetResources().end());
		Bindings.Parameters = std::move(Resources);
		if (Additional.InstanceIdsAllocation) Bindings.InstanceIdsAllocation = Additional.InstanceIdsAllocation;
		return Bindings;
	}

	class FStaticMeshVertexShader : public FMeshMaterialShader
	{
	public:
			static auto GetMaterialCompilationDescription() -> FMaterialShaderEntryDescriptor
			{ return {.Entry = EMaterialShaderEntry::LocalVertex, .GeometryUsage = 1, .PassUsage = 15, .VertexFactory = []() -> const FVertexFactoryType* { return &GetLocalVertexFactoryShaderType(); }}; }

		DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshVertexShader)
				DURIN_MATERIAL_PARAMETER(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBufferDynamic, true, Draw);
#include "Material/ViewParameters.inl"
#include "Material/GPUSceneParameters.inl"
			DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_MESH_MATERIAL_SHADER(
			FStaticMeshVertexShader,
			FMeshMaterialShader,
			"/Engine/StaticMeshBasePass",
			EShaderFrequency::Vertex,
			"VertexMain"
		);
	};

	class FSplineMeshVertexShader : public FMeshMaterialShader
	{
	public:
			static auto GetMaterialCompilationDescription() -> FMaterialShaderEntryDescriptor
			{ return {.Entry = EMaterialShaderEntry::SplineVertex, .GeometryUsage = 2, .PassUsage = 15, .VertexFactory = []() -> const FVertexFactoryType* { return &GetSplineVertexFactoryShaderType(); }}; }

		static auto ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& Options) -> void
		{
			GetSplineVertexFactoryShaderType().ModifyCompilationEnvironment(Parameters, Options);
		}
		DURIN_BEGIN_SHADER_PARAMETERS(FSplineMeshVertexShader)
				DURIN_MATERIAL_PARAMETER(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBufferDynamic, true, Draw);
#include "Material/ViewParameters.inl"
#include "Material/GPUSceneParameters.inl"
#include "Material/SplineParameters.inl"
			DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_MESH_MATERIAL_SHADER(FSplineMeshVertexShader, FMeshMaterialShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Vertex, "SplineVertexMain");
	};


	struct FSplineMeshUniform
	{
		FVector4f StartPosition{0.0f};
		FVector4f StartTangent{0.0f};
		FVector4f EndPosition{0.0f};
		FVector4f EndTangent{0.0f};
		FVector4f StartEndScale{1.0f};
		FVector4f StartEndOffset{0.0f};
		FVector4f RollUpAxis{0.0f};
		FVector4f SourceRangePolicy{0.0f};
	};

	inline auto MakeSplineMeshUniform(const FSplineMeshParams& Params) -> FSplineMeshUniform
	{
		FSplineMeshUniform Result;
		Result.StartPosition = FVector4f(FVector3f(Params.StartPosition), static_cast<float>(Params.StartRollRadians));
		Result.StartTangent = FVector4f(FVector3f(Params.StartTangent), static_cast<float>(Params.EndRollRadians));
		Result.EndPosition = FVector4f(FVector3f(Params.EndPosition), 0.0f);
		Result.EndTangent = FVector4f(FVector3f(Params.EndTangent), 0.0f);
		Result.StartEndScale = FVector4f(FVector2f(Params.StartScale), FVector2f(Params.EndScale));
		Result.StartEndOffset = FVector4f(FVector2f(Params.StartOffset), FVector2f(Params.EndOffset));
		Result.RollUpAxis = FVector4f(FVector3f(Params.SplineUpDirection), static_cast<float>(Params.ForwardAxis));
		Result.SourceRangePolicy = FVector4f(static_cast<float>(Params.SourceForwardMin), static_cast<float>(Params.SourceForwardMax), Params.Interpolation == ESplineMeshInterpolation::SmoothStep ? 1.0f : 0.0f, 0.0f);
		return Result;
	}

	inline auto GetIdentityText(
		const FMeshMaterialShaderMapIdentity& Identity
	) -> std::string
	{
		return std::format(
			"layout-version={},layout-id={},program={},blend={},shading={},mask-bits={}",
			Identity.RenderLayout.Version,
			Identity.RenderLayout.Id.ToString(),
			Identity.ProgramIdentity.ToString(),
			static_cast<uint8>(Identity.BlendMode),
			static_cast<uint8>(Identity.ShadingModel),
			std::bit_cast<uint32>(Identity.OpacityMaskThreshold)
		);
	}

	inline auto GetIdentityText(
		const FMaterialPlanningPassIdentity& Identity
	) -> std::string
	{
		return std::format(
			"{},two-sided={},depth-write={}",
			GetIdentityText(Identity.ShaderMap),
			Identity.bTwoSided,
			static_cast<uint8>(Identity.DepthWritePolicy)
		);
	}

	inline auto GetIdentityText(
		const FEffectiveMeshPipelineKey& Identity
	) -> std::string
	{
		return std::format(
			"{},vertex-domain={},polygon={},cull={},front={},depth-test={},depth-write={},depth-compare={},blend={},color-src={},color-dst={},color-op={},alpha-src={},alpha-dst={},alpha-op={},write-mask={}",
			GetIdentityText(Identity.Material),
			static_cast<uint8>(Identity.VertexDomain),
			static_cast<uint8>(Identity.Rasterizer.PolygonMode),
			static_cast<uint8>(Identity.Rasterizer.CullMode),
			static_cast<uint8>(Identity.Rasterizer.FrontFace),
			Identity.Depth.bEnableTest,
			Identity.Depth.bEnableWrite,
			static_cast<uint8>(Identity.Depth.CompareOp),
			Identity.ColorBlend.bEnable,
			static_cast<uint8>(Identity.ColorBlend.SrcColorFactor),
			static_cast<uint8>(Identity.ColorBlend.DstColorFactor),
			static_cast<uint8>(Identity.ColorBlend.ColorOp),
			static_cast<uint8>(Identity.ColorBlend.SrcAlphaFactor),
			static_cast<uint8>(Identity.ColorBlend.DstAlphaFactor),
			static_cast<uint8>(Identity.ColorBlend.AlphaOp),
			static_cast<uint8>(Identity.ColorBlend.ColorWriteMask)
		);
	}

	inline auto MakeMeshDrawSortKey(
		EMeshBasePass Pass,
		const FEffectiveMeshPipelineKey& PipelineKey,
		const FMaterialRenderRepresentation& Representation,
		uint32 NumVertices,
		const FVertexDeclarationElementList& Elements,
		const std::array<uint32, 6>& Geometry,
		uint64 PrimitiveId,
		uint32 LODIndex,
		uint64 SectionIndex,
		uint64 BatchId = 0
	) -> FMeshDrawSortKey
	{
		FMeshDrawSortKey Result;
		const FMaterialPlanningPassIdentity& Material = PipelineKey.Material;
		const FMeshMaterialShaderMapIdentity& Shader = Material.ShaderMap;
		const FGuid& LayoutId = Shader.RenderLayout.Id;
		const FXxHash128& ProgramDigest = Shader.ProgramIdentity.Digest;
		Result.Pipeline = {
			static_cast<uint32>(Pass), Shader.RenderLayout.Version,
			LayoutId.A, LayoutId.B, LayoutId.C, LayoutId.D,
			static_cast<uint32>(ProgramDigest.HashLow),
			static_cast<uint32>(ProgramDigest.HashLow >> 32),
			static_cast<uint32>(ProgramDigest.HashHigh),
			static_cast<uint32>(ProgramDigest.HashHigh >> 32),
			static_cast<uint32>(Shader.BlendMode),
			static_cast<uint32>(Shader.ShadingModel),
			std::bit_cast<uint32>(Shader.OpacityMaskThreshold),
			Material.bTwoSided ? 1u : 0u,
			static_cast<uint32>(Material.DepthWritePolicy),
			static_cast<uint32>(PipelineKey.VertexDomain),
			static_cast<uint32>(PipelineKey.Rasterizer.PolygonMode),
			static_cast<uint32>(PipelineKey.Rasterizer.CullMode),
			static_cast<uint32>(PipelineKey.Rasterizer.FrontFace),
			PipelineKey.Depth.bEnableTest ? 1u : 0u,
			PipelineKey.Depth.bEnableWrite ? 1u : 0u,
			static_cast<uint32>(PipelineKey.Depth.CompareOp),
			PipelineKey.ColorBlend.bEnable ? 1u : 0u,
			static_cast<uint32>(PipelineKey.ColorBlend.SrcColorFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.DstColorFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.ColorOp),
			static_cast<uint32>(PipelineKey.ColorBlend.SrcAlphaFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.DstAlphaFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.AlphaOp),
			static_cast<uint32>(PipelineKey.ColorBlend.ColorWriteMask),
			static_cast<uint32>(PipelineKey.FactoryKey.HashValue), static_cast<uint32>(PipelineKey.FactoryKey.HashValue >> 32),
			static_cast<uint32>(PipelineKey.LayoutKey.HashValue), static_cast<uint32>(PipelineKey.LayoutKey.HashValue >> 32),
			static_cast<uint32>(PipelineKey.Topology)
		};
		Result.MaterialUniform.Representation = Representation;

		if (NumVertices != 0)
		{
			size_t LastElement = Elements.size();
			while (LastElement > 0 && Elements[LastElement - 1].Type == EVertexElementType::None) --LastElement;
			Result.VertexFactory.resize(1 + LastElement * 6);
			Result.VertexFactory[0] = NumVertices;
			for (size_t Index = 0; Index < LastElement; ++Index)
			{
				const FVertexElement& Element = Elements[Index];
				const size_t Base = 1 + Index * 6;
				if (Element.Type == EVertexElementType::None) continue;
				Result.VertexFactory[Base] = Element.StreamIndex;
				Result.VertexFactory[Base + 1] = Element.Offset;
				Result.VertexFactory[Base + 2] =
					static_cast<uint32>(Element.Type);
				Result.VertexFactory[Base + 3] = Element.AttributeIndex;
				Result.VertexFactory[Base + 4] = Element.Stride;
				Result.VertexFactory[Base + 5] = static_cast<uint32>(Element.InputRate);
			}
		}
		Result.Geometry = Geometry;
		Result.PrimitiveId = PrimitiveId;
		Result.BatchId = BatchId;
		Result.SelectedLODIndex = LODIndex;
		Result.SectionIndex = SectionIndex;
		return Result;
	}

	inline auto MakeStaticMeshDrawSortKey(
		const FPreparedStaticMeshPrimitive& Primitive,
		const FMeshDrawDescription& Draw
	) -> FMeshDrawSortKey
	{
		const auto Elements = Primitive.CollectedBinding != nullptr ? Primitive.CollectedBinding->DeclarationElements : FVertexDeclarationElementList{};
		const std::array<uint32, 6> Geometry{Draw.Geometry.FirstElement, Draw.Geometry.ElementCount,
			Draw.Geometry.MinVertexIndex, Draw.Geometry.MaxVertexIndex, Draw.MaterialSlotDiagnostic,
			Draw.Indices.Range.Stride ? static_cast<uint32>(Draw.Indices.Range.BufferBytes / Draw.Indices.Range.Stride) : 0u};
		return MakeMeshDrawSortKey(Draw.Pass, Draw.PipelineKey, Draw.Material.Representation, Primitive.CollectedBinding != nullptr ? Primitive.CollectedBinding->NumVertices : 0u, Elements, Geometry, Primitive.PrimitiveId.Value, Primitive.SelectedLODIndex, Draw.SectionIndex, Primitive.BatchId);
	}

} // namespace Durin::RendererPrivate
