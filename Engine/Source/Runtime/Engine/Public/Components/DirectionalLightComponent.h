#pragma once

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

		DPROPERTY(Edit)
		bool bCastShadows = true;

		// Editor preview assistance. Runtime directional lights leave this disabled.
		float RimLightIntensity = 0.0f;
	};
}
