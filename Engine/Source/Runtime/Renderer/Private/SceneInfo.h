#pragma once

#include "RendererAPI.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

namespace Durin
{
	class FSkeletalMeshSceneProxy;
	class FSplineMeshSceneProxy;
	class FStaticMeshSceneProxy;
	struct FSkeletalPosePalette;
	struct FSplineMeshRenderDynamicData;

	// Owns one primitive proxy plus Renderer-derived transform and bounds state.
	class FPrimitiveSceneInfo final
	{
	public:
		RENDERER_API FPrimitiveSceneInfo(
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
		RENDERER_API explicit FLightSceneInfo(
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
