#include "Components/DirectionalLightComponent.h"

#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "IScene.h"

namespace Durin
{
	auto DDirectionalLightComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (GEngine != nullptr && GEngine->GetMainScene() != nullptr && (!GetOwner() || !GetOwner()->IsHidden())) GEngine->GetMainScene()->AddDirectionalLight(this);
	}

	auto DDirectionalLightComponent::OnUnregister() -> void
	{
		if (GEngine != nullptr && GEngine->GetMainScene() != nullptr) GEngine->GetMainScene()->RemoveDirectionalLight(this);
		Super::OnUnregister();
	}

	auto DDirectionalLightComponent::OnOwnerVisibilityChanged() -> void
	{
		if (!IsRegistered() || GEngine == nullptr || GEngine->GetMainScene() == nullptr) return;
		if (GetOwner() && GetOwner()->IsHidden())
			GEngine->GetMainScene()->RemoveDirectionalLight(this);
		else
			GEngine->GetMainScene()->AddDirectionalLight(this);
	}

	auto DDirectionalLightComponent::GetSceneData() const -> FDirectionalLightSceneData
	{
		FDirectionalLightSceneData Result;
		Result.Direction = glm::normalize(GetWorldRotation() * FVectorConstants::Forward);
		Result.Color = FVector3f(
			std::clamp(Color.R, 0.0f, 1.0f),
			std::clamp(Color.G, 0.0f, 1.0f),
			std::clamp(Color.B, 0.0f, 1.0f));
		Result.Intensity = FMath::Max(0.0f, Intensity);
		Result.AmbientIntensity = FMath::Max(0.0f, AmbientIntensity);
		return Result;
	}

	auto DDirectionalLightComponent::SetIntensity(float InIntensity) -> void
	{
		Intensity = FMath::Max(0.0f, InIntensity);
	}

	auto DDirectionalLightComponent::SetAmbientIntensity(float InIntensity) -> void
	{
		AmbientIntensity = FMath::Max(0.0f, InIntensity);
	}
}
