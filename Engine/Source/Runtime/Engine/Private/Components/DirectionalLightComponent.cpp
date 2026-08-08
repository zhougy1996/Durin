#include "Components/DirectionalLightComponent.h"

#include "Engine/Actor.h"
#include "Engine/LightSceneProxy.h"
#include "IScene.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextLightSceneId = 1;
	}

	auto DDirectionalLightComponent::OnRegister() -> void
	{
		Super::OnRegister();
		EnsureLightSceneId();
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::OnUnregister() -> void
	{
		if (IScene* Scene = GetRenderScene()) Scene->RemoveDirectionalLight(LightSceneId);
		Super::OnUnregister();
	}

	auto DDirectionalLightComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::GetSceneData() const -> FDirectionalLightSceneData
	{
		FDirectionalLightSceneData Result;
		Result.Direction = Math::Normalize(Math::RotateVector(GetWorldRotation(), FVectorConstants::Forward));
		Result.Color = FVector3f(
			std::clamp(Color.R, 0.0f, 1.0f),
			std::clamp(Color.G, 0.0f, 1.0f),
			std::clamp(Color.B, 0.0f, 1.0f));
		Result.Intensity = FMath::Max(0.0f, Intensity);
		Result.AmbientIntensity = FMath::Max(0.0f, AmbientIntensity);
		Result.RimLightIntensity = FMath::Max(0.0f, RimLightIntensity);
		return Result;
	}

	auto DDirectionalLightComponent::SetIntensity(float InIntensity) -> void
	{
		Intensity = FMath::Max(0.0f, InIntensity);
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::SetAmbientIntensity(float InIntensity) -> void
	{
		AmbientIntensity = FMath::Max(0.0f, InIntensity);
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::SetRimLightIntensity(float InIntensity) -> void
	{
		RimLightIntensity = FMath::Max(0.0f, InIntensity);
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		MarkLightRenderStateDirty();
	}

	auto DDirectionalLightComponent::EnsureLightSceneId() -> FLightSceneId
	{
		if (LightSceneId == InvalidLightSceneId)
			LightSceneId = FLightSceneId(GNextLightSceneId.fetch_add(1, std::memory_order_relaxed));
		return LightSceneId;
	}

	auto DDirectionalLightComponent::MarkLightRenderStateDirty() -> void
	{
		if (!IsRegistered()) return;
		IScene* Scene = GetRenderScene();
		if (Scene == nullptr) return;
		const FLightSceneId SceneId = EnsureLightSceneId();
		if (const AActor* Owner = GetOwner(); Owner && Owner->IsHidden())
		{
			Scene->RemoveDirectionalLight(SceneId);
			return;
		}
		Scene->AddOrReplaceDirectionalLight(
			SceneId,
			std::make_unique<FDirectionalLightSceneProxy>(GetSceneData()));
	}
}
