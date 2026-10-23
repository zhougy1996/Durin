#include "Components/PrimitiveComponent.h"

#include "CoreGlobals.h"
#include "DObject/WeakObjectPtr.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "IRendererModule.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SceneInterface.h"
#include "RenderingThread.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		std::unordered_set<FObjectKey> GPrecacheComponents;
		std::unordered_set<FObjectKey> GPendingPrecacheComponents;
		bool GProcessingPrecacheComponents = false;
		auto TrackPrecacheComponent(DPrimitiveComponent* Component) -> void
		{
			const FObjectKey Key(Component);
			GPrecacheComponents.insert(Key);
			GPendingPrecacheComponents.insert(Key);
		}
	}
	auto DPrimitiveComponent::UntrackPSOPrecache() -> void
	{
		const FObjectKey Key(this);
		GPrecacheComponents.erase(Key);
		GPendingPrecacheComponents.erase(Key);
	}
	auto DPrimitiveComponent::GetPendingPSOPrecacheComponentCount() -> size_t { return GPendingPrecacheComponents.size(); }
	auto DPrimitiveComponent::GetPSOPrecacheRenderer() const -> IRendererModule*
	{
		if (auto* Scene = GetRenderScene()) if (auto* Renderer = Scene->GetRendererModule()) return Renderer;
		return GEngine ? GEngine->GetRendererModule() : nullptr;
	}
	auto DPrimitiveComponent::PostLoad() -> void
	{
		Super::PostLoad();
		if (!IsTemplateObject()) { bPSOPrecacheSubmissionPending = true; TrackPrecacheComponent(this); }
	}
	auto DPrimitiveComponent::CollectPSOPrecacheData(const FPSOPrecacheParams&, FComponentPSOPrecacheParamsList&) -> void {}
	auto DPrimitiveComponent::PrecachePSOs() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (IsTemplateObject() || IsPendingKill() || bProcessingPSOPrecache) return;
		bProcessingPSOPrecache = true;
		struct FScope { bool& Flag; ~FScope() { Flag = false; } } Scope{bProcessingPSOPrecache};
		PSOPrecacheComponentData.Reset();
		PSOPrecacheDiagnostic.clear(); bPSOPrecacheSubmissionPending = false;
		FComponentPSOPrecacheParamsList Params;
		CollectPSOPrecacheData({}, Params);
		if (Params.size() > FPSOPrecacheComponentData::MaximumRequests)
		{
			PSOPrecacheDiagnostic = "Component PSO collection exceeds its capacity.";
			PSOPrecacheComponentData.SetMaterialPSOPrecacheData({FMaterialPSOPrecacheRequestID::Rejected(PSOPrecacheDiagnostic)});
			return;
		}
		std::vector<FMaterialPSOPrecacheRequestID> Requests;
		auto* Renderer = GetPSOPrecacheRenderer();
		if (!Params.empty()) TrackPrecacheComponent(this);
		// Validate readiness for the whole set before admitting any work. A missing
		// program must not discard and resubmit an already accepted prefix.
		for (const auto& Param : Params)
		{
			if (!Renderer || !GDynamicRHI || !GDynamicRHI->RHIGetCapabilities()
				|| !Param.MaterialInterface || !Param.MaterialInterface->GetRenderData().CompiledProgram)
			{
				bPSOPrecacheSubmissionPending = true;
				PSOPrecacheDiagnostic = "Waiting for Renderer, RHI and accepted material programs.";
				return;
			}
		}
		for (const auto& Param : Params)
		{
			std::vector<FMaterialPSOPrecacheRequestID> MaterialRequests;
			const auto Submitted = Param.MaterialInterface->PrecachePSOs(*Renderer, Param.VertexFactoryDataList,
				Param.PSOPrecacheParams, Param.Priority, MaterialRequests);
			if (!Submitted)
			{
				PSOPrecacheDiagnostic = Submitted.error();
				// Material admission has no aggregate capacity limit. Invalid/closed
				// requests are terminal; lower-level pressure stays inside the collector.
				Requests.push_back(FMaterialPSOPrecacheRequestID::Rejected(PSOPrecacheDiagnostic));
				PSOPrecacheComponentData.SetMaterialPSOPrecacheData(std::move(Requests));
				return;
			}
			for (auto& Request : MaterialRequests)
			{
				Request.BoostPriority(Param.Priority);
				Requests.push_back(std::move(Request));
			}
			if (Requests.size() > FPSOPrecacheComponentData::MaximumRequests)
			{
				PSOPrecacheDiagnostic = "Component PSO request set exceeds its capacity.";
				Requests = {FMaterialPSOPrecacheRequestID::Rejected(PSOPrecacheDiagnostic)};
				break;
			}
		}
		PSOPrecacheComponentData.SetMaterialPSOPrecacheData(std::move(Requests));
	}
	auto DPrimitiveComponent::CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority Priority) -> bool
	{
		if (!PSOPrecacheComponentData.IsPSOPrecacheCalled() && !bPSOPrecacheSubmissionPending) PrecachePSOs();
		return PSOPrecacheComponentData.CheckPSOPrecachingAndBoostPriority(Priority) || bPSOPrecacheSubmissionPending;
	}
	auto DPrimitiveComponent::IsPSOPrecacheReadyForProxyCreation() -> bool
	{
		const bool bVisible = IsVisible() && (!GetOwner() || !GetOwner()->IsHidden());
		CheckPSOPrecachingAndBoostPriority(bVisible ? EPSOPrecachePriority::Highest : EPSOPrecachePriority::High);
		// Custom scenes without a PSO backend retain their existing publication contract.
		return !GetPSOPrecacheRenderer() || PSOPrecacheProxyCreationStrategy == EPSOPrecacheProxyCreationStrategy::AlwaysCreate
			|| (!bPSOPrecacheSubmissionPending && PSOPrecacheComponentData.IsReadyForProxyCreation());
	}
	auto DPrimitiveComponent::SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy Strategy) -> void
	{
		if (PSOPrecacheProxyCreationStrategy == Strategy) return;
		PSOPrecacheProxyCreationStrategy = Strategy;
		RecreateRenderState();
	}
	auto DPrimitiveComponent::ProcessPSOPrecacheRequests(DWorld* World) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (GProcessingPrecacheComponents) return;
		static uint64 LastInvalidation = 0, LastShaderGeneration = 0;
		const auto Invalidation = FMaterialPSORequestManager::GetInvalidationSerial();
		const auto ShaderGeneration = GetShaderReloadGeneration();
		if (LastInvalidation != Invalidation || LastShaderGeneration != ShaderGeneration)
		{
			LastInvalidation = Invalidation; LastShaderGeneration = ShaderGeneration;
			InvalidatePSOPrecacheRequests();
		}
		GProcessingPrecacheComponents = true;
		struct FScope { ~FScope() { GProcessingPrecacheComponents = false; } } Scope;
		const auto Components = std::exchange(GPendingPrecacheComponents, {});
		for (const auto& Key : Components)
		{
			const FWeakObjectPtr Weak(Key);
			auto* Component = Cast<DPrimitiveComponent>(Weak.Get());
			if (!Component || Component->IsPendingKill()) { GPrecacheComponents.erase(Key); continue; }
			if (World && Component->GetPhysicsWorld() && Component->GetPhysicsWorld() != World)
			{ GPendingPrecacheComponents.insert(Key); continue; }
			const bool Stale = std::ranges::any_of(Component->PSOPrecacheComponentData.GetMaterialPSOPrecacheRequestIDs(),
				[](const auto& Request) { const auto State = Request.GetProgress().State;
					return State == EMaterialPSOPrecacheState::Stale || State == EMaterialPSOPrecacheState::Canceled; });
			if (Stale || Component->bPSOPrecacheSubmissionPending)
			{
				if (Component->IsRegistered() && Component->SceneProxy
					&& Component->PSOPrecacheProxyCreationStrategy == EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached)
					Component->DestroyRenderState();
				Component->PrecachePSOs();
			}
			Component->PSOPrecacheComponentData.Poll();
			Component = Cast<DPrimitiveComponent>(Weak.Get()); // Completion may destroy or unregister its owner.
			if (!Component || Component->IsPendingKill()) continue;
			if (Component->IsRegistered() && !Component->SceneProxy && Component->IsPSOPrecacheReadyForProxyCreation())
				Component->CreateRenderState();
			// Ready and failed sets leave the tick worklist. The weak registry is
			// inspected only for explicit material/device/shader invalidation.
			GPendingPrecacheComponents.erase(Key);
			if (Component->bPSOPrecacheSubmissionPending || Component->IsPSOPrecaching())
				TrackPrecacheComponent(Component);
		}
	}
	auto DPrimitiveComponent::InvalidatePSOPrecacheRequests() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		std::erase_if(GPrecacheComponents, [](const auto& Key) {
			const auto* Component = Cast<DPrimitiveComponent>(FWeakObjectPtr(Key).Get());
			return !Component || Component->IsPendingKill();
		});
		for (const auto& Key : GPrecacheComponents)
		{
			auto* Component = Cast<DPrimitiveComponent>(FWeakObjectPtr(Key).Get());
			if (!Component) continue;
			if (!std::ranges::any_of(Component->PSOPrecacheComponentData.GetMaterialPSOPrecacheRequestIDs(),
				[](const auto& Request) { const auto State = Request.GetProgress().State;
					return State == EMaterialPSOPrecacheState::Stale || State == EMaterialPSOPrecacheState::Canceled; })) continue;
			Component->bPSOPrecacheSubmissionPending = true;
			GPendingPrecacheComponents.insert(Key);
			if (Component->IsRegistered() && Component->SceneProxy
				&& Component->PSOPrecacheProxyCreationStrategy == EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached)
				if (auto* Scene = Component->GetRenderScene()) Scene->RemovePrimitive(Component);
		}
	}
}
