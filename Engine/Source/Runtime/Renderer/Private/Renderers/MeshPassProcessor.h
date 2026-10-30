#pragma once

#include "CoreMinimal.h"
#include "Materials/PSOPrecacheMaterial.h"
#include "Renderers/MeshPipelineDescription.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"

namespace Durin
{
	class FShaderType;
	class FMeshMaterialShaderMap;
}

namespace Durin::RendererPrivate
{
	class FMeshVertexFactoryImplementation;
	struct FMeshPassFragmentShader
	{
		FShaderType* Type = nullptr;
		FRHIShader* (*Resolve)(const FMeshMaterialShaderMap&) = nullptr;
		const char* FixedModule = nullptr;
		const char* FixedRequest = nullptr;
	};
	struct FMeshPSOPrecacheVariant
	{
		FMeshPipelineRasterOptions Raster;
		bool bRequired = false;
	};
	struct FMeshPSOCollectorContext
	{
		FRenderResourceGeneration Generation;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
	};

	// One pass owns eligibility, shader selection and pipeline description for both
	// drawing and collection. Geometry visibility and native submission stay outside.
	class FMeshPassProcessor : public IPSOCollector
	{
	public:
		RENDERER_API explicit FMeshPassProcessor(FMeshPSOCollectorContext Context = {});
		RENDERER_API ~FMeshPassProcessor() override;
		virtual auto GetPass() const -> EMeshPipelinePass = 0;
		// Collection instances belong to one immutable material/factory/config request.
		virtual auto CreatePSOCollector(FMeshPSOCollectorContext Context) const -> std::unique_ptr<FMeshPassProcessor> = 0;
		virtual auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool = 0;
		RENDERER_API auto IsMeshRelevant(const FMaterialPlanningPassIdentity& Material,
			const FPrimitiveRenderParticipation& Participation = {}, bool bSupportsGBuffer = true) const -> bool;
		RENDERER_API auto SupportsVertexFactory(const FMeshVertexFactoryImplementation& Factory) const -> bool;
		virtual auto GetVertexFactoryPass(bool bGPUCulling = false) const -> uint32 = 0;
		virtual auto GetFragmentShader(FMaterialShaderBlendModeKey BlendMode) const -> FMeshPassFragmentShader = 0;
		virtual auto DescribeMeshDraw(const FMeshPipelineState& State,
			FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
			FMaterialShaderBlendModeKey BlendMode = EMaterialBlendMode::Opaque, bool bReversedZ = false) const -> FMeshPipelineDescription = 0;
		virtual auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config,
			const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> = 0;

		RENDERER_API auto CollectPSOInitializers(const FSceneRenderPreparationConfig& SceneConfig,
			const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers) -> void override;
		// Async adaptation around the UE-shaped append interface. Required collection
		// finishes across all passes before any optional work is allowed to fail.
		auto SetCollectRequiredPSOs(bool bRequired) -> void { CollectionPhase = bRequired; }
		RENDERER_API auto GetPSOCollectionResult() const -> std::expected<bool, std::string>;
	private:
		struct FState;
		std::unique_ptr<FState> State;
		FMeshPSOCollectorContext Context;
		// Unset is the ordinary UE contract: collect every applicable initializer.
		std::optional<bool> CollectionPhase;
		auto CollectPhase(const FSceneRenderPreparationConfig& SceneConfig,
			const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers, bool bRequired) -> void;
	};

	RENDERER_API auto GetMeshPassProcessors() -> std::span<const FMeshPassProcessor* const>;
	RENDERER_API auto GetMeshPassProcessor(EMeshPipelinePass Pass) -> const FMeshPassProcessor&;
	auto GetGBufferMeshPassFragmentShader() -> FMeshPassFragmentShader;
	auto GetHitProxyMeshPassFragmentShader() -> FMeshPassFragmentShader;
}
