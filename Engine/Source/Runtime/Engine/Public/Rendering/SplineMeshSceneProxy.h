#pragma once

#include "Rendering/PrimitiveSceneProxy.h"
#include "Spline/SplineTypes.h"

namespace Durin
{
	struct FStaticMeshRenderData;

	// Captures revisioned spline deformation state copied for render-thread use.
	struct FSplineMeshRenderDynamicData
	{
		FSplineMeshParams Params;
		FBox LocalBounds;
		uint64 Revision = 0;
	};

	// Couples static-mesh resources with revisioned spline deformation and material state.
	class FSplineMeshSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API FSplineMeshSceneProxy(const FStaticMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			uint64 InMaterialComponentRevision,
			FSplineMeshRenderDynamicData InDynamicData);
		auto GetKind() const -> EPrimitiveSceneProxyKind override { return EPrimitiveSceneProxyKind::SplineMesh; }
		auto GetRenderData() const -> const FStaticMeshRenderData* { return RenderData; }
		auto GetDynamicData() const -> const FSplineMeshRenderDynamicData& { return DynamicData; }
		auto GetAcceptedDynamicUpdateCount() const -> uint64 { return AcceptedDynamicUpdateCount; }
		auto GetLocalBounds() const -> FBox override { return DynamicData.LocalBounds; }
		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const -> const FMaterialRenderProxyRef&;
		ENGINE_API auto ResolveMaterialRenderData_RenderThread(uint32 SlotIndex) const -> const FMaterialRenderData&;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(const FMaterialRenderProxyBindingUpdate& Update) -> bool override;
		ENGINE_API auto UpdateDynamicData_RenderThread(FSplineMeshRenderDynamicData InDynamicData) -> bool;

	private:
		const FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		uint64 MaterialComponentRevision = 0;
		FSplineMeshRenderDynamicData DynamicData;
		uint64 AcceptedDynamicUpdateCount = 0;
	};
}
