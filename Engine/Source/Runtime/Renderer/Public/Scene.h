#pragma once

#include "RendererAPI.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"
#include "SceneInterface.h"

namespace Durin
{
	class FScene;
	class FLightSceneRegistry;
	class FSkyBoxSceneRegistry;
	class FVolumetricCloudSceneRegistry;
	class FSkeletalMeshSceneProxy;
	class FSplineMeshSceneProxy;
	class FStaticMeshSceneProxy;
	class FTerrainSceneProxy;
	struct FSplineMeshRenderDynamicData;

	class RENDERER_API FPrimitiveSceneInfo final
	{
	public:
		FPrimitiveSceneInfo(
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
		auto GetStaticMeshProxy() const -> FStaticMeshSceneProxy&;
		auto GetSkeletalMeshProxy() const -> FSkeletalMeshSceneProxy&;
		auto GetTerrainProxy() const -> FTerrainSceneProxy&;
		auto GetSplineMeshProxy() const -> FSplineMeshSceneProxy&;
		auto SetTransform(const FMatrix& InTransform) -> void;
		auto SetVisible(bool bInVisible) -> void { bVisible = bInVisible; }
		auto UpdateMaterialBinding(const FMaterialRenderProxyBindingUpdate& Update) -> bool;
		auto UpdateSkeletalMeshDynamicData(
			std::shared_ptr<const FSkeletalPosePalette> Pose) -> bool;
		auto UpdateSplineMeshDynamicData(FSplineMeshRenderDynamicData DynamicData) -> bool;

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

	class RENDERER_API FLightSceneInfo final
	{
	public:
		FLightSceneInfo(FScene& InScene,
			std::shared_ptr<FLightSceneProxy> InProxy);

		auto GetId() const -> FLightSceneId
		{
			return Proxy->GetMetadata().SceneId;
		}
		auto GetKind() const -> ELightSceneProxyKind { return Kind; }
		auto GetInfluenceBounds() const -> const FBox& { return InfluenceBounds; }
		auto GetProxy() const -> const FLightSceneProxy& { return *Proxy; }
		auto GetDirectionalProxy() const -> const FDirectionalLightSceneProxy&;
		auto GetPointProxy() const -> const FPointLightSceneProxy&;
		auto GetSpotProxy() const -> const FSpotLightSceneProxy&;

	private:
		FScene* Scene = nullptr;
		std::shared_ptr<FLightSceneProxy> Proxy;
		ELightSceneProxyKind Kind = ELightSceneProxyKind::Directional;
		FBox InfluenceBounds;
	};

	class RENDERER_API FSkyBoxSceneInfo final
	{
	public:
		FSkyBoxSceneInfo(FScene& InScene,
			std::shared_ptr<FSkyBoxSceneProxy> InProxy)
			: Scene(&InScene), Proxy(std::move(InProxy)) {}

		auto GetId() const -> FSkyBoxSceneId
		{
			return Proxy->GetMetadata().SceneId;
		}
		auto GetRevision() const -> uint64
		{
			return Proxy->GetMetadata().Revision;
		}
		auto GetProxy() const -> const FSkyBoxSceneProxy& { return *Proxy; }

	private:
		FScene* Scene = nullptr;
		std::shared_ptr<FSkyBoxSceneProxy> Proxy;
	};

	class RENDERER_API FVolumetricCloudSceneInfo final
	{
	public:
		FVolumetricCloudSceneInfo(FScene& InScene,
			std::shared_ptr<FVolumetricCloudSceneProxy> InProxy)
			: Scene(&InScene), Proxy(std::move(InProxy)) {}

		auto GetId() const -> FVolumetricCloudSceneId
		{
			return Proxy->GetMetadata().SceneId;
		}
		auto GetProxy() const -> const FVolumetricCloudSceneProxy& { return *Proxy; }
		auto GetRevision() const -> uint64
		{
			return Proxy->GetMetadata().Revision;
		}

	private:
		FScene* Scene = nullptr;
		std::shared_ptr<FVolumetricCloudSceneProxy> Proxy;
	};

	struct FSkyBoxSceneSnapshot
	{
		TSceneProxyMetadata<FSkyBoxSceneId> Metadata;
		FSceneCandidateIdentity Identity;
		FSkyBoxSceneData Data;
	};

	struct FVolumetricCloudSceneSnapshot
	{
		TSceneProxyMetadata<FVolumetricCloudSceneId> Metadata;
		FSceneCandidateIdentity Identity;
		FVolumetricCloudSceneData Data;
	};

	class FScene : public FSceneInterface
	{
	public:
		RENDERER_API FScene();
		RENDERER_API ~FScene() override;
		RENDERER_API auto AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform, bool bVisible = true) -> void override;
		RENDERER_API auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void override;
		RENDERER_API auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void override;
		RENDERER_API auto UpdatePrimitiveVisibility(FPrimitiveSceneId PrimitiveId, bool bVisible) -> void override;
		RENDERER_API auto UpdatePrimitiveMaterialBinding(FPrimitiveSceneId PrimitiveId, const FMaterialRenderProxyBindingUpdate& Update) -> void override;
		RENDERER_API auto UpdateSkeletalMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			std::shared_ptr<const FSkeletalPosePalette> Pose) -> void override;
		RENDERER_API auto UpdateSplineMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			FSplineMeshRenderDynamicData DynamicData) -> void override;
		RENDERER_API auto AddOrReplaceLight(FLightSceneId LightId, std::unique_ptr<FLightSceneProxy> Proxy) -> void override;
		RENDERER_API auto RemoveLight(FLightSceneId LightId) -> void override;
		RENDERER_API auto AddOrReplaceSkyBox(FSkyBoxSceneId SkyBoxId, std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> void override;
		RENDERER_API auto RemoveSkyBox(FSkyBoxSceneId SkyBoxId) -> void override;
		RENDERER_API auto AddOrReplaceVolumetricCloud(
			FVolumetricCloudSceneId CloudId,
			std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> void override;
		RENDERER_API auto RemoveVolumetricCloud(
			FVolumetricCloudSceneId CloudId) -> void override;

		RENDERER_API auto GetActiveSkyBox_RenderThread(
			FSkyBoxSceneSnapshot& OutSkyBox) const -> bool;
		RENDERER_API auto GetSkyBoxCount_RenderThread() const -> size_t;
		RENDERER_API auto GetActiveVolumetricCloud_RenderThread(
			FVolumetricCloudSceneSnapshot& OutCloud) const -> bool;
		RENDERER_API auto GetVolumetricCloudCount_RenderThread() const -> size_t;

		auto GetPrimitiveSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return PrimitiveSceneInfos; }
		auto GetStaticMeshSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return StaticMeshSceneInfos; }
		auto GetSkeletalMeshSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return SkeletalMeshSceneInfos; }
		auto GetTerrainSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return TerrainSceneInfos; }
		auto GetSplineMeshSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return SplineMeshSceneInfos; }
		RENDERER_API auto GetDirectionalLightSceneInfos() const -> const std::vector<FLightSceneInfo*>&;
		RENDERER_API auto GetPointLightSceneInfos() const -> const std::vector<FLightSceneInfo*>&;
		RENDERER_API auto GetSpotLightSceneInfos() const -> const std::vector<FLightSceneInfo*>&;
		RENDERER_API auto GetActiveSkyBoxSceneInfo_RenderThread() const -> const FSkyBoxSceneInfo*;
		RENDERER_API auto GetActiveVolumetricCloudSceneInfo_RenderThread() const
			-> const FVolumetricCloudSceneInfo*;

	private:
		auto AllocatePublicationRevision() -> uint64;
		auto Clear_RenderThread() -> void;
		auto DetachPrimitive(FPrimitiveSceneInfo& Info) -> void;
		std::unordered_map<FPrimitiveSceneId, std::unique_ptr<FPrimitiveSceneInfo>, FSceneIdHash> PrimitiveInfosById;
		std::vector<FPrimitiveSceneInfo*> PrimitiveSceneInfos;
		std::vector<FPrimitiveSceneInfo*> StaticMeshSceneInfos;
		std::vector<FPrimitiveSceneInfo*> SkeletalMeshSceneInfos;
		std::vector<FPrimitiveSceneInfo*> TerrainSceneInfos;
		std::vector<FPrimitiveSceneInfo*> SplineMeshSceneInfos;
		std::unique_ptr<FLightSceneRegistry> Lights;
		std::unique_ptr<FSkyBoxSceneRegistry> SkyBoxes;
		std::unique_ptr<FVolumetricCloudSceneRegistry> VolumetricClouds;
		std::atomic<uint64> NextPublicationRevision = 1;
	};
}
