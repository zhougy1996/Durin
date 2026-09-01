#include "Components/DirectionalLightComponent.h"

#include "Rendering/LightSceneProxy.h"
#include "Math/Operations.h"

namespace Durin
{
	auto DDirectionalLightComponent::GetSceneData() const -> FDirectionalLightSceneData
	{
		FDirectionalLightSceneData Result;
		Result.Direction = Math::Normalize(Math::RotateVector(GetWorldRotation(), FVectorConstants::Forward));
		Result.Color = NormalizeLightColor(Color);
		Result.Intensity = NormalizeLightIntensity(Intensity);
		Result.AmbientIntensity = FMath::Max(0.0f, AmbientIntensity);
		Result.RimLightIntensity = FMath::Max(0.0f, RimLightIntensity);
		Result.bCastShadows = bCastShadows;
		return Result;
	}

	auto DDirectionalLightComponent::SetIntensity(float InIntensity) -> void
	{
		Intensity = NormalizeLightIntensity(InIntensity);
		MarkRenderStateDirty();
	}

	auto DDirectionalLightComponent::SetAmbientIntensity(float InIntensity) -> void
	{
		AmbientIntensity = FMath::Max(0.0f, InIntensity);
		MarkRenderStateDirty();
	}

	auto DDirectionalLightComponent::SetRimLightIntensity(float InIntensity) -> void
	{
		RimLightIntensity = FMath::Max(0.0f, InIntensity);
		MarkRenderStateDirty();
	}

	auto DDirectionalLightComponent::SetCastShadows(bool bInCastShadows) -> void
	{
		bCastShadows = bInCastShadows;
		MarkRenderStateDirty();
	}

	auto DDirectionalLightComponent::CreateSceneProxy(
		FLightSceneProxyDesc Desc) const
		-> std::unique_ptr<FLightSceneProxy>
	{
		return std::make_unique<FDirectionalLightSceneProxy>(
			std::move(Desc), GetSceneData());
	}
}
