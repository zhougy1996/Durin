#pragma once
#include "RendererAPI.h"

#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
namespace Durin
{
	// Owns renderer-side primitive and light proxies for one engine scene.
	class FScene : public IScene
	{
	public:
		RENDERER_API auto AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<PrimitiveSceneProxy> Proxy, const FMatrix& Transform) -> void override;

		RENDERER_API auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void override;

		RENDERER_API auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void override;
		RENDERER_API auto UpdatePrimitiveMaterial(FPrimitiveSceneId PrimitiveId, const FMaterialRenderUpdate& Update) -> void override;
		RENDERER_API auto Release() -> void override;
		RENDERER_API auto AddDirectionalLight(DDirectionalLightComponent* Light) -> void override;
		RENDERER_API auto RemoveDirectionalLight(DDirectionalLightComponent* Light) -> void override;
		RENDERER_API auto GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool override;
		RENDERER_API auto AddOrReplaceSkyBox(FSkyBoxSceneData Data) -> void override;
		RENDERER_API auto RemoveSkyBox(uint64 InstanceId, uint64 Revision) -> void override;
		RENDERER_API auto GetActiveSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool override;
		RENDERER_API auto GetSkyBoxCount_RenderThread() const -> size_t override;

		auto GetPrimitiveSceneProxies() const -> const std::vector<PrimitiveSceneProxy*>& { return PrimitiveSceneProxies; }

	private:
		std::unordered_map<FPrimitiveSceneId, std::shared_ptr<PrimitiveSceneProxy>> PrimitiveToProxy;
		std::vector<PrimitiveSceneProxy*> PrimitiveSceneProxies;
		std::vector<DDirectionalLightComponent*> DirectionalLights;
		std::unordered_map<uint64, FSkyBoxSceneData> SkyBoxes;
		std::unordered_map<uint64, uint64> SkyBoxRevisions;
	};
}
