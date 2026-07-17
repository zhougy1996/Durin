#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

namespace Durin
{
	class PrimitiveSceneProxy;
	class DDirectionalLightComponent;
	using FPrimitiveSceneId = uint64;
	inline constexpr FPrimitiveSceneId InvalidPrimitiveSceneId = 0;

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

		virtual auto AddOrReplacePrimitive(
			FPrimitiveSceneId PrimitiveId,
			std::unique_ptr<PrimitiveSceneProxy> Proxy,
			const FMatrix& Transform
		) -> void = 0;

		virtual auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void = 0;

		virtual auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void = 0;

		virtual auto UpdatePrimitiveMaterial(FPrimitiveSceneId PrimitiveId, const FMaterialRenderUpdate& Update) -> void = 0;

		virtual auto Release() -> void = 0;

		virtual auto AddDirectionalLight(DDirectionalLightComponent* Light) -> void = 0;
		virtual auto RemoveDirectionalLight(DDirectionalLightComponent* Light) -> void = 0;
		virtual auto GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool = 0;
	};
}
