#include "Scene.h"

#include "Components/PrimitiveComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "RHICommandList.h"

namespace Durin
{
	auto FScene::AddPrimitive(DPrimitiveComponent* Primitive) -> void
	{
		if (Primitive == nullptr)
		{
			return;
		}

		RemovePrimitive(Primitive);

		std::unique_ptr<PrimitiveSceneProxy> Proxy = Primitive->CreateSceneProxy();
		if (Proxy == nullptr)
		{
			return;
		}

		Proxy->SetTransform(FRHICommandListImmediate::Get(), Primitive->GetRenderMatrix(), FVector3(0.0));
		PrimitiveSceneProxy* ProxyPtr = Proxy.get();
		PrimitiveToProxy.emplace(Primitive, std::move(Proxy));
		PrimitiveSceneProxies.push_back(ProxyPtr);
	}

	auto FScene::RemovePrimitive(DPrimitiveComponent* Primitive) -> void
	{
		const auto FoundIt = PrimitiveToProxy.find(Primitive);
		if (FoundIt == PrimitiveToProxy.end())
		{
			return;
		}

		PrimitiveSceneProxy* Proxy = FoundIt->second.get();
		std::erase(PrimitiveSceneProxies, Proxy);
		PrimitiveToProxy.erase(FoundIt);
	}

	auto FScene::UpdatePrimitiveTransform(DPrimitiveComponent* Primitive) -> void
	{
		const auto FoundIt = PrimitiveToProxy.find(Primitive);
		if (FoundIt == PrimitiveToProxy.end())
		{
			return;
		}

		FoundIt->second->SetTransform(FRHICommandListImmediate::Get(), Primitive->GetRenderMatrix(), FVector3(0.0));
	}

	auto FScene::AddDirectionalLight(DDirectionalLightComponent* Light) -> void
	{
		if (Light != nullptr && std::ranges::find(DirectionalLights, Light) == DirectionalLights.end()) DirectionalLights.push_back(Light);
	}

	auto FScene::RemoveDirectionalLight(DDirectionalLightComponent* Light) -> void
	{
		std::erase(DirectionalLights, Light);
	}

	auto FScene::GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool
	{
		if (DirectionalLights.empty() || DirectionalLights.front() == nullptr) return false;
		OutLight = DirectionalLights.front()->GetSceneData();
		return true;
	}
}
