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

	auto FScene::UpdatePrimitiveMaterialBinding(
		FPrimitiveSceneId PrimitiveId,
		const FMaterialRenderProxyBindingUpdate& Update) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveMaterialBinding)([this, PrimitiveId, Update](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			const auto FoundIt = PrimitiveToProxy.find(PrimitiveId);
			if (FoundIt == PrimitiveToProxy.end()) return;
			if (auto* StaticMeshProxy = dynamic_cast<FStaticMeshSceneProxy*>(FoundIt->second.get()))
			{
				StaticMeshProxy->UpdateMaterialRenderProxyBinding(Update);
			}
		});
	}

	auto FScene::Release() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseScene)([this](FRHICommandListImmediate& CommandList) {
			CheckRenderingThread();
			PrimitiveSceneProxies.clear();
			PrimitiveToProxy.clear();
			SkyBoxes.clear();
			SkyBoxRevisions.clear();
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

	auto FScene::AddOrReplaceSkyBox(FSkyBoxSceneData Data) -> void
	{
		if (!Data.SceneId.IsValid() || Data.InstanceId == 0) return;
		ENQUEUE_RENDER_COMMAND(AddOrReplaceSkyBox)([this, Data = std::move(Data)](FRHICommandListImmediate&) mutable {
			CheckRenderingThread();
			uint64& LatestRevision = SkyBoxRevisions[Data.InstanceId];
			if (Data.Revision < LatestRevision) return;
			LatestRevision = Data.Revision;
			SkyBoxes.insert_or_assign(Data.InstanceId, std::move(Data));
		});
	}

	auto FScene::RemoveSkyBox(uint64 InstanceId, uint64 Revision) -> void
	{
		if (InstanceId == 0) return;
		ENQUEUE_RENDER_COMMAND(RemoveSkyBox)([this, InstanceId, Revision](FRHICommandListImmediate&) {
			CheckRenderingThread();
			uint64& LatestRevision = SkyBoxRevisions[InstanceId];
			if (Revision < LatestRevision) return;
			LatestRevision = Revision;
			SkyBoxes.erase(InstanceId);
		});
	}

	auto FScene::GetActiveSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool
	{
		CheckRenderingThread();
		if (SkyBoxes.empty()) return false;
		const auto Active = std::ranges::min_element(SkyBoxes,
			[](const auto& Left, const auto& Right) {
				const FSkyBoxSceneData& A = Left.second;
				const FSkyBoxSceneData& B = Right.second;
				return std::tie(A.SceneId, A.SelectionKey, A.InstanceId)
					< std::tie(B.SceneId, B.SelectionKey, B.InstanceId);
			});
		OutSkyBox = Active->second;
		return true;
	}

	auto FScene::GetSkyBoxCount_RenderThread() const -> size_t
	{
		CheckRenderingThread();
		return SkyBoxes.size();
	}
}
