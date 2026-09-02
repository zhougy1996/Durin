#pragma once

#include "RendererAPI.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

namespace Durin
{
	class FScene;
	class FSkeletalMeshSceneProxy;
	class FSplineMeshSceneProxy;
	class FStaticMeshSceneProxy;
	class FTerrainSceneProxy;
	struct FSkeletalPosePalette;
	struct FSplineMeshRenderDynamicData;

	// Owns one primitive proxy plus Renderer-derived transform and bounds state.
	class FPrimitiveSceneInfo final
	{
	public:
		RENDERER_API FPrimitiveSceneInfo(
			FScene& InScene,
			FPrimitiveSceneId InId,
			std::shared_ptr<FPrimitiveSceneProxy> InProxy,
			const FMatrix& InTransform);

		auto GetId() const -> FPrimitiveSceneId { return Id; }
		auto GetKind() const -> EPrimitiveSceneProxyKind { return Kind; }
		auto GetTransform() const -> const FMatrix& { return Transform; }
		auto GetLocalBounds() const -> const FBox& { return LocalBounds; }
		auto GetWorldBounds() const -> const FBox& { return WorldBounds; }
		auto IsVisible() const -> bool { return bVisible; }
		auto GetProxy() const -> FPrimitiveSceneProxy& { return *Proxy; }
		RENDERER_API auto GetStaticMeshProxy() const -> FStaticMeshSceneProxy&;
		RENDERER_API auto GetSkeletalMeshProxy() const -> FSkeletalMeshSceneProxy&;
		RENDERER_API auto GetTerrainProxy() const -> FTerrainSceneProxy&;
		RENDERER_API auto GetSplineMeshProxy() const -> FSplineMeshSceneProxy&;
		RENDERER_API auto SetTransform(const FMatrix& InTransform) -> void;
		auto SetVisible(bool bInVisible) -> void { bVisible = bInVisible; }
		RENDERER_API auto UpdateMaterialBinding(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool;
		RENDERER_API auto UpdateSkeletalMeshDynamicData(
			std::shared_ptr<const FSkeletalPosePalette> Pose) -> bool;
		RENDERER_API auto UpdateSplineMeshDynamicData(
			FSplineMeshRenderDynamicData DynamicData) -> bool;

	private:
		FScene* Scene = nullptr;
		FPrimitiveSceneId Id = InvalidPrimitiveSceneId;
		std::shared_ptr<FPrimitiveSceneProxy> Proxy;
		EPrimitiveSceneProxyKind Kind = EPrimitiveSceneProxyKind::StaticMesh;
		FMatrix Transform{1.0};
		FBox LocalBounds;
		FBox WorldBounds;
		bool bVisible = true;
	};

	// Owns one light proxy plus Renderer-derived family membership and bounds.
	class FLightSceneInfo final
	{
	public:
		RENDERER_API FLightSceneInfo(FScene& InScene,
			std::shared_ptr<FLightSceneProxy> InProxy);
		RENDERER_API ~FLightSceneInfo();

		auto GetId() const -> FLightSceneId { return Proxy->GetDesc().Id; }
		auto GetKind() const -> ELightSceneProxyKind { return Kind; }
		auto GetInfluenceBounds() const -> const FBox& { return InfluenceBounds; }
		auto GetProxy() const -> const FLightSceneProxy& { return *Proxy; }
		RENDERER_API auto GetDirectionalProxy() const -> const FDirectionalLightSceneProxy&;
		RENDERER_API auto GetPointProxy() const -> const FPointLightSceneProxy&;
		RENDERER_API auto GetSpotProxy() const -> const FSpotLightSceneProxy&;

	private:
		FScene* Scene = nullptr;
		std::shared_ptr<FLightSceneProxy> Proxy;
		ELightSceneProxyKind Kind = ELightSceneProxyKind::Directional;
		FBox InfluenceBounds;
	};

	// Owns one attached sky candidate and its proxy-to-scene association.
	class FSkyBoxSceneInfo final
	{
	public:
		RENDERER_API FSkyBoxSceneInfo(FScene& InScene,
			std::shared_ptr<FSkyBoxSceneProxy> InProxy);
		RENDERER_API ~FSkyBoxSceneInfo();

		auto GetProxy() const -> const FSkyBoxSceneProxy& { return *Proxy; }

	private:
		FScene* Scene = nullptr;
		std::shared_ptr<FSkyBoxSceneProxy> Proxy;
	};

	// Owns one attached cloud candidate and its proxy-to-scene association.
	class FVolumetricCloudSceneInfo final
	{
	public:
		RENDERER_API FVolumetricCloudSceneInfo(FScene& InScene,
			std::shared_ptr<FVolumetricCloudSceneProxy> InProxy);
		RENDERER_API ~FVolumetricCloudSceneInfo();

		auto GetId() const -> FVolumetricCloudSceneId
		{
			return Proxy->GetDesc().RuntimeId;
		}
		auto GetProxy() const -> const FVolumetricCloudSceneProxy& { return *Proxy; }

	private:
		FScene* Scene = nullptr;
		std::shared_ptr<FVolumetricCloudSceneProxy> Proxy;
	};
}
