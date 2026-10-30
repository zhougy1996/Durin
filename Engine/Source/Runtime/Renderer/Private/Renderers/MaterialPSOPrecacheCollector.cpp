#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "Materials/MaterialRenderProxy.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "RenderingThread.h"

namespace Durin::RendererPrivate
{
	namespace
	{
		struct FCollectedMaterialPSOs
		{
			std::vector<std::unique_ptr<FMeshPassProcessor>> Processors;
			std::array<size_t, 2> Next{};
			FMaterialResource Material;
		};
	}
	auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
		bool bSupportsGBuffer, const FSceneRenderPreparationConfig& Config) -> std::vector<FMaterialPSOPrecachePass>
	{
		std::vector<FMaterialPSOPrecachePass> Result;
		for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
		{
			const auto* Processor = &FPassProcessorManager::GetMeshPassProcessor(Pass);
			if (!Processor->IsEnabled(Config) || !Processor->IsMeshRelevant(Params.Material, Params.PrecachePSOParams.Participation, bSupportsGBuffer)) continue;
			for (const auto& Variant : Processor->GetPSOPrecacheVariants(Config, Params.PrecachePSOParams))
				Result.push_back({Processor->GetPass(), Variant.Raster, Variant.bRequired});
		}
		std::stable_sort(Result.begin(), Result.end(), [](const auto& A, const auto& B) { return A.bRequired > B.bRequired; });
		return Result;
	}
	auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
		const FMeshVertexFactoryImplementation& Factory, const FSceneRenderPreparationConfig& Config) -> std::vector<FMaterialPSOPrecachePass>
	{
		auto Result = SelectMaterialPSOPrecachePasses(Params,
			Factory.GetShaderType(FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).GetVertexFactoryPass()) != nullptr, Config);
		std::erase_if(Result, [&](const auto& Entry) { return !FPassProcessorManager::GetMeshPassProcessor(Entry.Pass).SupportsVertexFactory(Factory); });
		return Result;
	}
	auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params, FMaterialPSOPrecacheCollection& Collection,
		const FRenderResourceGeneration& Generation) -> std::expected<bool, std::string>
	{
		CheckRenderingThread();
		if (!Params.PreparationConfig.Version || Params.PreparationConfig.FeatureLevel != Params.FeatureLevel
			|| !Params.CompiledProgram || Params.CompiledProgram->FeatureLevel != Params.FeatureLevel)
			return std::unexpected(std::string("Material PSO collection requires a matching Renderer snapshot and accepted program."));
		if (!Collection.Resources)
		{
			auto Resources = std::make_shared<FCollectedMaterialPSOs>();
			for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
			{
				auto Processor = FPassProcessorManager::CreateMeshPassProcessor(Pass, {Generation, Params.Priority});
				if (!Processor) return std::unexpected(std::string("Registered mesh pass factory failed to create a collector."));
				Resources->Processors.push_back(std::move(Processor));
			}
			FMaterialStaticProperties Properties;
			Properties.BlendMode = static_cast<EMaterialBlendMode>(Params.Material.ShaderMap.BlendMode.Value);
			Properties.ShadingModel = static_cast<EMaterialShadingModel>(Params.Material.ShaderMap.ShadingModel.Value);
			Properties.OpacityMaskThreshold = Params.Material.ShaderMap.OpacityMaskThreshold;
			Properties.bTwoSided = Params.Material.bTwoSided;
			Properties.DepthWritePolicy = Params.Material.DepthWritePolicy;
			Resources->Material.StaticProperties = Properties;
			Resources->Material.CompiledProgram = Params.CompiledProgram;
			Collection.Resources = std::move(Resources);
		}
		auto Resources = std::static_pointer_cast<FCollectedMaterialPSOs>(Collection.Resources);
		for (size_t Group = 0; Group < 2; ++Group)
		{
			if (Group == 1) Collection.bRequiredPSOsCollected = true;
			while (Resources->Next[Group] < Resources->Processors.size())
			{
				auto& Processor = *Resources->Processors[Resources->Next[Group]];
				Processor.SetCollectRequiredPSOs(Group == 0);
				Processor.CollectPSOInitializers(Params.PreparationConfig, Resources->Material,
					Params.VertexFactoryData, Params.PrecachePSOParams, Collection.PSOInitializers);
				const auto Result = Processor.GetPSOCollectionResult();
				if (!Result) return std::unexpected(Result.error());
				if (!*Result) return false;
				++Resources->Next[Group];
			}
		}
		return true;
	}
}
