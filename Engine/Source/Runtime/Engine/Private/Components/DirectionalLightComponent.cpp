#include "Components/DirectionalLightComponent.h"

#include "Rendering/LightSceneProxy.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		auto NormalizeShadowBias(float Value, float Default) -> float
		{
			return std::isfinite(Value) ? std::clamp(Value, 0.0f, 4.0f) : Default;
		}
	}

	auto DDirectionalLightComponent::GetSceneData() const -> FDirectionalLightSceneData
	{
		FDirectionalLightSceneData Result;
		Result.Direction = Math::Normalize(Math::RotateVector(GetWorldRotation(), FVectorConstants::Forward));
		Result.Color = NormalizeLightColor(Color);
		Result.Intensity = NormalizeLightIntensity(Intensity);
		Result.AmbientIntensity = FMath::Max(0.0f, AmbientIntensity);
		Result.RimLightIntensity = FMath::Max(0.0f, RimLightIntensity);
		Result.bCastShadows = bCastShadows;
		Result.ShadowBias = {
			NormalizeShadowBias(ShadowDepthBias, 1.0f),
			NormalizeShadowBias(ShadowSlopeBias, 1.0f),
			NormalizeShadowBias(ShadowNormalBias, 0.0f)};
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

	auto DDirectionalLightComponent::SetShadowBias(
		float InDepthBias, float InSlopeBias, float InNormalBias) -> void
	{
		ShadowDepthBias = NormalizeShadowBias(InDepthBias, 1.0f);
		ShadowSlopeBias = NormalizeShadowBias(InSlopeBias, 1.0f);
		ShadowNormalBias = NormalizeShadowBias(InNormalBias, 0.0f);
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
