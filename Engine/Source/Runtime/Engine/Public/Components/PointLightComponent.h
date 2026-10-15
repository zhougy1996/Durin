#pragma once

#include "CoreMinimal.h"

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
		auto CreateSceneProxy(FLightSceneProxyDesc Desc) const
			-> std::unique_ptr<FLightSceneProxy> override;

	private:
		DFUNCTION(CallInEditor, Category = "Light", DisplayName = "Normalize Settings",
			ToolTip = "Clamp authored light color, intensity, and range to their supported values.")
		ENGINE_API auto NormalizeSettings() -> void;

		DPROPERTY(Category = "Light", Edit, MetaData="HideAlpha")
		FLinearColor Color{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Category = "Light", Edit)
		float Intensity = 1.0f;

		DPROPERTY(Category = "Light", Edit)
		float Range = 10.0f;
	};
}
