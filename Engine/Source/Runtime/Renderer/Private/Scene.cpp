#include "Scene.h"

#include "Math/Operations.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto TransformBounds(const FBox& Bounds, const FMatrix& Transform) -> FBox
		{
			FBox Result;
			if (!Bounds.bIsValid || !Math::IsFinite(Transform)) return Result;
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Point(
					(Corner & 1u) != 0 ? Bounds.Max.x : Bounds.Min.x,
					(Corner & 2u) != 0 ? Bounds.Max.y : Bounds.Min.y,
					(Corner & 4u) != 0 ? Bounds.Max.z : Bounds.Min.z);
				const FVector4 Transformed = Transform * FVector4(Point, 1.0);
				Result.AddPoint(FVector3(Transformed));
			}
			return Result;
		}
	}

	FPrimitiveSceneInfo::FPrimitiveSceneInfo(FScene& InScene, FPrimitiveSceneId InId,
		std::shared_ptr<FPrimitiveSceneProxy> InProxy, const FMatrix& InTransform)
		: Scene(&InScene), Id(InId), Proxy(std::move(InProxy)), Kind(Proxy->GetKind()),
		  Transform(InTransform), LocalBounds(Proxy->GetLocalBounds()),
		  WorldBounds(TransformBounds(LocalBounds, Transform))
	{
	}

	auto FPrimitiveSceneInfo::GetStaticMeshProxy() const -> FStaticMeshSceneProxy&
	{
		check(Kind == EPrimitiveSceneProxyKind::StaticMesh);
		return static_cast<FStaticMeshSceneProxy&>(*Proxy);
	}

	auto FPrimitiveSceneInfo::GetTextureCubePreviewProxy() const -> FTextureCubePreviewSceneProxy&
	{
		check(Kind == EPrimitiveSceneProxyKind::TextureCubePreview);
		return static_cast<FTextureCubePreviewSceneProxy&>(*Proxy);
	}

	auto FPrimitiveSceneInfo::SetTransform(const FMatrix& InTransform) -> void
	{
		Transform = InTransform;
		WorldBounds = TransformBounds(LocalBounds, Transform);
	}

	auto FPrimitiveSceneInfo::UpdateMaterialBinding(const FMaterialRenderProxyBindingUpdate& Update) -> bool
	{
		return Proxy->UpdateMaterialBinding_RenderThread(Update);
	}

	auto FScene::DetachPrimitive(FPrimitiveSceneInfo& Info) -> void
	{
		std::erase(PrimitiveSceneInfos, &Info);
		switch (Info.GetKind())
		{
		case EPrimitiveSceneProxyKind::StaticMesh: std::erase(StaticMeshSceneInfos, &Info); break;
		case EPrimitiveSceneProxyKind::TextureCubePreview: std::erase(TextureCubePreviewSceneInfos, &Info); break;
		}
	}

	auto FScene::AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId || Proxy == nullptr || !Math::IsFinite(Transform)) return;
		std::shared_ptr<FPrimitiveSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplacePrimitive)([this, PrimitiveId, SharedProxy = std::move(SharedProxy), Transform](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId); Found != PrimitiveInfosById.end())
			{
				DetachPrimitive(*Found->second);
				PrimitiveInfosById.erase(Found);
			}
			auto Info = std::make_unique<FPrimitiveSceneInfo>(*this, PrimitiveId, SharedProxy, Transform);
			FPrimitiveSceneInfo* RawInfo = Info.get();
			PrimitiveSceneInfos.push_back(RawInfo);
			switch (RawInfo->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh: StaticMeshSceneInfos.push_back(RawInfo); break;
			case EPrimitiveSceneProxyKind::TextureCubePreview: TextureCubePreviewSceneInfos.push_back(RawInfo); break;
			}
			PrimitiveInfosById.emplace(PrimitiveId, std::move(Info));
		});
	}

	auto FScene::RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(RemovePrimitive)([this, PrimitiveId](FRHICommandListImmediate&) {
			CheckRenderingThread();
			const auto Found = PrimitiveInfosById.find(PrimitiveId);
			if (Found == PrimitiveInfosById.end()) return;
			DetachPrimitive(*Found->second);
			PrimitiveInfosById.erase(Found);
		});
	}

	auto FScene::UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId || !Math::IsFinite(Transform)) return;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveTransform)([this, PrimitiveId, Transform](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId); Found != PrimitiveInfosById.end()) Found->second->SetTransform(Transform);
		});
	}

	auto FScene::UpdatePrimitiveMaterialBinding(FPrimitiveSceneId PrimitiveId, const FMaterialRenderProxyBindingUpdate& Update) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveMaterialBinding)([this, PrimitiveId, Update](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId); Found != PrimitiveInfosById.end()) Found->second->UpdateMaterialBinding(Update);
		});
	}

	auto FScene::Release() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseScene)([this](FRHICommandListImmediate&) {
			CheckRenderingThread();
			PrimitiveSceneInfos.clear(); StaticMeshSceneInfos.clear(); TextureCubePreviewSceneInfos.clear(); PrimitiveInfosById.clear();
			DirectionalLightSceneInfos.clear(); LightInfosById.clear();
			SkyBoxSceneInfos.clear(); SkyBoxInfosById.clear();
		});
	}

	auto FScene::AddOrReplaceDirectionalLight(FLightSceneId LightId, std::unique_ptr<FDirectionalLightSceneProxy> Proxy) -> void
	{
		if (LightId == InvalidLightSceneId || Proxy == nullptr) return;
		std::shared_ptr<FDirectionalLightSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplaceDirectionalLight)([this, LightId, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = LightInfosById.find(LightId); Found != LightInfosById.end())
			{
				FLightSceneInfo* Previous = Found->second.get();
				const auto Membership = std::ranges::find(DirectionalLightSceneInfos, Previous);
				check(Membership != DirectionalLightSceneInfos.end());
				auto Replacement = std::make_unique<FLightSceneInfo>(*this, LightId, SharedProxy);
				*Membership = Replacement.get();
				Found->second = std::move(Replacement);
				return;
			}
			auto Info = std::make_unique<FLightSceneInfo>(*this, LightId, SharedProxy);
			DirectionalLightSceneInfos.push_back(Info.get());
			LightInfosById.emplace(LightId, std::move(Info));
		});
	}

	auto FScene::RemoveDirectionalLight(FLightSceneId LightId) -> void
	{
		if (LightId == InvalidLightSceneId) return;
		ENQUEUE_RENDER_COMMAND(RemoveDirectionalLight)([this, LightId](FRHICommandListImmediate&) {
			CheckRenderingThread();
			const auto Found = LightInfosById.find(LightId);
			if (Found == LightInfosById.end()) return;
			std::erase(DirectionalLightSceneInfos, Found->second.get());
			LightInfosById.erase(Found);
		});
	}

	auto FScene::GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool
	{
		CheckRenderingThread();
		if (DirectionalLightSceneInfos.empty()) return false;
		OutLight = DirectionalLightSceneInfos.front()->GetProxy().GetData();
		return true;
	}

	auto FScene::AddOrReplaceSkyBox(FSkyBoxSceneId SkyBoxId, FGuid PersistentId, std::string SelectionKey, std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> void
	{
		if (SkyBoxId == InvalidSkyBoxSceneId || !PersistentId.IsValid() || Proxy == nullptr) return;
		std::shared_ptr<FSkyBoxSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplaceSkyBox)([this, SkyBoxId, PersistentId, SelectionKey = std::move(SelectionKey), SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) mutable {
			CheckRenderingThread();
			if (const auto Found = SkyBoxInfosById.find(SkyBoxId); Found != SkyBoxInfosById.end())
			{
				std::erase(SkyBoxSceneInfos, Found->second.get());
				SkyBoxInfosById.erase(Found);
			}
			auto Info = std::make_unique<FSkyBoxSceneInfo>(*this, SkyBoxId, PersistentId, std::move(SelectionKey), SharedProxy);
			SkyBoxSceneInfos.push_back(Info.get());
			SkyBoxInfosById.emplace(SkyBoxId, std::move(Info));
		});
	}

	auto FScene::RemoveSkyBox(FSkyBoxSceneId SkyBoxId) -> void
	{
		if (SkyBoxId == InvalidSkyBoxSceneId) return;
		ENQUEUE_RENDER_COMMAND(RemoveSkyBox)([this, SkyBoxId](FRHICommandListImmediate&) {
			CheckRenderingThread();
			const auto Found = SkyBoxInfosById.find(SkyBoxId);
			if (Found == SkyBoxInfosById.end()) return;
			std::erase(SkyBoxSceneInfos, Found->second.get());
			SkyBoxInfosById.erase(Found);
		});
	}

	auto FScene::GetActiveSkyBoxSceneInfo_RenderThread() const -> const FSkyBoxSceneInfo*
	{
		CheckRenderingThread();
		if (SkyBoxSceneInfos.empty()) return nullptr;
		return *std::ranges::min_element(SkyBoxSceneInfos, [](const FSkyBoxSceneInfo* A, const FSkyBoxSceneInfo* B) {
			return std::tuple(A->GetPersistentId(), A->GetSelectionKey(), A->GetId())
				< std::tuple(B->GetPersistentId(), B->GetSelectionKey(), B->GetId());
		});
	}

	auto FScene::GetActiveSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool
	{
		const FSkyBoxSceneInfo* Info = GetActiveSkyBoxSceneInfo_RenderThread();
		if (Info == nullptr) return false;
		OutSkyBox = Info->GetProxy().GetData();
		OutSkyBox.SceneId = Info->GetPersistentId();
		OutSkyBox.SelectionKey = Info->GetSelectionKey();
		OutSkyBox.InstanceId = Info->GetId().Value;
		return true;
	}

	auto FScene::GetSkyBoxCount_RenderThread() const -> size_t
	{
		CheckRenderingThread();
		return SkyBoxSceneInfos.size();
	}
}
