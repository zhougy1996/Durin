#pragma once
#include "RendererAPI.h"

#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
namespace Durin
{
	class FScene : public IScene
	{
	public:
		auto AddPrimitive(DPrimitiveComponent* Primitive) -> void override;

		auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void override;

		auto UpdatePrimitiveTransform(DPrimitiveComponent* Primitive) -> void override;

		auto GetPrimitiveSceneProxies() const -> const std::vector<PrimitiveSceneProxy*>& { return PrimitiveSceneProxies; }

	private:
		std::unordered_map<DPrimitiveComponent*, std::unique_ptr<PrimitiveSceneProxy>> PrimitiveToProxy;
		std::vector<PrimitiveSceneProxy*> PrimitiveSceneProxies;
	};
}
