#include "Components/PointLightComponent.h"

#include "Rendering/LightSceneProxy.h"
#include "Math/Operations.h"

namespace Durin
{
	auto DPointLightComponent::GetSceneData() const -> FPointLightSceneData
	{
		FPointLightSceneData Result;
		Result.Position = GetWorldLocation();
		Result.Color = NormalizeLightColor(Color);
		Result.Intensity = NormalizeLightIntensity(Intensity);
		Result.Range = std::isfinite(Range) && Range > 0.0f ? Range : 1.0f;
		return Result;
	}

	auto DPointLightComponent::SetIntensity(float InIntensity) -> void
	{
		Intensity = NormalizeLightIntensity(InIntensity);
		MarkRenderStateDirty();
	}

	auto DPointLightComponent::SetRange(float InRange) -> void
	{
		Range = std::isfinite(InRange) && InRange > 0.0f ? InRange : 1.0f;
		MarkRenderStateDirty();
	}

	auto DPointLightComponent::CreateSceneProxy(FLightSceneProxyDesc Desc) const
		-> std::unique_ptr<FLightSceneProxy>
	{
		return std::make_unique<FPointLightSceneProxy>(
			std::move(Desc), GetSceneData());
	}
}
