#include "Components/DirectionalLightComponent.h"

#include "Engine/Actor.h"
#include "IScene.h"
#include "Math/Operations.h"

namespace Durin
{
	auto DDirectionalLightComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (IScene* Scene = GetRenderScene(); Scene && (!GetOwner() || !GetOwner()->IsHidden())) Scene->AddDirectionalLight(this);
	}

	auto DDirectionalLightComponent::OnUnregister() -> void
	{
		if (IScene* Scene = GetRenderScene()) Scene->RemoveDirectionalLight(this);
		Super::OnUnregister();
	}

	auto DDirectionalLightComponent::OnOwnerVisibilityChanged() -> void
	{
		if (!IsRegistered()) return;
		IScene* Scene = GetRenderScene();
		if (Scene == nullptr) return;
		if (GetOwner() && GetOwner()->IsHidden())
			Scene->RemoveDirectionalLight(this);
		else
			Scene->AddDirectionalLight(this);
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
	}

	auto DDirectionalLightComponent::SetAmbientIntensity(float InIntensity) -> void
	{
		AmbientIntensity = FMath::Max(0.0f, InIntensity);
	}

	auto DDirectionalLightComponent::SetRimLightIntensity(float InIntensity) -> void
	{
		RimLightIntensity = FMath::Max(0.0f, InIntensity);
	}
}
