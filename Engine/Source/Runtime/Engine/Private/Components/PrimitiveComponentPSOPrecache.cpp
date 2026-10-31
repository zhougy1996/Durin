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
		EComponentPSOPrecacheMode GComponentPSOPrecacheMode = EComponentPSOPrecacheMode::Automatic;
		// Weak owners of automatic requests; used only to wake them on policy changes.
		std::unordered_set<FObjectKey> GPrecacheComponentOwners;
		std::unordered_set<FObjectKey> GPendingPrecacheComponents;
		bool GProcessingPrecacheComponents = false;
		auto TrackPrecacheComponent(DPrimitiveComponent* Component) -> void
		{
			const FObjectKey Key(Component);
			GPrecacheComponentOwners.insert(Key);
			GPendingPrecacheComponents.insert(Key);
		}
	}
	auto GetComponentPSOPrecacheMode() -> EComponentPSOPrecacheMode
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		return GComponentPSOPrecacheMode;
	}
	auto SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode Mode) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (GComponentPSOPrecacheMode == Mode) return;
		GComponentPSOPrecacheMode = Mode;
		GPendingPrecacheComponents.insert(GPrecacheComponentOwners.begin(), GPrecacheComponentOwners.end());
	}
	auto IsComponentPSOPrecachingEnabled() -> bool
	{
		switch (GetComponentPSOPrecacheMode())
		{
		case EComponentPSOPrecacheMode::Enabled: return true;
		case EComponentPSOPrecacheMode::Disabled: return false;
		default:
			// Use the host, not the current world: PIE/preview worlds in an
			// editor host retain the editor policy. Game hosts in editor-capable
			// builds still get the runtime default.
			return GEngine ? GEngine->GetInitialWorldType() == EWorldType::Game : !DURIN_WITH_EDITOR;
		}
	}
	auto DPrimitiveComponent::ResetPSOPrecacheIfDisabled() -> bool
	{
		if (IsComponentPSOPrecachingEnabled()) return false;
		UntrackPSOPrecache();
		if (PSOPrecacheComponentData.IsPSOPrecacheCalled()) PSOPrecacheComponentData.Reset();
		bPSOPrecacheSubmissionPending = false;
		PSOPrecacheDiagnostic.clear();
		return true;
	}
	auto DPrimitiveComponent::UntrackPSOPrecache() -> void
	{
		const FObjectKey Key(this);
		GPendingPrecacheComponents.erase(Key);
		GPrecacheComponentOwners.erase(Key);
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
		if (!IsTemplateObject() && !ResetPSOPrecacheIfDisabled())
		{ bPSOPrecacheSubmissionPending = true; TrackPrecacheComponent(this); }
	}
	auto DPrimitiveComponent::CollectPSOPrecacheData(const FPSOPrecacheParams&, FComponentPSOPrecacheParamsList&) -> void {}
	auto DPrimitiveComponent::PrecachePSOs() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (IsTemplateObject() || IsPendingKill() || bProcessingPSOPrecache) return;
		if (ResetPSOPrecacheIfDisabled()) return;
		bProcessingPSOPrecache = true;
		struct FScope { bool& Flag; ~FScope() { Flag = false; } } Scope{bProcessingPSOPrecache};
		const FObjectKey Owner(this);
		auto SetRequests = [&](std::vector<FMaterialPSOPrecacheRequestID> Requests) {
			PSOPrecacheComponentData.SetMaterialPSOPrecacheData(std::move(Requests), {}, [Owner] {
				auto* Component = Cast<DPrimitiveComponent>(FWeakObjectPtr(Owner).Get());
				if (!Component || Component->IsPendingKill()) return;
				GPendingPrecacheComponents.insert(Owner);
				// Keep the wake-up so the owner tick can release a withheld proxy,
				// but never withdraw a visible proxy after policy was disabled.
				if (!IsComponentPSOPrecachingEnabled()) return;
				const bool Stale = std::ranges::any_of(Component->PSOPrecacheComponentData.GetMaterialPSOPrecacheRequestIDs(),
					[](const auto& Request) { return !Request.IsCurrent(); });
				if (!Stale) return;
				Component->bPSOPrecacheSubmissionPending = true;
				if (Component->IsRegistered() && Component->SceneProxy
					&& Component->PSOPrecacheProxyCreationStrategy == EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached)
					if (auto* Scene = Component->GetRenderScene()) Scene->RemovePrimitive(Component);
			});
		};
		PSOPrecacheComponentData.Reset();
		PSOPrecacheDiagnostic.clear(); bPSOPrecacheSubmissionPending = false;
		const auto FeatureLevel = GetRenderScene() ? GetRenderScene()->GetFeatureLevel() : FSceneInterface::GetDefaultFeatureLevel();
		FComponentPSOPrecacheParamsList Params;
		FPSOPrecacheParams PrecacheParams;
		SetupPrecachePSOParams(PrecacheParams);
		CollectPSOPrecacheData(PrecacheParams, Params);
		if (Params.empty()) UntrackPSOPrecache();
		else TrackPrecacheComponent(this);
		if (Params.size() > FPSOPrecacheComponentData::MaximumRequests)
		{
			PSOPrecacheDiagnostic = "Component PSO collection exceeds its capacity.";
			SetRequests({FMaterialPSOPrecacheRequestID::Rejected(PSOPrecacheDiagnostic)});
			return;
		}
		std::vector<FMaterialPSOPrecacheRequestID> Requests;
		auto* Renderer = GetPSOPrecacheRenderer();
		// Validate readiness for the whole set before admitting any work. A missing
		// program must not discard and resubmit an already accepted prefix.
		for (const auto& Param : Params)
		{
			if (Renderer && GDynamicRHI && GDynamicRHI->RHIGetCapabilities() && Param.MaterialInterface)
				Param.MaterialInterface->CacheResourceShadersForRendering(FeatureLevel);
			if (!Renderer || !GDynamicRHI || !GDynamicRHI->RHIGetCapabilities()
				|| !Param.MaterialInterface || !Param.MaterialInterface->GetAcceptedCompiledProgram(FeatureLevel))
			{
				auto* StatusOwner = Param.MaterialInterface;
				while (StatusOwner && StatusOwner->IsDynamicInstance() && StatusOwner->GetParent()) StatusOwner = StatusOwner->GetParent();
				const auto State = StatusOwner ? StatusOwner->GetMaterialCompileStatus(FeatureLevel).State : EMaterialCompileState::NeverRequested;
				const bool Failed = State == EMaterialCompileState::Failed || State == EMaterialCompileState::Rejected
					|| State == EMaterialCompileState::Canceled;
				if (Renderer && GDynamicRHI && Param.MaterialInterface
					&& (Failed || (StatusOwner && !StatusOwner->GetMaterialCookDiagnostic(FeatureLevel).empty())))
				{
					PSOPrecacheDiagnostic = std::format("Material '{}' has no accepted program: {}",
						Param.MaterialInterface->GetObjectPath(), Param.MaterialInterface->GetMaterialCookDiagnostic(FeatureLevel));
					SetRequests({FMaterialPSOPrecacheRequestID::Rejected(PSOPrecacheDiagnostic)});
					return;
				}
				bPSOPrecacheSubmissionPending = true;
				PSOPrecacheDiagnostic = "Waiting for Renderer, RHI and accepted material programs.";
				return;
			}
		}
		for (const auto& Param : Params)
		{
			std::vector<FMaterialPSOPrecacheRequestID> MaterialRequests;
			const auto Submitted = Param.MaterialInterface->PrecachePSOs(*Renderer, FeatureLevel, Param.VertexFactoryDataList,
				Param.PSOPrecacheParams, Param.Priority, MaterialRequests);
			if (!Submitted)
			{
				PSOPrecacheDiagnostic = Submitted.error();
				// Material admission has no aggregate capacity limit. Invalid/closed
				// requests are terminal; lower-level pressure stays inside the collector.
				Requests.push_back(FMaterialPSOPrecacheRequestID::Rejected(PSOPrecacheDiagnostic));
				SetRequests(std::move(Requests));
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
		SetRequests(std::move(Requests));
	}
	auto DPrimitiveComponent::CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority Priority) -> bool
	{
		if (ResetPSOPrecacheIfDisabled()) return false;
		if (!PSOPrecacheComponentData.IsPSOPrecacheCalled() && !bPSOPrecacheSubmissionPending) PrecachePSOs();
		return PSOPrecacheComponentData.CheckPSOPrecachingAndBoostPriority(Priority) || bPSOPrecacheSubmissionPending;
	}
	auto DPrimitiveComponent::IsPSOPrecacheReadyForProxyCreation() -> bool
	{
		if (ResetPSOPrecacheIfDisabled()) return true;
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
		FPSOPrecacheComponentData::DispatchOwnerNotifications();
		const auto Components = std::exchange(GPendingPrecacheComponents, {});
		for (const auto& Key : Components)
		{
			const FWeakObjectPtr Weak(Key);
			auto* Component = Cast<DPrimitiveComponent>(Weak.Get());
			if (!Component || Component->IsPendingKill()) { GPrecacheComponentOwners.erase(Key); continue; }
			if (World && Component->GetPhysicsWorld() && Component->GetPhysicsWorld() != World)
			{ GPendingPrecacheComponents.insert(Key); continue; }
			if (Component->ResetPSOPrecacheIfDisabled())
			{
				if (Component->IsRegistered() && !Component->SceneProxy) Component->CreateRenderState();
				continue;
			}
			const bool Stale = std::ranges::any_of(Component->PSOPrecacheComponentData.GetMaterialPSOPrecacheRequestIDs(),
				[](const auto& Request) { return !Request.IsCurrent(); });
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
			// Accepted work wakes this owner through notifications; only unsubmitted
			// asset/RHI demand needs another host tick.
			GPendingPrecacheComponents.erase(Key);
			if (Component->bPSOPrecacheSubmissionPending)
				TrackPrecacheComponent(Component);
		}
	}

}
