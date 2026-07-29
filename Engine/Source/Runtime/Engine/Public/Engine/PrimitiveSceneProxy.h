#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListBase;
	struct FStaticMeshRenderData;

	// Stores renderer-owned state detached from the game-thread primitive component.
	class PrimitiveSceneProxy
	{
	public:
		ENGINE_API virtual ~PrimitiveSceneProxy() = default;

		ENGINE_API auto SetTransform(FRHICommandListBase& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void;
		ENGINE_API auto GetLocalToWorld() const -> const FMatrix&;

	protected:
		FMatrix LocalToWorld_{1.0};

		FVector3 ActorPosition_;
	};

	// Couples static-mesh render resources with revisioned per-slot material bindings.
	class FStaticMeshSceneProxy : public PrimitiveSceneProxy
	{
	public:
		ENGINE_API explicit FStaticMeshSceneProxy(
			const FStaticMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			uint64 InMaterialComponentRevision);

		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		ENGINE_API auto ResolveMaterialRenderData_RenderThread(
			uint32 SlotIndex) const -> const FMaterialRenderData&;
		auto GetNumMaterials() const -> uint32 { return static_cast<uint32>(Materials.size()); }
		auto ResolveMaterialRenderData_RenderThread() const
			-> const FMaterialRenderData&
		{
			return ResolveMaterialRenderData_RenderThread(0);
		}
		auto GetMaterialComponentRevision() const -> uint64 { return MaterialComponentRevision; }
		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const
			-> const FMaterialRenderProxyRef&;
		ENGINE_API auto UpdateMaterialRenderProxyBinding(
			const FMaterialRenderProxyBindingUpdate& Update) -> void;

	private:
		// Non-owning borrow bounded by the component render-state lifetime. The
		// component removes this proxy before the asset retires the render data.
		const FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		uint64 MaterialComponentRevision = 0;
	};

	// Couples the shared preview sphere with one retained cube resource for editor previews.
	class FTextureCubePreviewSceneProxy final : public PrimitiveSceneProxy
	{
	public:
		ENGINE_API FTextureCubePreviewSceneProxy(
			const FStaticMeshRenderData* InRenderData,
			FRHITextureReferenceRef InTextureReference);

		auto GetRenderData() const -> const FStaticMeshRenderData* { return RenderData; }
		auto GetTextureReference() const -> const FRHITextureReferenceRef&
		{
			return TextureReference;
		}

	private:
		// Same component-render-state bounded borrow as FStaticMeshSceneProxy.
		const FStaticMeshRenderData* RenderData = nullptr;
		FRHITextureReferenceRef TextureReference;
	};
}
