#pragma once

#include "CoreMinimal.h"

#include "RendererAPI.h"
#include "GPUScene.h"

#include "Rendering/PrimitiveComponentId.h"
#include "Rendering/PrimitiveSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

namespace Durin
{
	namespace RendererPrivate { class FCachedMeshDrawCommandStorage; struct FCachedMeshDrawCommandInfo; struct FStaticMeshCommandBuildRequest; struct FMeshDrawCommandBindingCache; }
	class FUniformBuffer;
	class FScene;
	class FSceneRenderingService;
	class FSplineMeshSceneProxy;
	struct FSplineMeshRenderDynamicData;

	// Owns one primitive proxy and its renderer-managed bounds.
	class FPrimitiveSceneInfo final
	{
	public:
		RENDERER_API FPrimitiveSceneInfo(
			FPrimitiveComponentId InId,
			std::shared_ptr<FPrimitiveSceneProxy> InProxy,
			const FMatrix& InTransform, FScene* InScene = nullptr);

		RENDERER_API ~FPrimitiveSceneInfo();
		RENDERER_API auto BeginDeferredUpdateStaticMeshes() -> void;
		auto RequestStaticMeshUpdate() -> void;
		auto GetId() const -> FPrimitiveComponentId { return Id; }
		auto GetPersistentIndex() const -> FPersistentPrimitiveIndex { return GPUSceneHandle.PersistentPrimitiveIndex; }
		RENDERER_API auto RequestGPUSceneUpdate(EGPUSceneDirtyState Dirty) -> void;
		FGPUScenePrimitiveHandle GPUSceneHandle;
		uint32 InstanceSceneDataOffset = INDEX_NONE_U32;
		uint32 NumInstanceSceneDataEntries = 0;
		static auto CacheMeshDrawCommands(FScene* Scene, std::span<FPrimitiveSceneInfo* const> SceneInfos,
			FSceneRenderingService& Service) -> void;
		static auto BuildCachedMeshDrawCommands(FScene& Scene, std::span<const RendererPrivate::FStaticMeshCommandBuildRequest> Requests,
			RendererPrivate::FMeshDrawCommandBindingCache& Bindings) -> void;
		RENDERER_API auto RemoveCachedMeshDrawCommands() -> void;
		RENDERER_API auto GetPrimitiveUniformBuffer(uint64 Batch) -> FUniformBuffer&;
		RENDERER_API auto RetainPrimitiveUniformBuffers(std::span<const uint64> Batches) -> void;
		RENDERER_API auto GetSceneInfoRef() const -> FPrimitiveSceneInfoRef;
		auto GetKind() const -> EPrimitiveSceneProxyKind { return Kind; }
		auto GetLocalBounds() const -> const FBox& { return LocalBounds; }
		auto GetWorldBounds() const -> const FBox& { return WorldBounds; }
		auto GetOcclusionBounds() const -> const FBox& { return WorldBounds; }
		auto CanBeOccluded() const -> bool { return bCanBeOccluded; }
		auto IsVisible() const -> bool { return bVisible; }
		auto GetProxy() const -> FPrimitiveSceneProxy& { return *Proxy; }
		RENDERER_API auto GetSplineMeshProxy() const -> FSplineMeshSceneProxy&;
		RENDERER_API auto SetTransform(const FMatrix& InTransform) -> void;
		auto SetVisible(bool bInVisible) -> void { bVisible = bInVisible; }
		auto RefreshBounds() -> void
		{
			LocalBounds = Proxy->GetLocalBounds();
			SetTransform(Proxy->GetLocalToWorld());
		}
		RENDERER_API auto UpdateMaterialBinding(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool;
		RENDERER_API auto UpdateSplineMeshDynamicData(
			FSplineMeshRenderDynamicData DynamicData) -> bool;

	private:
		friend class FScene;
		friend class RendererPrivate::FCachedMeshDrawCommandStorage;
		RENDERER_API auto DetachFromScene() -> void;
		RendererPrivate::FCachedMeshDrawCommandStorage* DrawLists = nullptr;
		std::map<std::array<uint64, 7>, std::shared_ptr<RendererPrivate::FCachedMeshDrawCommandInfo>> StaticMeshCommandInfos;
		std::map<uint64, std::shared_ptr<FUniformBuffer>> PrimitiveUniformBuffers;
		FScene* Scene = nullptr;
		bool bNeedsStaticMeshUpdate = false;
		bool bCanBeOccluded = false;
		FPrimitiveComponentId Id = InvalidPrimitiveComponentId;
		std::shared_ptr<FPrimitiveSceneProxy> Proxy;
		EPrimitiveSceneProxyKind Kind = EPrimitiveSceneProxyKind::StaticMesh;
		FBox LocalBounds;
		FBox WorldBounds;
		bool bVisible = true;
	};

	// Owns one light proxy plus Renderer-derived family membership and bounds.
	class FLightSceneInfo final
	{
	public:
		RENDERER_API explicit FLightSceneInfo(
			std::shared_ptr<FLightSceneProxy> InProxy);
		RENDERER_API ~FLightSceneInfo();

		auto GetId() const -> FLightComponentId { return Proxy->GetDesc().Id; }
		auto GetKind() const -> ELightSceneProxyKind { return Kind; }
		auto GetInfluenceBounds() const -> const FBox& { return InfluenceBounds; }
		auto GetProxy() const -> const FLightSceneProxy& { return *Proxy; }
		RENDERER_API auto GetDirectionalProxy() const -> const FDirectionalLightSceneProxy&;
		RENDERER_API auto GetPointProxy() const -> const FPointLightSceneProxy&;
		RENDERER_API auto GetSpotProxy() const -> const FSpotLightSceneProxy&;

	private:
		std::shared_ptr<FLightSceneProxy> Proxy;
		ELightSceneProxyKind Kind = ELightSceneProxyKind::Directional;
		FBox InfluenceBounds;
	};

	// Owns one attached cloud candidate and its proxy-to-scene association.
	class FVolumetricCloudSceneInfo final
	{
	public:
		RENDERER_API explicit FVolumetricCloudSceneInfo(
			std::shared_ptr<FVolumetricCloudSceneProxy> InProxy);
		RENDERER_API ~FVolumetricCloudSceneInfo();

		auto GetProxy() const -> const FVolumetricCloudSceneProxy& { return *Proxy; }

	private:
		std::shared_ptr<FVolumetricCloudSceneProxy> Proxy;
	};
}
