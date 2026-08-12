#include "Engine/TerrainSceneProxy.h"

#include "Threading/RunnableThread.h"

namespace Durin
{
	FTerrainSceneProxy::FTerrainSceneProxy(
		std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
		uint64 InRevision, double InSpacingX, double InSpacingY,
		double InHeightScale, double InHeightOffset,
		std::vector<FTerrainPatchDescriptor> InPatches,
		FBox InLocalBounds, FMaterialRenderProxyRef InMaterial,
		uint64 InMaterialComponentRevision)
		: Payload(std::move(InPayload)), Revision(InRevision),
		  SpacingX(InSpacingX), SpacingY(InSpacingY), HeightScale(InHeightScale),
		  HeightOffset(InHeightOffset), Patches(std::move(InPatches)),
		  LocalBounds(InLocalBounds), Material(std::move(InMaterial)),
		  MaterialComponentRevision(InMaterialComponentRevision)
	{
	}

	auto FTerrainSceneProxy::ResolveMaterialRenderData_RenderThread() const
		-> const FMaterialRenderData&
	{
		if (Material) return Material->Resolve_RenderThread();
		RecordMaterialFallbackReason(EMaterialFallbackReason::MissingProxy);
		return GetErrorMaterialRenderData();
	}

	auto FTerrainSceneProxy::UpdateMaterialBinding_RenderThread(
		const FMaterialRenderProxyBindingUpdate& Update) -> bool
	{
		CheckRenderingThread();
		if (Update.ComponentRevision <= MaterialComponentRevision
			|| Update.SlotIndex != 0) return false;
		Material = Update.MaterialProxy;
		MaterialComponentRevision = Update.ComponentRevision;
		RecordMaterialBindingUpdate();
		return true;
	}
}
