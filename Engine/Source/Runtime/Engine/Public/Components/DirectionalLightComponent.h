#pragma once

#include "Components/SceneComponent.h"

#ifdef _DHT_PARSER
namespace Durin
{
	// DHT only needs the storage shape; parsing the full color header through an
	// older libclang fails against newer MSVC STL headers before reaching this class.
	struct FLinearColor
	{
		float R, G, B, A;
	};
}
#else
#include "Math/Color.h"
#endif

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
		DPROPERTY(Edit, MetaData="HideAlpha")
		FLinearColor Color{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		float AmbientIntensity = 0.08f;
	};
}
