#pragma once

#include "Components/LightComponent.h"

#include "SpotLightComponent.gen.h"

namespace Durin
{
	struct FSpotLightSceneData;

	// Publishes a finite-radius conical light into the render scene.
	DCLASS()
	class DSpotLightComponent : public DLightComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto GetSceneData() const -> FSpotLightSceneData;
		ENGINE_API auto SetIntensity(float InIntensity) -> void;
		ENGINE_API auto SetRange(float InRange) -> void;
		ENGINE_API auto SetConeAngles(float InInnerDegrees, float InOuterDegrees) -> void;

	protected:
		auto CreateSceneProxy(FLightSceneProxyDesc Desc) const
			-> std::unique_ptr<FLightSceneProxy> override;

	private:
		DPROPERTY(Edit, MetaData="HideAlpha")
		FLinearColor Color{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		float Range = 10.0f;

		DPROPERTY(Edit)
		float InnerConeAngle = 30.0f;

		DPROPERTY(Edit)
		float OuterConeAngle = 45.0f;
	};
}
