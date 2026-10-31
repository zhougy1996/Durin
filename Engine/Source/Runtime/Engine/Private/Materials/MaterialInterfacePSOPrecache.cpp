#include "Materials/MaterialInterface.h"
#include "Materials/MaterialShaderMap.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"
#include "PSOPrecacheComponent.h"
#include "Rendering/SceneInterface.h"
#include "RenderingThread.h"

namespace Durin
{
	auto DMaterialInterface::PrecachePSOs(ERHIFeatureLevel FeatureLevel,
		std::span<const FPSOPrecacheVertexFactoryData> VertexFactoryDataList, const FPSOPrecacheParams& PrecacheParams,
		EPSOPrecachePriority Priority, std::vector<FMaterialPSOPrecacheRequestID>& OutMaterialPSORequestIDs) -> std::expected<void, std::string>
	{
		CheckGameThread();
		auto* Renderer = GEngine ? GEngine->GetRendererModule() : nullptr;
		if (!Renderer) return std::unexpected(std::string("Material PSO precache requires an initialized Renderer."));
		return PrecachePSOs(*Renderer, FeatureLevel, VertexFactoryDataList, PrecacheParams, Priority, OutMaterialPSORequestIDs);
	}
	auto DMaterialInterface::PrecachePSOs(IRendererModule& Renderer, ERHIFeatureLevel FeatureLevel,
		std::span<const FPSOPrecacheVertexFactoryData> VertexFactoryDataList, const FPSOPrecacheParams& PrecacheParams,
		EPSOPrecachePriority Priority, std::vector<FMaterialPSOPrecacheRequestID>& OutMaterialPSORequestIDs) -> std::expected<void, std::string>
	{
		CheckGameThread();
		if (IsPendingKill() || VertexFactoryDataList.empty() || VertexFactoryDataList.size() > FPSOPrecacheComponentData::MaximumRequests)
			return std::unexpected(std::string("Material PSO precache requires a live material and a bounded Vertex Factory list."));
		if (FeatureLevel > FSceneInterface::GetDefaultFeatureLevel()) return std::unexpected(std::string("Material PSO feature level exceeds RHI capability."));
		const auto Material = GetMaterialResource(FeatureLevel);
		if (!Material.HasValidGameThreadShaderMap())
			return std::unexpected(std::string("Material PSO precache requires an accepted material program."));
		const auto Properties = Material.GetStaticProperties();
		const auto& ShaderMap = Material.CompiledProgram->GetShaderMap();
		const FMaterialPlanningPassIdentity Identity{
			.ShaderMap = {.RenderLayout = ShaderMap.Layout.Identity, .ProgramIdentity = ShaderMap.ShaderMapId,
				.BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel,
				.OpacityMaskThreshold = Properties.OpacityMaskThreshold},
			.bTwoSided = Properties.bTwoSided, .DepthWritePolicy = Properties.DepthWritePolicy};
		std::vector<FMaterialPSOPrecacheRequestID> Requests;
		for (const auto& Factory : VertexFactoryDataList)
		{
			auto Request = Renderer.PrecacheMaterialPSOs({Identity, Material.CompiledProgram, Factory, PrecacheParams, Priority, FeatureLevel});
			if (!Request.GetValue()) return std::unexpected(Request.GetDiagnostic());
			Requests.push_back(PSOPrecacheOwner.Observe(Request));
		}
		OutMaterialPSORequestIDs = std::move(Requests);
		return {};
	}
}
