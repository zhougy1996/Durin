#pragma once

#include "Components/SceneComponent.h"

#include "DirectionalLightComponent.gen.h"

namespace Durin
{
	struct FDirectionalLightSceneData;

	DCLASS()
	class DDirectionalLightComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto GetSceneData() const -> FDirectionalLightSceneData;

	private:
		DPROPERTY(Edit)
		float ColorR = 1.0f;

		DPROPERTY(Edit)
		float ColorG = 1.0f;

		DPROPERTY(Edit)
		float ColorB = 1.0f;

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		float AmbientIntensity = 0.08f;
	};
}
