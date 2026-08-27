#include "Scene.h"

#include "Rendering/SkeletalMeshSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/TerrainSceneProxy.h"

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

	FScene::~FScene()
	{
		if (IsInRenderingThread()) return;

		checkf(IsInGameThread(),
			"Renderer scenes must be destroyed from the game or rendering thread.");
		if (GetRenderCommandAdmissionState()
			!= ERenderCommandAdmissionState::Running)
		{
			checkf(GetRenderCommandAdmissionState()
					== ERenderCommandAdmissionState::Stopped
					&& GetNumPendingRenderCommands() == 0,
				"A renderer scene was destroyed while render commands were draining.");
			return;
		}
		ENQUEUE_RENDER_COMMAND(ClearSceneBeforeSynchronousDestruction)(
			[this](FRHICommandListImmediate&) { Clear_RenderThread(); });
		FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}

	FLightSceneInfo::FLightSceneInfo(FScene& InScene, FLightSceneId InId,
		std::shared_ptr<FLightSceneProxy> InProxy)
		: Scene(&InScene), Id(InId), Proxy(std::move(InProxy)), Kind(Proxy->GetKind())
	{
		FVector3 Position(0.0);
		double Range = 0.0;
		if (Kind == ELightSceneProxyKind::Point)
		{
			const auto& Data = GetPointProxy().GetData();
			Position = Data.Position;
			Range = Data.Range;
		}
		else if (Kind == ELightSceneProxyKind::Spot)
		{
			const auto& Data = GetSpotProxy().GetData();
			Position = Data.Position;
			Range = Data.Range;
		}
		if (Range > 0.0)
		{
			InfluenceBounds.AddPoint(Position - FVector3(Range));
			InfluenceBounds.AddPoint(Position + FVector3(Range));
		}
	}

	auto FLightSceneInfo::GetDirectionalProxy() const
		-> const FDirectionalLightSceneProxy&
	{
		check(Kind == ELightSceneProxyKind::Directional);
		return static_cast<const FDirectionalLightSceneProxy&>(*Proxy);
	}

	auto FLightSceneInfo::GetPointProxy() const -> const FPointLightSceneProxy&
	{
		check(Kind == ELightSceneProxyKind::Point);
		return static_cast<const FPointLightSceneProxy&>(*Proxy);
	}

	auto FLightSceneInfo::GetSpotProxy() const -> const FSpotLightSceneProxy&
	{
		check(Kind == ELightSceneProxyKind::Spot);
		return static_cast<const FSpotLightSceneProxy&>(*Proxy);
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

	auto FPrimitiveSceneInfo::GetSkeletalMeshProxy() const -> FSkeletalMeshSceneProxy&
	{
		check(Kind == EPrimitiveSceneProxyKind::SkeletalMesh);
		return static_cast<FSkeletalMeshSceneProxy&>(*Proxy);
	}

	auto FPrimitiveSceneInfo::GetTerrainProxy() const -> FTerrainSceneProxy&
	{
		check(Kind == EPrimitiveSceneProxyKind::Terrain);
		return static_cast<FTerrainSceneProxy&>(*Proxy);
	}

	auto FPrimitiveSceneInfo::GetSplineMeshProxy() const -> FSplineMeshSceneProxy&
	{
		check(Kind == EPrimitiveSceneProxyKind::SplineMesh);
		return static_cast<FSplineMeshSceneProxy&>(*Proxy);
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

	auto FPrimitiveSceneInfo::UpdateSkeletalMeshDynamicData(
		std::shared_ptr<const FSkeletalPosePalette> Pose) -> bool
	{
		if (Kind != EPrimitiveSceneProxyKind::SkeletalMesh
			|| !GetSkeletalMeshProxy().UpdateDynamicData_RenderThread(std::move(Pose))) return false;
		LocalBounds = GetSkeletalMeshProxy().GetLocalBounds();
		WorldBounds = TransformBounds(LocalBounds, Transform);
		return true;
	}

	auto FPrimitiveSceneInfo::UpdateSplineMeshDynamicData(
		FSplineMeshRenderDynamicData DynamicData) -> bool
	{
		if (Kind != EPrimitiveSceneProxyKind::SplineMesh
			|| !GetSplineMeshProxy().UpdateDynamicData_RenderThread(std::move(DynamicData))) return false;
		LocalBounds = GetSplineMeshProxy().GetLocalBounds();
		WorldBounds = TransformBounds(LocalBounds, Transform);
		return true;
	}

	auto FScene::DetachPrimitive(FPrimitiveSceneInfo& Info) -> void
	{
		std::erase(PrimitiveSceneInfos, &Info);
		switch (Info.GetKind())
		{
		case EPrimitiveSceneProxyKind::StaticMesh: std::erase(StaticMeshSceneInfos, &Info); break;
		case EPrimitiveSceneProxyKind::SkeletalMesh: std::erase(SkeletalMeshSceneInfos, &Info); break;
		case EPrimitiveSceneProxyKind::Terrain: std::erase(TerrainSceneInfos, &Info); break;
		case EPrimitiveSceneProxyKind::SplineMesh: std::erase(SplineMeshSceneInfos, &Info); break;
		}
	}

	auto FScene::AddOrReplacePrimitive(FPrimitiveSceneId PrimitiveId, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform, bool bVisible) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId || Proxy == nullptr || !Math::IsFinite(Transform)) return;
		std::shared_ptr<FPrimitiveSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplacePrimitive)([this, PrimitiveId, SharedProxy = std::move(SharedProxy), Transform, bVisible](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId); Found != PrimitiveInfosById.end())
			{
				DetachPrimitive(*Found->second);
				PrimitiveInfosById.erase(Found);
			}
			auto Info = std::make_unique<FPrimitiveSceneInfo>(*this, PrimitiveId, SharedProxy, Transform);
			Info->SetVisible(bVisible);
			FPrimitiveSceneInfo* RawInfo = Info.get();
			PrimitiveSceneInfos.push_back(RawInfo);
			switch (RawInfo->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh: StaticMeshSceneInfos.push_back(RawInfo); break;
			case EPrimitiveSceneProxyKind::SkeletalMesh: SkeletalMeshSceneInfos.push_back(RawInfo); break;
			case EPrimitiveSceneProxyKind::Terrain: TerrainSceneInfos.push_back(RawInfo); break;
			case EPrimitiveSceneProxyKind::SplineMesh: SplineMeshSceneInfos.push_back(RawInfo); break;
			}
			PrimitiveInfosById.emplace(PrimitiveId, std::move(Info));
		});
	}

	auto FScene::UpdatePrimitiveVisibility(
		FPrimitiveSceneId PrimitiveId,
		bool bVisible) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveVisibility)(
			[this, PrimitiveId, bVisible](FRHICommandListImmediate&) {
				CheckRenderingThread();
				if (const auto Found = PrimitiveInfosById.find(PrimitiveId);
					Found != PrimitiveInfosById.end())
				{
					Found->second->SetVisible(bVisible);
				}
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

	auto FScene::UpdateSkeletalMeshDynamicData(
		FPrimitiveSceneId PrimitiveId,
		std::shared_ptr<const FSkeletalPosePalette> Pose) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId || !Pose) return;
		ENQUEUE_RENDER_COMMAND(UpdateSkeletalMeshDynamicData)(
			[this, PrimitiveId, Pose = std::move(Pose)](FRHICommandListImmediate&) mutable {
				CheckRenderingThread();
				if (const auto Found = PrimitiveInfosById.find(PrimitiveId);
					Found != PrimitiveInfosById.end())
					Found->second->UpdateSkeletalMeshDynamicData(std::move(Pose));
			});
	}

	auto FScene::UpdateSplineMeshDynamicData(
		FPrimitiveSceneId PrimitiveId,
		FSplineMeshRenderDynamicData DynamicData) -> void
	{
		if (PrimitiveId == InvalidPrimitiveSceneId || DynamicData.Revision == 0
			|| !DynamicData.LocalBounds.bIsValid) return;
		ENQUEUE_RENDER_COMMAND(UpdateSplineMeshDynamicData)(
			[this, PrimitiveId, DynamicData = std::move(DynamicData)](FRHICommandListImmediate&) mutable {
				CheckRenderingThread();
				if (const auto Found = PrimitiveInfosById.find(PrimitiveId);
					Found != PrimitiveInfosById.end())
					Found->second->UpdateSplineMeshDynamicData(std::move(DynamicData));
			});
	}

	auto FScene::Clear_RenderThread() -> void
	{
		CheckRenderingThread();
		PrimitiveSceneInfos.clear(); StaticMeshSceneInfos.clear(); SkeletalMeshSceneInfos.clear();
		TerrainSceneInfos.clear(); SplineMeshSceneInfos.clear(); PrimitiveInfosById.clear();
		DirectionalLightSceneInfos.clear(); PointLightSceneInfos.clear();
		SpotLightSceneInfos.clear(); LightInfosById.clear();
		SkyBoxSceneInfos.clear(); SkyBoxInfosById.clear();
		VolumetricCloudSceneInfos.clear(); VolumetricCloudInfosById.clear();
	}

	auto FScene::AttachLight(FLightSceneInfo& Info) -> void
	{
		switch (Info.GetKind())
		{
		case ELightSceneProxyKind::Directional:
			DirectionalLightSceneInfos.push_back(&Info);
			break;
		case ELightSceneProxyKind::Point:
			PointLightSceneInfos.push_back(&Info);
			break;
		case ELightSceneProxyKind::Spot:
			SpotLightSceneInfos.push_back(&Info);
			break;
		}
	}

	auto FScene::DetachLight(FLightSceneInfo& Info) -> void
	{
		switch (Info.GetKind())
		{
		case ELightSceneProxyKind::Directional:
			std::erase(DirectionalLightSceneInfos, &Info);
			break;
		case ELightSceneProxyKind::Point:
			std::erase(PointLightSceneInfos, &Info);
			break;
		case ELightSceneProxyKind::Spot:
			std::erase(SpotLightSceneInfos, &Info);
			break;
		}
	}

	auto FScene::AddOrReplaceLight(FLightSceneId LightId, std::unique_ptr<FLightSceneProxy> Proxy) -> void
	{
		if (LightId == InvalidLightSceneId || Proxy == nullptr) return;
		std::shared_ptr<FLightSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplaceLight)([this, LightId, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = LightInfosById.find(LightId); Found != LightInfosById.end())
			{
				DetachLight(*Found->second);
				LightInfosById.erase(Found);
			}
			auto Info = std::make_unique<FLightSceneInfo>(*this, LightId, SharedProxy);
			AttachLight(*Info);
			LightInfosById.emplace(LightId, std::move(Info));
		});
	}

	auto FScene::RemoveLight(FLightSceneId LightId) -> void
	{
		if (LightId == InvalidLightSceneId) return;
		ENQUEUE_RENDER_COMMAND(RemoveLight)([this, LightId](FRHICommandListImmediate&) {
			CheckRenderingThread();
			const auto Found = LightInfosById.find(LightId);
			if (Found == LightInfosById.end()) return;
			DetachLight(*Found->second);
			LightInfosById.erase(Found);
		});
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

	auto FScene::AddOrReplaceVolumetricCloud(
		FVolumetricCloudSceneId CloudId, uint64 PublicationRevision,
		std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> void
	{
		if (CloudId == InvalidVolumetricCloudSceneId || PublicationRevision == 0
			|| Proxy == nullptr) return;
		const FVolumetricCloudSceneData& Data = Proxy->GetData();
		if (!Data.PersistentId.IsValid() || Data.InstanceId != CloudId.Value
			|| Data.PublicationRevision != PublicationRevision) return;
		std::shared_ptr<FVolumetricCloudSceneProxy> SharedProxy(std::move(Proxy));
		ENQUEUE_RENDER_COMMAND(AddOrReplaceVolumetricCloud)(
			[this, CloudId, PublicationRevision,
				SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
				CheckRenderingThread();
				if (const auto Found = VolumetricCloudInfosById.find(CloudId);
					Found != VolumetricCloudInfosById.end())
				{
					if (Found->second->GetRevision() >= PublicationRevision) return;
					std::erase(VolumetricCloudSceneInfos, Found->second.get());
					VolumetricCloudInfosById.erase(Found);
				}
				auto Info = std::make_unique<FVolumetricCloudSceneInfo>(
					*this, CloudId, SharedProxy);
				VolumetricCloudSceneInfos.push_back(Info.get());
				VolumetricCloudInfosById.emplace(CloudId, std::move(Info));
			});
	}

	auto FScene::RemoveVolumetricCloud(
		FVolumetricCloudSceneId CloudId, uint64 ExpectedRevision) -> void
	{
		if (CloudId == InvalidVolumetricCloudSceneId || ExpectedRevision == 0) return;
		ENQUEUE_RENDER_COMMAND(RemoveVolumetricCloud)(
			[this, CloudId, ExpectedRevision](FRHICommandListImmediate&) {
				CheckRenderingThread();
				const auto Found = VolumetricCloudInfosById.find(CloudId);
				if (Found == VolumetricCloudInfosById.end()
					|| Found->second->GetRevision() != ExpectedRevision) return;
				std::erase(VolumetricCloudSceneInfos, Found->second.get());
				VolumetricCloudInfosById.erase(Found);
			});
	}

	auto FScene::GetActiveVolumetricCloudSceneInfo_RenderThread() const
		-> const FVolumetricCloudSceneInfo*
	{
		CheckRenderingThread();
		FVolumetricCloudSceneInfo* Active = nullptr;
		for (FVolumetricCloudSceneInfo* Candidate : VolumetricCloudSceneInfos)
		{
			const FVolumetricCloudSceneData& Data = Candidate->GetProxy().GetData();
			if (!Data.bEligible
				|| (Data.BaseDensityTexture
					&& !Data.BaseDensityTexture->GetReferencedTexture_RenderThread())
				|| (Data.DetailDensityTexture
					&& !Data.DetailDensityTexture->GetReferencedTexture_RenderThread()))
				continue;
			if (Active == nullptr)
			{
				Active = Candidate;
				continue;
			}
			const FVolumetricCloudSceneData& ActiveData = Active->GetProxy().GetData();
			if (Data.Priority > ActiveData.Priority
				|| (Data.Priority == ActiveData.Priority
					&& std::tuple(Data.PersistentId, Data.SelectionKey, Data.InstanceId)
					< std::tuple(ActiveData.PersistentId, ActiveData.SelectionKey,
						ActiveData.InstanceId)))
				Active = Candidate;
		}
		return Active;
	}

	auto FScene::GetActiveVolumetricCloud_RenderThread(
		FVolumetricCloudSceneData& OutCloud) const -> bool
	{
		const FVolumetricCloudSceneInfo* Info =
			GetActiveVolumetricCloudSceneInfo_RenderThread();
		if (Info == nullptr) return false;
		OutCloud = Info->GetProxy().GetData();
		return true;
	}

	auto FScene::GetVolumetricCloudCount_RenderThread() const -> size_t
	{
		CheckRenderingThread();
		return VolumetricCloudSceneInfos.size();
	}
}
