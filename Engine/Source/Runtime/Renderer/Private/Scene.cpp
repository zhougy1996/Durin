#include "Scene.h"
#include "SceneRegistry.h"

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

	FScene::FScene()
		: Lights(std::make_unique<FLightSceneRegistry>())
		, SkyBoxes(std::make_unique<FSkyBoxSceneRegistry>())
		, VolumetricClouds(std::make_unique<FVolumetricCloudSceneRegistry>())
	{
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

	FLightSceneInfo::FLightSceneInfo(FScene& InScene,
		std::shared_ptr<FLightSceneProxy> InProxy)
		: Scene(&InScene), Proxy(std::move(InProxy)), Kind(Proxy->GetKind())
	{
		Proxy->AttachToSceneInfo(this);
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

	FLightSceneInfo::~FLightSceneInfo()
	{
		Proxy->DetachFromSceneInfo(this);
	}

	FSkyBoxSceneInfo::FSkyBoxSceneInfo(FScene& InScene,
		std::shared_ptr<FSkyBoxSceneProxy> InProxy)
		: Scene(&InScene), Proxy(std::move(InProxy))
	{
		Proxy->AttachToSceneInfo(this);
	}

	FSkyBoxSceneInfo::~FSkyBoxSceneInfo()
	{
		Proxy->DetachFromSceneInfo(this);
	}

	FVolumetricCloudSceneInfo::FVolumetricCloudSceneInfo(FScene& InScene,
		std::shared_ptr<FVolumetricCloudSceneProxy> InProxy)
		: Scene(&InScene), Proxy(std::move(InProxy))
	{
		Proxy->AttachToSceneInfo(this);
	}

	FVolumetricCloudSceneInfo::~FVolumetricCloudSceneInfo()
	{
		Proxy->DetachFromSceneInfo(this);
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
		Lights->Clear();
		SkyBoxes->Clear();
		VolumetricClouds->Clear();
	}

	auto FLightSceneRegistry::Attach(FLightSceneInfo& Info) -> void
	{
		switch (Info.GetKind())
		{
		case ELightSceneProxyKind::Directional:
			Directional.push_back(&Info);
			break;
		case ELightSceneProxyKind::Point:
			Point.push_back(&Info);
			break;
		case ELightSceneProxyKind::Spot:
			Spot.push_back(&Info);
			break;
		}
	}

	auto FLightSceneRegistry::Detach(FLightSceneInfo& Info) -> void
	{
		switch (Info.GetKind())
		{
		case ELightSceneProxyKind::Directional: std::erase(Directional, &Info); break;
		case ELightSceneProxyKind::Point: std::erase(Point, &Info); break;
		case ELightSceneProxyKind::Spot: std::erase(Spot, &Info); break;
		}
	}

	auto FLightSceneRegistry::Add(
		FScene& Scene, std::shared_ptr<FLightSceneProxy> Proxy) -> void
	{
		check(Proxy != nullptr && Proxy->GetDesc().IsValid());
		FLightSceneProxy* RawProxy = Proxy.get();
		check(!InfosByProxy.contains(RawProxy));
		auto Info = std::make_unique<FLightSceneInfo>(Scene, std::move(Proxy));
		FLightSceneInfo* RawInfo = Info.get();
		const auto [It, bInserted] = InfosByProxy.emplace(RawProxy, std::move(Info));
		check(bInserted);
		Attach(*RawInfo);
	}

	auto FLightSceneRegistry::Remove(FLightSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		const auto Found = InfosByProxy.find(Proxy);
		checkf(Found != InfosByProxy.end(),
			"Attempted to remove an unknown light scene proxy.");
		if (Found == InfosByProxy.end()) return;
		Detach(*Found->second);
		InfosByProxy.erase(Found);
	}

	auto FLightSceneRegistry::Clear() -> void
	{
		Directional.clear(); Point.clear(); Spot.clear(); InfosByProxy.clear();
	}

	auto FScene::AddLight(std::unique_ptr<FLightSceneProxy> Proxy) -> bool
	{
		if (Proxy == nullptr || !Proxy->GetDesc().IsValid()) return false;
		std::shared_ptr<FLightSceneProxy> SharedProxy(std::move(Proxy));
		return TryEnqueueRenderCommand("AddLight",
			[this, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
				CheckRenderingThread();
				Lights->Add(*this, SharedProxy);
			});
	}

	auto FScene::RemoveLight(FLightSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		TryEnqueueRenderCommand("RemoveLight",
			[this, Proxy](FRHICommandListImmediate&) {
				CheckRenderingThread();
				Lights->Remove(Proxy);
			});
	}

	auto FScene::GetDirectionalLightSceneInfos() const
		-> const std::vector<FLightSceneInfo*>&
	{
		return Lights->GetDirectional();
	}

	auto FScene::GetPointLightSceneInfos() const
		-> const std::vector<FLightSceneInfo*>&
	{
		return Lights->GetPoint();
	}

	auto FScene::GetSpotLightSceneInfos() const
		-> const std::vector<FLightSceneInfo*>&
	{
		return Lights->GetSpot();
	}

	auto FSkyBoxSceneRegistry::Add(
		FScene& Scene, std::shared_ptr<FSkyBoxSceneProxy> Proxy) -> void
	{
		check(Proxy != nullptr && Proxy->GetDesc().IsValid());
		FSkyBoxSceneProxy* RawProxy = Proxy.get();
		check(!InfosByProxy.contains(RawProxy));
		auto Info = std::make_unique<FSkyBoxSceneInfo>(Scene, std::move(Proxy));
		FSkyBoxSceneInfo* RawInfo = Info.get();
		const auto [It, bInserted] = InfosByProxy.emplace(RawProxy, std::move(Info));
		check(bInserted);
		SceneInfos.push_back(RawInfo);
	}

	auto FSkyBoxSceneRegistry::Remove(FSkyBoxSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		const auto Found = InfosByProxy.find(Proxy);
		checkf(Found != InfosByProxy.end(),
			"Attempted to remove an unknown sky-box scene proxy.");
		if (Found == InfosByProxy.end()) return;
		std::erase(SceneInfos, Found->second.get());
		InfosByProxy.erase(Found);
	}

	auto FSkyBoxSceneRegistry::Clear() -> void
	{
		SceneInfos.clear(); InfosByProxy.clear();
	}

	auto FSkyBoxSceneRegistry::GetActive() const -> const FSkyBoxSceneInfo*
	{
		if (SceneInfos.empty()) return nullptr;
		return *std::ranges::min_element(SceneInfos,
			[](const FSkyBoxSceneInfo* A, const FSkyBoxSceneInfo* B) {
				const auto& ADesc = A->GetProxy().GetDesc();
				const auto& BDesc = B->GetProxy().GetDesc();
				return std::tuple(ADesc.PersistentId, ADesc.SelectionKey,
					ADesc.RuntimeId)
					< std::tuple(BDesc.PersistentId, BDesc.SelectionKey,
						BDesc.RuntimeId);
			});
	}

	auto FScene::AddSkyBox(std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> bool
	{
		if (Proxy == nullptr || !Proxy->GetDesc().IsValid()) return false;
		std::shared_ptr<FSkyBoxSceneProxy> SharedProxy(std::move(Proxy));
		return TryEnqueueRenderCommand("AddSkyBox",
			[this, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
				CheckRenderingThread();
				SkyBoxes->Add(*this, SharedProxy);
			});
	}

	auto FScene::RemoveSkyBox(FSkyBoxSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		TryEnqueueRenderCommand("RemoveSkyBox",
			[this, Proxy](FRHICommandListImmediate&) {
				CheckRenderingThread();
				SkyBoxes->Remove(Proxy);
			});
	}

	auto FScene::GetActiveSkyBoxSceneInfo_RenderThread() const
		-> const FSkyBoxSceneInfo*
	{
		CheckRenderingThread();
		return SkyBoxes->GetActive();
	}

	auto FScene::GetActiveSkyBox_RenderThread(
		FSkyBoxSceneSnapshot& OutSkyBox) const -> bool
	{
		const FSkyBoxSceneInfo* Info = GetActiveSkyBoxSceneInfo_RenderThread();
		if (Info == nullptr) return false;
		OutSkyBox.Desc = Info->GetProxy().GetDesc();
		return true;
	}

	auto FScene::GetSkyBoxCount_RenderThread() const -> size_t
	{
		CheckRenderingThread();
		return SkyBoxes->Num();
	}

	auto FVolumetricCloudSceneRegistry::Add(
		FScene& Scene, std::shared_ptr<FVolumetricCloudSceneProxy> Proxy) -> void
	{
		check(Proxy != nullptr && Proxy->GetDesc().IsValid());
		FVolumetricCloudSceneProxy* RawProxy = Proxy.get();
		check(!InfosByProxy.contains(RawProxy));
		auto Info = std::make_unique<FVolumetricCloudSceneInfo>(Scene,
			std::move(Proxy));
		FVolumetricCloudSceneInfo* RawInfo = Info.get();
		const auto [It, bInserted] = InfosByProxy.emplace(RawProxy, std::move(Info));
		check(bInserted);
		SceneInfos.push_back(RawInfo);
	}

	auto FVolumetricCloudSceneRegistry::Remove(
		FVolumetricCloudSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		const auto Found = InfosByProxy.find(Proxy);
		checkf(Found != InfosByProxy.end(),
			"Attempted to remove an unknown volumetric-cloud scene proxy.");
		if (Found == InfosByProxy.end()) return;
		std::erase(SceneInfos, Found->second.get());
		InfosByProxy.erase(Found);
	}

	auto FVolumetricCloudSceneRegistry::Clear() -> void
	{
		SceneInfos.clear(); InfosByProxy.clear();
	}

	auto FVolumetricCloudSceneRegistry::GetActive() const
		-> const FVolumetricCloudSceneInfo*
	{
		FVolumetricCloudSceneInfo* Active = nullptr;
		for (FVolumetricCloudSceneInfo* Candidate : SceneInfos)
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
			const FVolumetricCloudSceneProxyDesc& Desc =
				Candidate->GetProxy().GetDesc();
			const FVolumetricCloudSceneProxyDesc& ActiveDesc =
				Active->GetProxy().GetDesc();
			if (Data.Priority > ActiveData.Priority
				|| (Data.Priority == ActiveData.Priority
					&& std::tuple(Desc.PersistentId, Desc.SelectionKey,
						Desc.RuntimeId)
					< std::tuple(ActiveDesc.PersistentId,
						ActiveDesc.SelectionKey, ActiveDesc.RuntimeId)))
				Active = Candidate;
		}
		return Active;
	}

	auto FScene::AddVolumetricCloud(
		std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> bool
	{
		if (Proxy == nullptr || !Proxy->GetDesc().IsValid()) return false;
		std::shared_ptr<FVolumetricCloudSceneProxy> SharedProxy(std::move(Proxy));
		return TryEnqueueRenderCommand("AddVolumetricCloud",
			[this, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
				CheckRenderingThread();
				VolumetricClouds->Add(*this, SharedProxy);
			});
	}

	auto FScene::RemoveVolumetricCloud(
		FVolumetricCloudSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		TryEnqueueRenderCommand("RemoveVolumetricCloud",
			[this, Proxy](FRHICommandListImmediate&) {
				CheckRenderingThread();
				VolumetricClouds->Remove(Proxy);
			});
	}

	auto FScene::GetActiveVolumetricCloudSceneInfo_RenderThread() const
		-> const FVolumetricCloudSceneInfo*
	{
		CheckRenderingThread();
		return VolumetricClouds->GetActive();
	}

	auto FScene::GetActiveVolumetricCloud_RenderThread(
		FVolumetricCloudSceneSnapshot& OutCloud) const -> bool
	{
		const FVolumetricCloudSceneInfo* Info =
			GetActiveVolumetricCloudSceneInfo_RenderThread();
		if (Info == nullptr) return false;
		OutCloud.Desc = Info->GetProxy().GetDesc();
		return true;
	}

	auto FScene::GetVolumetricCloudCount_RenderThread() const -> size_t
	{
		CheckRenderingThread();
		return VolumetricClouds->Num();
	}
}
