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

namespace Durin
{
	namespace
	{
		std::vector<FWeakObjectPtr> GPrecacheComponents;
		bool GProcessingPrecacheComponents = false;
		auto TrackPrecacheComponent(DPrimitiveComponent* Component) -> void
		{
			const FWeakObjectPtr Weak(Component);
			if (std::ranges::find(GPrecacheComponents, Weak) == GPrecacheComponents.end()) GPrecacheComponents.push_back(Weak);
		}
	}
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
		for (const auto& Param : Params)
		{
			if (!Renderer || !GDynamicRHI || !GDynamicRHI->RHIGetCapabilities()
				|| !Param.MaterialInterface || !Param.MaterialInterface->GetRenderData().CompiledProgram)
			{
				bPSOPrecacheSubmissionPending = true;
				PSOPrecacheDiagnostic = "Waiting for Renderer, RHI and accepted material programs.";
				return;
			}
			std::vector<FMaterialPSOPrecacheRequestID> MaterialRequests;
			const auto Submitted = Param.MaterialInterface->PrecachePSOs(*Renderer, Param.VertexFactoryDataList,
				Param.PSOPrecacheParams, Param.Priority, MaterialRequests);
			if (!Submitted)
			{
				PSOPrecacheDiagnostic = Submitted.error();
				// Admission pressure is retried on owner ticks, without publishing a partial set.
				bPSOPrecacheSubmissionPending = true;
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
		if (!PSOPrecacheComponentData.IsPSOPrecacheCalled() || bPSOPrecacheSubmissionPending) PrecachePSOs();
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
		GProcessingPrecacheComponents = true;
		struct FScope { ~FScope() { GProcessingPrecacheComponents = false; } } Scope;
		const auto Components = GPrecacheComponents;
		for (const auto& Weak : Components)
		{
			auto* Component = Cast<DPrimitiveComponent>(Weak.Get());
			if (!Component || Component->IsPendingKill()) continue;
			if (World && Component->GetPhysicsWorld() != World) { TrackPrecacheComponent(Component); continue; }
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
			if (Component->IsRegistered() || Component->bPSOPrecacheSubmissionPending || Component->IsPSOPrecaching())
				TrackPrecacheComponent(Component);
		}
		std::erase_if(GPrecacheComponents, [](const auto& Weak) {
			const auto* Component = Cast<DPrimitiveComponent>(Weak.Get());
			return !Component || Component->IsPendingKill() || (!Component->IsRegistered() && !Component->IsPSOPrecaching());
		});
	}
	auto DPrimitiveComponent::InvalidatePSOPrecacheRequests() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		for (const auto& Weak : GPrecacheComponents)
		{
			auto* Component = Cast<DPrimitiveComponent>(Weak.Get());
			if (!Component || !Component->IsRegistered() || !Component->SceneProxy) continue;
			if (!std::ranges::any_of(Component->PSOPrecacheComponentData.GetMaterialPSOPrecacheRequestIDs(),
				[](const auto& Request) { return Request.GetProgress().State == EMaterialPSOPrecacheState::Stale; })) continue;
			Component->bPSOPrecacheSubmissionPending = true;
			if (Component->PSOPrecacheProxyCreationStrategy == EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached)
				if (auto* Scene = Component->GetRenderScene()) Scene->RemovePrimitive(Component);
		}
	}
}
