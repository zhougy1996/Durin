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
	class FPrimitiveSceneInfo;
	class FLightSceneInfo;
	class FSkyBoxSceneInfo;
	class FVolumetricCloudSceneInfo;
	class FLightSceneRegistry;
	class FSkyBoxSceneRegistry;
	class FVolumetricCloudSceneRegistry;
	class FSkeletalMeshSceneProxy;
	class FSplineMeshSceneProxy;
	class FStaticMeshSceneProxy;
	class FTerrainSceneProxy;
	struct FSplineMeshRenderDynamicData;

	struct FSkyBoxSceneSnapshot
	{
		FSkyBoxSceneProxyDesc Desc;
	};

	struct FVolumetricCloudSceneSnapshot
	{
		FVolumetricCloudSceneProxyDesc Desc;
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
		RENDERER_API auto AddLight(
			std::unique_ptr<FLightSceneProxy> Proxy) -> bool override;
		RENDERER_API auto RemoveLight(FLightSceneProxy* Proxy) -> void override;
		RENDERER_API auto AddSkyBox(
			std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> bool override;
		RENDERER_API auto RemoveSkyBox(FSkyBoxSceneProxy* Proxy) -> void override;
		RENDERER_API auto AddVolumetricCloud(
			std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> bool override;
		RENDERER_API auto RemoveVolumetricCloud(
			FVolumetricCloudSceneProxy* Proxy) -> void override;

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
	};
}
