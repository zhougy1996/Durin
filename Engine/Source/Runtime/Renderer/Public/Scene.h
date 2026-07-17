#pragma once
#include "RendererAPI.h"

#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
namespace Durin
{
	class RENDERER_API FScene : public IScene
	{
	public:
		auto AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<PrimitiveSceneProxy> Proxy, const FMatrix& Transform) -> void override;

		auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void override;

		auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void override;
		auto UpdatePrimitiveMaterial(FPrimitiveSceneId PrimitiveId, const FMaterialRenderUpdate& Update) -> void override;
		auto Release() -> void override;
		auto AddDirectionalLight(DDirectionalLightComponent* Light) -> void override;
		auto RemoveDirectionalLight(DDirectionalLightComponent* Light) -> void override;
		auto GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool override;

		auto GetPrimitiveSceneProxies() const -> const std::vector<PrimitiveSceneProxy*>& { return PrimitiveSceneProxies; }

	private:
		std::unordered_map<FPrimitiveSceneId, std::shared_ptr<PrimitiveSceneProxy>> PrimitiveToProxy;
		std::vector<PrimitiveSceneProxy*> PrimitiveSceneProxies;
		std::vector<DDirectionalLightComponent*> DirectionalLights;
	};
}
