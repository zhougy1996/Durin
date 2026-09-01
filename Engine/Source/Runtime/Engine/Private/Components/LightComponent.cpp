#include "Components/LightComponent.h"

#include "Engine/Actor.h"
#include "SceneInterface.h"
#include "Math/Operations.h"
#include "Rendering/LightSceneProxy.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextLightSceneId = 1;
	}

	DLightComponent::DLightComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DLightComponent::OnRegister() -> void
	{
		Super::OnRegister();
		EnsureLightSceneId();
		CreateRenderState();
	}

	auto DLightComponent::OnUnregister() -> void
	{
		DestroyRenderState();
		Super::OnUnregister();
	}

	auto DLightComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkRenderStateDirty();
	}

	auto DLightComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkRenderStateDirty();
	}

	auto DLightComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit)) return;
		MarkRenderStateDirty();
	}

	auto DLightComponent::NormalizeLightColor(const FLinearColor& Color) -> FVector3f
	{
		auto Normalize = [](float Value) {
			return std::isfinite(Value) ? std::clamp(Value, 0.0f, 1.0f) : 0.0f;
		};
		return FVector3f(Normalize(Color.R), Normalize(Color.G), Normalize(Color.B));
	}

	auto DLightComponent::NormalizeLightIntensity(float Intensity) -> float
	{
		return std::isfinite(Intensity) ? FMath::Max(0.0f, Intensity) : 0.0f;
	}

	auto DLightComponent::EnsureLightSceneId() -> FLightSceneId
	{
		if (LightSceneId == InvalidLightSceneId)
			LightSceneId = FLightSceneId(GNextLightSceneId.fetch_add(1, std::memory_order_relaxed));
		return LightSceneId;
	}

	auto DLightComponent::CreateRenderState() -> void
	{
		if (!IsRegistered()) return;
		if (FSceneInterface* Scene = GetRenderScene()) Scene->AddLight(this);
	}

	auto DLightComponent::DestroyRenderState() -> void
	{
		if (!IsRegistered()) return;
		if (FSceneInterface* Scene = GetRenderScene()) Scene->RemoveLight(this);
	}

	auto DLightComponent::MarkRenderStateDirty() -> void
	{
		if (!IsRegistered()) return;
		DestroyRenderState();
		CreateRenderState();
	}
} // namespace Durin
