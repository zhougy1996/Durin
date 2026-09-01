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
		RENDERER_API auto AddPrimitive(DPrimitiveComponent* Primitive) -> void override;
		RENDERER_API auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void override;
		RENDERER_API auto AddLight(DLightComponent* Light) -> void override;
		RENDERER_API auto RemoveLight(DLightComponent* Light) -> void override;
		RENDERER_API auto AddSkyBox(DSkyBoxComponent* SkyBox) -> void override;
		RENDERER_API auto RemoveSkyBox(DSkyBoxComponent* SkyBox) -> void override;
		RENDERER_API auto AddVolumetricCloud(
			DVolumetricCloudComponent* Cloud
		) -> void override;
		RENDERER_API auto RemoveVolumetricCloud(
			DVolumetricCloudComponent* Cloud
		) -> void override;
		RENDERER_API auto Release() -> void override;
		RENDERER_API auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void override;
		RENDERER_API auto UpdatePrimitiveVisibility(FPrimitiveSceneId PrimitiveId, bool bVisible) -> void override;
		RENDERER_API auto UpdatePrimitiveMaterialBinding(FPrimitiveSceneId PrimitiveId, const FMaterialRenderProxyBindingUpdate& Update) -> void override;
		RENDERER_API auto UpdateSkeletalMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			std::shared_ptr<const FSkeletalPosePalette> Pose
		) -> void override;
		RENDERER_API auto UpdateSplineMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			FSplineMeshRenderDynamicData DynamicData
		) -> void override;
		RENDERER_API auto GetActiveSkyBox_RenderThread(
			FSkyBoxSceneSnapshot& OutSkyBox
		) const -> bool;
		RENDERER_API auto GetSkyBoxCount_RenderThread() const -> size_t;
		RENDERER_API auto GetActiveVolumetricCloud_RenderThread(
			FVolumetricCloudSceneSnapshot& OutCloud
		) const -> bool;
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
		RENDERER_API FScene();
		RENDERER_API ~FScene() override;

		enum class ELifecycleState : uint8
		{
			Active,
			Releasing,
			Released
		};

		RENDERER_API auto TryAddPrimitiveProxy(FPrimitiveSceneId PrimitiveId, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform, bool bVisible) -> bool;
		RENDERER_API auto TryRemovePrimitiveProxy(FPrimitiveSceneId PrimitiveId) -> bool;
		RENDERER_API auto TryAddLightProxy(std::unique_ptr<FLightSceneProxy> Proxy) -> bool;
		RENDERER_API auto TryRemoveLightProxy(FLightSceneProxy* Proxy) -> bool;
		RENDERER_API auto TryAddSkyBoxProxy(std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> bool;
		RENDERER_API auto TryRemoveSkyBoxProxy(FSkyBoxSceneProxy* Proxy) -> bool;
		RENDERER_API auto TryAddVolumetricCloudProxy(
			std::unique_ptr<FVolumetricCloudSceneProxy> Proxy
		) -> bool;
		RENDERER_API auto TryRemoveVolumetricCloudProxy(
			FVolumetricCloudSceneProxy* Proxy
		) -> bool;

		auto RequireActive(std::string_view Operation) const -> void;
		auto IsEmpty_RenderThread() const -> bool;
		RENDERER_API static auto DestroyScene(FSceneInterface* Scene) -> void;
		static auto GetActiveSceneCount() -> size_t;
		static auto GetAllocatedSceneCount() -> size_t;
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
		std::atomic<ELifecycleState> LifecycleState{ELifecycleState::Active};

		friend class FRendererModule;
		friend class FSceneInterfaceTestAccess;
	};
} // namespace Durin
