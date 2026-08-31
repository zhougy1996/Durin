#pragma once

#include "Components/LightComponent.h"

#include "PointLightComponent.gen.h"

namespace Durin
{
	struct FPointLightSceneData;

	// Publishes a finite-radius omnidirectional light into the render scene.
	DCLASS()
	class DPointLightComponent : public DLightComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto GetSceneData() const -> FPointLightSceneData;
		ENGINE_API auto SetIntensity(float InIntensity) -> void;
		ENGINE_API auto SetRange(float InRange) -> void;

	protected:
		auto CreateSceneProxy() const -> std::unique_ptr<FLightSceneProxy> override;

	private:
		DPROPERTY(Edit, MetaData="HideAlpha")
		FLinearColor Color{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		float Range = 10.0f;
	};
}
