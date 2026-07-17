#include "Scene.h"

#include "Components/DirectionalLightComponent.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FScene::AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<PrimitiveSceneProxy> Proxy, const FMatrix& Transform) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId || Proxy == nullptr) return;
		// The command pipe stores copyable callables; after enqueue, the final proxy owner remains on the rendering thread.
		std::shared_ptr<PrimitiveSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplacePrimitive)([this, PrimitiveId, SharedProxy = std::move(SharedProxy), Transform](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			if (const auto FoundIt = PrimitiveToProxy.find(PrimitiveId); FoundIt != PrimitiveToProxy.end())
			{
				std::erase(PrimitiveSceneProxies, FoundIt->second.get());
				PrimitiveToProxy.erase(FoundIt);
			}
			SharedProxy->SetTransform(CommandList, Transform, FVector3(0.0));
			PrimitiveSceneProxies.push_back(SharedProxy.get());
			PrimitiveToProxy.emplace(PrimitiveId, SharedProxy);
		});
	}

	auto FScene::RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(RemovePrimitive)([this, PrimitiveId](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			const auto FoundIt = PrimitiveToProxy.find(PrimitiveId);
			if (FoundIt == PrimitiveToProxy.end()) return;
			std::erase(PrimitiveSceneProxies, FoundIt->second.get());
			PrimitiveToProxy.erase(FoundIt);
		});
	}

	auto FScene::UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveTransform)([this, PrimitiveId, Transform](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			const auto FoundIt = PrimitiveToProxy.find(PrimitiveId);
			if (FoundIt == PrimitiveToProxy.end()) return;
			FoundIt->second->SetTransform(CommandList, Transform, FVector3(0.0));
		});
	}

	auto FScene::UpdatePrimitiveMaterial(FPrimitiveSceneId PrimitiveId, const FMaterialRenderUpdate& Update) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveMaterial)([this, PrimitiveId, Update](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			const auto FoundIt = PrimitiveToProxy.find(PrimitiveId);
			if (FoundIt == PrimitiveToProxy.end()) return;
			if (auto* StaticMeshProxy = dynamic_cast<FStaticMeshSceneProxy*>(FoundIt->second.get()))
			{
				StaticMeshProxy->UpdateMaterialRenderData(Update);
			}
		});
	}

	auto FScene::Release() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseScene)([this](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			PrimitiveSceneProxies.clear();
			PrimitiveToProxy.clear();
		});
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
