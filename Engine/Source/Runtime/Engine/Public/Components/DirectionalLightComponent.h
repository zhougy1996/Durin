#pragma once

#include "CoreMinimal.h"

#include "Components/LightComponent.h"

#include "DirectionalLightComponent.gen.h"

namespace Durin
{
	struct FDirectionalLightSceneData;

	// Publishes one directional light's color and intensity into the render scene.
	DCLASS()
	class DDirectionalLightComponent : public DLightComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto GetSceneData() const -> FDirectionalLightSceneData;
		ENGINE_API auto SetIntensity(float InIntensity) -> void;
		ENGINE_API auto SetAmbientIntensity(float InIntensity) -> void;
		ENGINE_API auto SetRimLightIntensity(float InIntensity) -> void;
		ENGINE_API auto SetCastShadows(bool bInCastShadows) -> void;
		// Finite strengths are clamped to [0,4]; non-finite values restore defaults.
		ENGINE_API auto SetShadowBias(float InDepthBias, float InSlopeBias, float InNormalBias) -> void;

	protected:
		auto CreateSceneProxy(FLightSceneProxyDesc Desc) const
			-> std::unique_ptr<FLightSceneProxy> override;

	private:
		DPROPERTY(Edit, MetaData="HideAlpha")
		FLinearColor Color{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		float AmbientIntensity = 0.08f;

		DPROPERTY(Edit, Category = "Shadows")
		bool bCastShadows = true;

		DPROPERTY(Edit, Category = "Shadows", DisplayName = "Depth Bias",
			ClampMin = "0", ClampMax = "4", Step = "0.01", Precision = 2,
			ToolTip = "Scales constant raster and receiver depth bias. 1 preserves the default; lower values reduce shadow separation but can reveal acne.")
		float ShadowDepthBias = 1.0f;

		DPROPERTY(Edit, Category = "Shadows", DisplayName = "Slope Bias",
			ClampMin = "0", ClampMax = "4", Step = "0.01", Precision = 2,
			ToolTip = "Scales slope-dependent raster depth bias. Lower values tighten contact on grazing surfaces but can reveal acne.")
		float ShadowSlopeBias = 1.0f;

		DPROPERTY(Edit, Category = "Shadows", DisplayName = "Normal Bias",
			ClampMin = "0", ClampMax = "4", Step = "0.01", Precision = 2,
			ToolTip = "Receiver offset in shadow texels along the geometric normal, increasing at grazing angles. 0 disables it; displacement is bounded.")
		float ShadowNormalBias = 0.0f;

		// Editor preview assistance. Runtime directional lights leave this disabled.
		float RimLightIntensity = 0.0f;
	};
}
