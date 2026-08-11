#pragma once

#include "RendererAPI.h"

#include "Engine/FPrimitiveSceneProxy.h"
#include "Engine/LightSceneProxy.h"
#include "Engine/SkyBoxSceneProxy.h"
#include "IScene.h"

namespace Durin
{
	class FScene;

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
		auto SetTransform(const FMatrix& InTransform) -> void;
		auto SetVisible(bool bInVisible) -> void { bVisible = bInVisible; }
		auto UpdateMaterialBinding(const FMaterialRenderProxyBindingUpdate& Update) -> bool;
		auto UpdateSkeletalMeshDynamicData(
			std::shared_ptr<const FSkeletalPosePalette> Pose) -> bool;

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
		FLightSceneInfo(FScene& InScene, FLightSceneId InId,
			std::shared_ptr<FDirectionalLightSceneProxy> InProxy)
			: Scene(&InScene), Id(InId), Proxy(std::move(InProxy)) {}

		auto GetId() const -> FLightSceneId { return Id; }
		auto GetProxy() const -> const FDirectionalLightSceneProxy& { return *Proxy; }

	private:
		FScene* Scene = nullptr;
		FLightSceneId Id = InvalidLightSceneId;
		std::shared_ptr<FDirectionalLightSceneProxy> Proxy;
	};

	class RENDERER_API FSkyBoxSceneInfo final
	{
	public:
		FSkyBoxSceneInfo(FScene& InScene, FSkyBoxSceneId InId,
			FGuid InPersistentId, std::string InSelectionKey,
			std::shared_ptr<FSkyBoxSceneProxy> InProxy)
			: Scene(&InScene), Id(InId), PersistentId(InPersistentId),
			  SelectionKey(std::move(InSelectionKey)), Proxy(std::move(InProxy)) {}

		auto GetId() const -> FSkyBoxSceneId { return Id; }
		auto GetPersistentId() const -> const FGuid& { return PersistentId; }
		auto GetSelectionKey() const -> const std::string& { return SelectionKey; }
		auto GetProxy() const -> const FSkyBoxSceneProxy& { return *Proxy; }

	private:
		FScene* Scene = nullptr;
		FSkyBoxSceneId Id = InvalidSkyBoxSceneId;
		FGuid PersistentId;
		std::string SelectionKey;
		std::shared_ptr<FSkyBoxSceneProxy> Proxy;
	};

	class FScene : public IScene
	{
	public:
		RENDERER_API auto AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform, bool bVisible = true) -> void override;
		RENDERER_API auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void override;
		RENDERER_API auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void override;
		RENDERER_API auto UpdatePrimitiveVisibility(FPrimitiveSceneId PrimitiveId, bool bVisible) -> void override;
		RENDERER_API auto UpdatePrimitiveMaterialBinding(FPrimitiveSceneId PrimitiveId, const FMaterialRenderProxyBindingUpdate& Update) -> void override;
		RENDERER_API auto UpdateSkeletalMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			std::shared_ptr<const FSkeletalPosePalette> Pose) -> void override;
		RENDERER_API auto Release() -> void override;
		RENDERER_API auto AddOrReplaceDirectionalLight(FLightSceneId LightId, std::unique_ptr<FDirectionalLightSceneProxy> Proxy) -> void override;
		RENDERER_API auto RemoveDirectionalLight(FLightSceneId LightId) -> void override;
		RENDERER_API auto GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool override;
		RENDERER_API auto AddOrReplaceSkyBox(FSkyBoxSceneId SkyBoxId, FGuid PersistentId, std::string SelectionKey, std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> void override;
		RENDERER_API auto RemoveSkyBox(FSkyBoxSceneId SkyBoxId) -> void override;
		RENDERER_API auto GetActiveSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool override;
		RENDERER_API auto GetSkyBoxCount_RenderThread() const -> size_t override;

		auto GetPrimitiveSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return PrimitiveSceneInfos; }
		auto GetStaticMeshSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return StaticMeshSceneInfos; }
		auto GetSkeletalMeshSceneInfos() const -> const std::vector<FPrimitiveSceneInfo*>& { return SkeletalMeshSceneInfos; }
		RENDERER_API auto GetActiveSkyBoxSceneInfo_RenderThread() const -> const FSkyBoxSceneInfo*;

	private:
		auto DetachPrimitive(FPrimitiveSceneInfo& Info) -> void;
		std::unordered_map<FPrimitiveSceneId, std::unique_ptr<FPrimitiveSceneInfo>, FSceneIdHash> PrimitiveInfosById;
		std::vector<FPrimitiveSceneInfo*> PrimitiveSceneInfos;
		std::vector<FPrimitiveSceneInfo*> StaticMeshSceneInfos;
		std::vector<FPrimitiveSceneInfo*> SkeletalMeshSceneInfos;
		std::unordered_map<FLightSceneId, std::unique_ptr<FLightSceneInfo>, FSceneIdHash> LightInfosById;
		std::vector<FLightSceneInfo*> DirectionalLightSceneInfos;
		std::unordered_map<FSkyBoxSceneId, std::unique_ptr<FSkyBoxSceneInfo>, FSceneIdHash> SkyBoxInfosById;
		std::vector<FSkyBoxSceneInfo*> SkyBoxSceneInfos;
	};
}
