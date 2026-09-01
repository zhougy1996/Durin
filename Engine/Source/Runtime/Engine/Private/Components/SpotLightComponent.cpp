#include "Components/SpotLightComponent.h"

#include "Rendering/LightSceneProxy.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		auto NormalizeConeAngle(float Value) -> float
		{
			return std::isfinite(Value) ? std::clamp(Value, 0.0f, 89.0f) : 0.0f;
		}
	}

	auto DSpotLightComponent::GetSceneData() const -> FSpotLightSceneData
	{
		FSpotLightSceneData Result;
		Result.Position = GetWorldLocation();
		Result.Direction = Math::Normalize(
			Math::RotateVector(GetWorldRotation(), FVectorConstants::Forward));
		Result.Color = NormalizeLightColor(Color);
		Result.Intensity = NormalizeLightIntensity(Intensity);
		Result.Range = std::isfinite(Range) && Range > 0.0f ? Range : 1.0f;
		Result.OuterConeAngle = NormalizeConeAngle(OuterConeAngle);
		Result.InnerConeAngle = std::min(
			NormalizeConeAngle(InnerConeAngle), Result.OuterConeAngle);
		return Result;
	}

	auto DSpotLightComponent::SetIntensity(float InIntensity) -> void
	{
		Intensity = NormalizeLightIntensity(InIntensity);
		MarkRenderStateDirty();
	}

	auto DSpotLightComponent::SetRange(float InRange) -> void
	{
		Range = std::isfinite(InRange) && InRange > 0.0f ? InRange : 1.0f;
		MarkRenderStateDirty();
	}

	auto DSpotLightComponent::SetConeAngles(
		float InInnerDegrees, float InOuterDegrees) -> void
	{
		OuterConeAngle = NormalizeConeAngle(InOuterDegrees);
		InnerConeAngle = std::min(
			NormalizeConeAngle(InInnerDegrees), OuterConeAngle);
		MarkRenderStateDirty();
	}

	auto DSpotLightComponent::CreateSceneProxy(FLightSceneProxyDesc Desc) const
		-> std::unique_ptr<FLightSceneProxy>
	{
		return std::make_unique<FSpotLightSceneProxy>(
			std::move(Desc), GetSceneData());
	}
}
