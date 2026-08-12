#pragma once

#include "Engine/FPrimitiveSceneProxy.h"

namespace Durin
{
	struct FStaticMeshRenderData;

	// Couples static-mesh render resources with revisioned per-slot material bindings.
	class FStaticMeshSceneProxy : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API explicit FStaticMeshSceneProxy(
			const FStaticMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			uint64 InMaterialComponentRevision);

		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		auto GetKind() const -> EPrimitiveSceneProxyKind override { return EPrimitiveSceneProxyKind::StaticMesh; }
		ENGINE_API auto GetLocalBounds() const -> FBox override;
		ENGINE_API auto ResolveMaterialRenderData_RenderThread(
			uint32 SlotIndex) const -> const FMaterialRenderData&;
		auto GetNumMaterials() const -> uint32 { return static_cast<uint32>(Materials.size()); }
		auto ResolveMaterialRenderData_RenderThread() const -> const FMaterialRenderData&
		{
			return ResolveMaterialRenderData_RenderThread(0);
		}
		auto GetMaterialComponentRevision() const -> uint64 { return MaterialComponentRevision; }
		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const
			-> const FMaterialRenderProxyRef&;
		ENGINE_API auto UpdateMaterialRenderProxyBinding(
			const FMaterialRenderProxyBindingUpdate& Update) -> void;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool override;

	private:
		// Non-owning borrow bounded by the component render-state lifetime. The
		// component removes this proxy before the asset retires the render data.
		const FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		uint64 MaterialComponentRevision = 0;
	};
}
