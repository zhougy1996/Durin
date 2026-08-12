#include "Components/LightComponent.h"

#include "Engine/Actor.h"
#include "Engine/LightSceneProxy.h"
#include "Math/Operations.h"

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
		MarkLightRenderStateDirty();
	}

	auto DLightComponent::OnUnregister() -> void
	{
		if (IScene* Scene = GetRenderScene()) Scene->RemoveLight(LightSceneId);
		Super::OnUnregister();
	}

	auto DLightComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkLightRenderStateDirty();
	}

	auto DLightComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkLightRenderStateDirty();
	}

	auto DLightComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		MarkLightRenderStateDirty();
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

	auto DLightComponent::MarkLightRenderStateDirty() -> void
	{
		if (!IsRegistered()) return;
		IScene* Scene = GetRenderScene();
		if (Scene == nullptr) return;
		const FLightSceneId SceneId = EnsureLightSceneId();
		if (const AActor* Owner = GetOwner(); Owner && Owner->IsHidden())
		{
			Scene->RemoveLight(SceneId);
			return;
		}
		Scene->AddOrReplaceLight(SceneId, CreateSceneProxy());
	}
}
