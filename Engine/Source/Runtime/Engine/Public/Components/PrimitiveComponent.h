#pragma once
#include "Components/SceneComponent.h"

#include "PrimitiveComponent.gen.h"

namespace Durin
{
	class PrimitiveSceneProxy;

	DCLASS()
	class DPrimitiveComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;

		ENGINE_API virtual auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>;
		ENGINE_API auto GetRenderMatrix() const -> FMatrix;
	};
}
