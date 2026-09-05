#include "Scene.h"
#include "SceneRegistry.h"

#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Actor.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"

#include "Math/Operations.h"
#include "RenderingThread.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::atomic<size_t> GActiveSceneCount = 0;
		std::atomic<size_t> GAllocatedSceneCount = 0;

		auto TransformBounds(const FBox& Bounds, const FMatrix& Transform) -> FBox
		{
			FBox Result;
			if (!Bounds.bIsValid || !Math::IsFinite(Transform)) return Result;
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Point(
					(Corner & 1u) != 0 ? Bounds.Max.x : Bounds.Min.x,
					(Corner & 2u) != 0 ? Bounds.Max.y : Bounds.Min.y,
					(Corner & 4u) != 0 ? Bounds.Max.z : Bounds.Min.z
				);
				const FVector4 Transformed = Transform * FVector4(Point, 1.0);
				Result.AddPoint(FVector3(Transformed));
			}
			return Result;
		}

		template<typename ComponentType>
		auto RequireComponentBoundary(ComponentType* Component, std::string_view Operation) -> void
		{
			requiref(IsInGameThread(), "{} must execute on the game thread.", Operation);
			requiref(Component != nullptr, "{} requires a component.", Operation);
			requiref(Component->IsRegistered(), "{} requires a registered component.", Operation);
		}
	} // namespace

	FScene::FScene()
		: Lights(std::make_unique<FLightSceneRegistry>())
		, SkyBoxes(std::make_unique<FSkyBoxSceneRegistry>())
		, VolumetricClouds(std::make_unique<FVolumetricCloudSceneRegistry>())
	{
		GActiveSceneCount.fetch_add(1, std::memory_order_relaxed);
		GAllocatedSceneCount.fetch_add(1, std::memory_order_relaxed);
	}

	FScene::~FScene()
	{
		requiref(IsInRenderingThread(), "Renderer scenes must be destroyed on the rendering thread.");
		requiref(LifecycleState.load(std::memory_order_acquire) == ELifecycleState::Released, "Renderer scenes must finish Release before destruction.");
		requiref(IsEmpty_RenderThread(), "Renderer scenes must have empty registries before destruction.");
		GAllocatedSceneCount.fetch_sub(1, std::memory_order_relaxed);
	}

	auto FScene::AddPrimitive(DPrimitiveComponent* Primitive) -> void
	{
		RequireComponentBoundary(Primitive, "AddPrimitive");
		requiref(Primitive->GetRenderScene() == this, "AddPrimitive requires the component to target this scene.");
		requiref(!Primitive->bSceneProxyPublished, "AddPrimitive cannot publish a component twice.");
		std::unique_ptr<FPrimitiveSceneProxy> Proxy = Primitive->CreateSceneProxy();
		if (Proxy == nullptr) return;
		const FPrimitiveSceneId Id = Primitive->EnsurePrimitiveSceneId();
		const AActor* Owner = Primitive->GetOwner();
		const bool bVisible = Primitive->IsVisible() && (Owner == nullptr || !Owner->IsHidden());
		const bool bAccepted = TryAddPrimitiveProxy(Id, std::move(Proxy), Primitive->GetRenderMatrix(), bVisible);
		requiref(bAccepted, "AddPrimitive was rejected after its render state was constructed.");
		Primitive->bSceneProxyPublished = true;
	}

	auto FScene::RemovePrimitive(DPrimitiveComponent* Primitive) -> void
	{
		RequireComponentBoundary(Primitive, "RemovePrimitive");
		requiref(Primitive->GetRenderScene() == this, "RemovePrimitive requires the component to target this scene.");
		if (!Primitive->bSceneProxyPublished) return;
		const bool bAccepted = TryRemovePrimitiveProxy(Primitive->GetPrimitiveSceneId());
		requiref(bAccepted, "RemovePrimitive was rejected for a published render state.");
		Primitive->bSceneProxyPublished = false;
	}

	auto FScene::AddLight(DLightComponent* Light) -> void
	{
		RequireComponentBoundary(Light, "AddLight");
		requiref(Light->GetRenderScene() == this, "AddLight requires the component to target this scene.");
		requiref(Light->SceneProxy == nullptr, "AddLight cannot publish a component twice.");
		if (const AActor* Owner = Light->GetOwner(); Owner && Owner->IsHidden()) return;
		auto Proxy = Light->CreateSceneProxy(FLightSceneProxyDesc{Light->EnsureLightSceneId()});
		if (Proxy == nullptr) return;
		FLightSceneProxy* Token = Proxy.get();
		const bool bAccepted = TryAddLightProxy(std::move(Proxy));
		requiref(bAccepted, "AddLight was rejected after its render state was constructed.");
		Light->SceneProxy = Token;
	}

	auto FScene::RemoveLight(DLightComponent* Light) -> void
	{
		RequireComponentBoundary(Light, "RemoveLight");
		requiref(Light->GetRenderScene() == this, "RemoveLight requires the component to target this scene.");
		if (Light->SceneProxy == nullptr) return;
		const bool bAccepted = TryRemoveLightProxy(Light->SceneProxy);
		requiref(bAccepted, "RemoveLight was rejected for a published render state.");
		Light->SceneProxy = nullptr;
	}

	auto FScene::AddSkyBox(DSkyBoxComponent* SkyBox) -> void
	{
		RequireComponentBoundary(SkyBox, "AddSkyBox");
		requiref(SkyBox->GetRenderScene() == this, "AddSkyBox requires the component to target this scene.");
		requiref(SkyBox->SceneProxy == nullptr, "AddSkyBox cannot publish a component twice.");
		auto Proxy = SkyBox->CreateSceneProxy();
		if (Proxy == nullptr) return;
		FSkyBoxSceneProxy* Token = Proxy.get();
		const bool bAccepted = TryAddSkyBoxProxy(std::move(Proxy));
		requiref(bAccepted, "AddSkyBox was rejected after its render state was constructed.");
		SkyBox->SceneProxy = Token;
	}

	auto FScene::RemoveSkyBox(DSkyBoxComponent* SkyBox) -> void
	{
		RequireComponentBoundary(SkyBox, "RemoveSkyBox");
		requiref(SkyBox->GetRenderScene() == this, "RemoveSkyBox requires the component to target this scene.");
		if (SkyBox->SceneProxy == nullptr) return;
		const bool bAccepted = TryRemoveSkyBoxProxy(SkyBox->SceneProxy);
		requiref(bAccepted, "RemoveSkyBox was rejected for a published render state.");
		SkyBox->SceneProxy = nullptr;
	}

	auto FScene::AddVolumetricCloud(DVolumetricCloudComponent* Cloud) -> void
	{
		RequireComponentBoundary(Cloud, "AddVolumetricCloud");
		requiref(Cloud->GetRenderScene() == this, "AddVolumetricCloud requires the component to target this scene.");
		requiref(Cloud->SceneProxy == nullptr, "AddVolumetricCloud cannot publish a component twice.");
		auto Proxy = Cloud->CreateSceneProxy();
		if (Proxy == nullptr) return;
		FVolumetricCloudSceneProxy* Token = Proxy.get();
		const bool bAccepted = TryAddVolumetricCloudProxy(std::move(Proxy));
		requiref(bAccepted, "AddVolumetricCloud was rejected after its render state was constructed.");
		Cloud->SceneProxy = Token;
	}

	auto FScene::RemoveVolumetricCloud(DVolumetricCloudComponent* Cloud) -> void
	{
		RequireComponentBoundary(Cloud, "RemoveVolumetricCloud");
		requiref(Cloud->GetRenderScene() == this, "RemoveVolumetricCloud requires the component to target this scene.");
		if (Cloud->SceneProxy == nullptr) return;
		const bool bAccepted = TryRemoveVolumetricCloudProxy(Cloud->SceneProxy);
		requiref(bAccepted, "RemoveVolumetricCloud was rejected for a published render state.");
		Cloud->SceneProxy = nullptr;
	}

	auto FScene::RequireActive(std::string_view Operation) const -> void
	{
		requiref(LifecycleState.load(std::memory_order_acquire) == ELifecycleState::Active, "{} requires an active renderer scene.", Operation);
	}

	auto FScene::Release() -> void
	{
		requiref(IsInGameThread(), "FScene::Release must execute on the game thread.");
		ELifecycleState Expected = ELifecycleState::Active;
		requiref(LifecycleState.compare_exchange_strong(Expected, ELifecycleState::Releasing, std::memory_order_acq_rel), "FScene::Release is a single-use operation.");
		PublishedSkyBoxProxy = nullptr;
		const bool bAccepted = TryEnqueueRenderCommand("ReleaseScene", [this](FRHICommandListImmediate&) {
			CheckRenderingThread();
			Clear_RenderThread();
			LifecycleState.store(ELifecycleState::Released, std::memory_order_release);
		});
		requiref(bAccepted, "FScene::Release must be admitted before render-command shutdown.");
		GActiveSceneCount.fetch_sub(1, std::memory_order_relaxed);
	}

	auto FScene::DestroyScene(FSceneInterface* Scene) -> void
	{
		auto* RendererScene = dynamic_cast<FScene*>(Scene);
		requiref(RendererScene != nullptr, "The renderer scene deleter received an incompatible scene.");
		requiref(RendererScene->LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active, "FScenePtr owners must call Release before reset or destruction.");
		const bool bAccepted = TryEnqueueRenderCommand("DestroyScene", [RendererScene](FRHICommandListImmediate&) {
			CheckRenderingThread();
			requiref(RendererScene->LifecycleState.load(std::memory_order_acquire) == ELifecycleState::Released, "Scene deletion must execute after its Release command.");
			delete RendererScene;
		});
		requiref(bAccepted, "Renderer scene deletion must be admitted before render-command shutdown.");
	}

	auto FScene::GetActiveSceneCount() -> size_t
	{
		return GActiveSceneCount.load(std::memory_order_acquire);
	}

	auto FScene::GetAllocatedSceneCount() -> size_t
	{
		return GAllocatedSceneCount.load(std::memory_order_acquire);
	}

	FLightSceneInfo::FLightSceneInfo(std::shared_ptr<FLightSceneProxy> InProxy)
		: Proxy(std::move(InProxy))
		, Kind(Proxy->GetKind())
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

	FVolumetricCloudSceneInfo::FVolumetricCloudSceneInfo(
		std::shared_ptr<FVolumetricCloudSceneProxy> InProxy)
		: Proxy(std::move(InProxy))
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

	FPrimitiveSceneInfo::FPrimitiveSceneInfo(FPrimitiveSceneId InId,
		std::shared_ptr<FPrimitiveSceneProxy> InProxy,
		const FMatrix& InTransform)
		: Id(InId)
		, Proxy(std::move(InProxy))
		, Kind(Proxy->GetKind())
		, Transform(InTransform)
		, LocalBounds(Proxy->GetLocalBounds())
		, WorldBounds(TransformBounds(LocalBounds, Transform))
	{
	}

	auto FPrimitiveSceneInfo::GetStaticMeshProxy() const -> FStaticMeshSceneProxy&
	{
		check(Kind == EPrimitiveSceneProxyKind::StaticMesh);
		return static_cast<FStaticMeshSceneProxy&>(*Proxy);
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

	auto FPrimitiveSceneInfo::UpdateSplineMeshDynamicData(
		FSplineMeshRenderDynamicData DynamicData
	) -> bool
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
		case EPrimitiveSceneProxyKind::SplineMesh: std::erase(SplineMeshSceneInfos, &Info); break;
		}
	}

	auto FScene::TryAddPrimitiveProxy(FPrimitiveSceneId PrimitiveId, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform, bool bVisible) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| PrimitiveId == InvalidPrimitiveSceneId || Proxy == nullptr
			|| !Math::IsFinite(Transform)) return false;
		std::shared_ptr<FPrimitiveSceneProxy> SharedProxy(std::move(Proxy));
		return TryEnqueueRenderCommand("AddPrimitive", [this, PrimitiveId, SharedProxy = std::move(SharedProxy), Transform, bVisible](FRHICommandListImmediate&) {
			CheckRenderingThread();
			checkf(!PrimitiveInfosById.contains(PrimitiveId), "A primitive scene ID cannot be published twice.");
		auto Info = std::make_unique<FPrimitiveSceneInfo>(PrimitiveId, SharedProxy, Transform);
			Info->SetVisible(bVisible);
			FPrimitiveSceneInfo* RawInfo = Info.get();
			PrimitiveSceneInfos.push_back(RawInfo);
			switch (RawInfo->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh: StaticMeshSceneInfos.push_back(RawInfo); break;
			case EPrimitiveSceneProxyKind::SplineMesh: SplineMeshSceneInfos.push_back(RawInfo); break;
			}
			PrimitiveInfosById.emplace(PrimitiveId, std::move(Info));
		});
	}

	auto FScene::UpdatePrimitiveVisibility(
		FPrimitiveSceneId PrimitiveId,
		bool bVisible
	) -> void
	{
		RequireActive("UpdatePrimitiveVisibility");
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		const bool bAccepted = TryEnqueueRenderCommand("UpdatePrimitiveVisibility", [this, PrimitiveId, bVisible](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId);
				Found != PrimitiveInfosById.end())
			{
				Found->second->SetVisible(bVisible);
			}
		});
		requiref(bAccepted, "UpdatePrimitiveVisibility command admission failed.");
	}

	auto FScene::TryRemovePrimitiveProxy(FPrimitiveSceneId PrimitiveId) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| PrimitiveId == InvalidPrimitiveSceneId) return false;
		return TryEnqueueRenderCommand("RemovePrimitive", [this, PrimitiveId](FRHICommandListImmediate&) {
			CheckRenderingThread();
			const auto Found = PrimitiveInfosById.find(PrimitiveId);
			if (Found == PrimitiveInfosById.end()) return;
			DetachPrimitive(*Found->second);
			PrimitiveInfosById.erase(Found);
		});
	}

	auto FScene::UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void
	{
		RequireActive("UpdatePrimitiveTransform");
		if (PrimitiveId == InvalidPrimitiveSceneId || !Math::IsFinite(Transform)) return;
		const bool bAccepted = TryEnqueueRenderCommand("UpdatePrimitiveTransform", [this, PrimitiveId, Transform](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId); Found != PrimitiveInfosById.end()) Found->second->SetTransform(Transform);
		});
		requiref(bAccepted, "UpdatePrimitiveTransform command admission failed.");
	}

	auto FScene::UpdatePrimitiveMaterialBinding(FPrimitiveSceneId PrimitiveId, const FMaterialRenderProxyBindingUpdate& Update) -> void
	{
		RequireActive("UpdatePrimitiveMaterialBinding");
		if (PrimitiveId == InvalidPrimitiveSceneId) return;
		const bool bAccepted = TryEnqueueRenderCommand("UpdatePrimitiveMaterialBinding", [this, PrimitiveId, Update](FRHICommandListImmediate&) {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId); Found != PrimitiveInfosById.end()) Found->second->UpdateMaterialBinding(Update);
		});
		requiref(bAccepted, "UpdatePrimitiveMaterialBinding command admission failed.");
	}

	auto FScene::UpdateSplineMeshDynamicData(
		FPrimitiveSceneId PrimitiveId,
		FSplineMeshRenderDynamicData DynamicData
	) -> void
	{
		RequireActive("UpdateSplineMeshDynamicData");
		if (PrimitiveId == InvalidPrimitiveSceneId || DynamicData.Revision == 0
			|| !DynamicData.LocalBounds.bIsValid) return;
		const bool bAccepted = TryEnqueueRenderCommand("UpdateSplineMeshDynamicData", [this, PrimitiveId, DynamicData = std::move(DynamicData)](FRHICommandListImmediate&) mutable {
			CheckRenderingThread();
			if (const auto Found = PrimitiveInfosById.find(PrimitiveId);
				Found != PrimitiveInfosById.end())
				Found->second->UpdateSplineMeshDynamicData(std::move(DynamicData));
		});
		requiref(bAccepted, "UpdateSplineMeshDynamicData command admission failed.");
	}

	auto FScene::IsEmpty_RenderThread() const -> bool
	{
		CheckRenderingThread();
		return PrimitiveInfosById.empty() && PrimitiveSceneInfos.empty()
			   && StaticMeshSceneInfos.empty()
			   && SplineMeshSceneInfos.empty()
			   && Lights->Num() == 0 && SkyBoxes->Num() == 0
			   && VolumetricClouds->Num() == 0;
	}

	auto FScene::Clear_RenderThread() -> void
	{
		CheckRenderingThread();
		PrimitiveSceneInfos.clear();
		StaticMeshSceneInfos.clear();
		SplineMeshSceneInfos.clear();
		PrimitiveInfosById.clear();
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

	auto FLightSceneRegistry::Add(std::shared_ptr<FLightSceneProxy> Proxy) -> void
	{
		check(Proxy != nullptr && Proxy->GetDesc().IsValid());
		FLightSceneProxy* RawProxy = Proxy.get();
		check(!InfosByProxy.contains(RawProxy));
		auto Info = std::make_unique<FLightSceneInfo>(std::move(Proxy));
		FLightSceneInfo* RawInfo = Info.get();
		const auto [It, bInserted] = InfosByProxy.emplace(RawProxy, std::move(Info));
		check(bInserted);
		Attach(*RawInfo);
	}

	auto FLightSceneRegistry::Remove(FLightSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		const auto Found = InfosByProxy.find(Proxy);
		checkf(Found != InfosByProxy.end(), "Attempted to remove an unknown light scene proxy.");
		if (Found == InfosByProxy.end()) return;
		Detach(*Found->second);
		InfosByProxy.erase(Found);
	}

	auto FLightSceneRegistry::Clear() -> void
	{
		Directional.clear();
		Point.clear();
		Spot.clear();
		InfosByProxy.clear();
	}

	auto FScene::TryAddLightProxy(std::unique_ptr<FLightSceneProxy> Proxy) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| Proxy == nullptr || !Proxy->GetDesc().IsValid()) return false;
		std::shared_ptr<FLightSceneProxy> SharedProxy(std::move(Proxy));
		return TryEnqueueRenderCommand("AddLight", [this, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
			CheckRenderingThread();
			Lights->Add(SharedProxy);
		});
	}

	auto FScene::TryRemoveLightProxy(FLightSceneProxy* Proxy) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| Proxy == nullptr) return false;
		return TryEnqueueRenderCommand("RemoveLight", [this, Proxy](FRHICommandListImmediate&) {
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
		std::shared_ptr<FSkyBoxSceneProxy> InProxy) -> void
	{
		check(InProxy != nullptr && Proxy == nullptr);
		Proxy = std::move(InProxy);
	}

	auto FSkyBoxSceneRegistry::Remove(FSkyBoxSceneProxy* Proxy) -> void
	{
		if (Proxy == nullptr) return;
		checkf(this->Proxy && this->Proxy.get() == Proxy,
			"Attempted to remove an unknown sky-box scene proxy.");
		if (!this->Proxy || this->Proxy.get() != Proxy) return;
		this->Proxy.reset();
	}

	auto FSkyBoxSceneRegistry::Clear() -> void
	{
		Proxy.reset();
	}

	auto FScene::TryAddSkyBoxProxy(std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| Proxy == nullptr || PublishedSkyBoxProxy != nullptr) return false;
		FSkyBoxSceneProxy* Token = Proxy.get();
		std::shared_ptr<FSkyBoxSceneProxy> SharedProxy(std::move(Proxy));
		const bool bAccepted = TryEnqueueRenderCommand("AddSkyBox", [this, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
			CheckRenderingThread();
			SkyBoxes->Add(SharedProxy);
		});
		if (bAccepted) PublishedSkyBoxProxy = Token;
		return bAccepted;
	}

	auto FScene::TryRemoveSkyBoxProxy(FSkyBoxSceneProxy* Proxy) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| Proxy == nullptr || Proxy != PublishedSkyBoxProxy) return false;
		const bool bAccepted = TryEnqueueRenderCommand("RemoveSkyBox", [this, Proxy](FRHICommandListImmediate&) {
			CheckRenderingThread();
			SkyBoxes->Remove(Proxy);
		});
		if (bAccepted) PublishedSkyBoxProxy = nullptr;
		return bAccepted;
	}

	auto FScene::GetSkyBoxProxy_RenderThread() const
		-> const FSkyBoxSceneProxy*
	{
		CheckRenderingThread();
		return SkyBoxes->Get();
	}

	auto FScene::GetSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool
	{
		const FSkyBoxSceneProxy* Proxy = GetSkyBoxProxy_RenderThread();
		if (Proxy == nullptr) return false;
		OutSkyBox = Proxy->GetData();
		return true;
	}

	auto FScene::GetSkyBoxCount_RenderThread() const -> size_t
	{
		CheckRenderingThread();
		return SkyBoxes->Num();
	}

	auto FVolumetricCloudSceneRegistry::Add(
		std::shared_ptr<FVolumetricCloudSceneProxy> Proxy) -> void
	{
		check(Proxy != nullptr && Proxy->GetDesc().IsValid());
		FVolumetricCloudSceneProxy* RawProxy = Proxy.get();
		check(!InfosByProxy.contains(RawProxy));
		auto Info = std::make_unique<FVolumetricCloudSceneInfo>(std::move(Proxy));
		FVolumetricCloudSceneInfo* RawInfo = Info.get();
		const auto [It, bInserted] = InfosByProxy.emplace(RawProxy, std::move(Info));
		check(bInserted);
		SceneInfos.push_back(RawInfo);
	}

	auto FVolumetricCloudSceneRegistry::Remove(
		FVolumetricCloudSceneProxy* Proxy
	) -> void
	{
		if (Proxy == nullptr) return;
		const auto Found = InfosByProxy.find(Proxy);
		checkf(Found != InfosByProxy.end(), "Attempted to remove an unknown volumetric-cloud scene proxy.");
		if (Found == InfosByProxy.end()) return;
		std::erase(SceneInfos, Found->second.get());
		InfosByProxy.erase(Found);
	}

	auto FVolumetricCloudSceneRegistry::Clear() -> void
	{
		SceneInfos.clear();
		InfosByProxy.clear();
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
					&& std::pair(Desc.PersistentId, Desc.SelectionKey)
						   < std::pair(ActiveDesc.PersistentId, ActiveDesc.SelectionKey)))
				Active = Candidate;
		}
		return Active;
	}

	auto FScene::TryAddVolumetricCloudProxy(
		std::unique_ptr<FVolumetricCloudSceneProxy> Proxy
	) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| Proxy == nullptr || !Proxy->GetDesc().IsValid()) return false;
		std::shared_ptr<FVolumetricCloudSceneProxy> SharedProxy(std::move(Proxy));
		return TryEnqueueRenderCommand("AddVolumetricCloud", [this, SharedProxy = std::move(SharedProxy)](FRHICommandListImmediate&) {
			CheckRenderingThread();
			VolumetricClouds->Add(SharedProxy);
		});
	}

	auto FScene::TryRemoveVolumetricCloudProxy(
		FVolumetricCloudSceneProxy* Proxy
	) -> bool
	{
		if (LifecycleState.load(std::memory_order_acquire) != ELifecycleState::Active
			|| Proxy == nullptr) return false;
		return TryEnqueueRenderCommand("RemoveVolumetricCloud", [this, Proxy](FRHICommandListImmediate&) {
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
		FVolumetricCloudSceneSnapshot& OutCloud
	) const -> bool
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
} // namespace Durin
