#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class DPrimitiveComponent;
	class DDirectionalLightComponent;

	struct FDirectionalLightSceneData
	{
		FVector3 Direction{-0.5, -0.5, -1.0};
		FVector3f Color{1.0f, 1.0f, 1.0f};
		float Intensity = 1.0f;
		float AmbientIntensity = 0.08f;
	};

	class IScene
	{
	public:
		ENGINE_API IScene() = default;
		ENGINE_API virtual ~IScene() = default;

		virtual auto AddPrimitive(DPrimitiveComponent* Primitive) -> void = 0;

		virtual auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void = 0;

		virtual auto UpdatePrimitiveTransform(DPrimitiveComponent* Primitive) -> void = 0;

		virtual auto AddDirectionalLight(DDirectionalLightComponent* Light) -> void = 0;
		virtual auto RemoveDirectionalLight(DDirectionalLightComponent* Light) -> void = 0;
		virtual auto GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool = 0;
	};
}
