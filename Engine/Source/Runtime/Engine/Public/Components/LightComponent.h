#pragma once

#include "Components/SceneComponent.h"
#include "SceneTypes.h"

#ifdef _DHT_PARSER
namespace Durin
{
	struct FLinearColor
	{
		float R, G, B, A;
	};
}
#else
#include "Math/Color.h"
#endif

#include "LightComponent.gen.h"

namespace Durin
{
	class FLightSceneProxy;

	// Owns stable light identity and the shared game-to-render publication lifecycle.
	DCLASS(Abstract)
	class DLightComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DLightComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		auto GetLightSceneId() const -> FLightSceneId { return LightSceneId; }

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API static auto NormalizeLightColor(const FLinearColor& Color) -> FVector3f;
		ENGINE_API static auto NormalizeLightIntensity(float Intensity) -> float;
		virtual auto CreateSceneProxy() const -> std::unique_ptr<FLightSceneProxy> = 0;
		auto MarkLightRenderStateDirty() -> void;

	private:
		auto EnsureLightSceneId() -> FLightSceneId;

		FLightSceneId LightSceneId;
	};
}
