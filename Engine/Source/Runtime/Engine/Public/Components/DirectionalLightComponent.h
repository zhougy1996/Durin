#pragma once

#include "Components/SceneComponent.h"
#include "IScene.h"

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

	// Publishes one directional light's color and intensity into the render scene.
	DCLASS()
	class DDirectionalLightComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto GetSceneData() const -> FDirectionalLightSceneData;
		ENGINE_API auto SetIntensity(float InIntensity) -> void;
		ENGINE_API auto SetAmbientIntensity(float InIntensity) -> void;
		ENGINE_API auto SetRimLightIntensity(float InIntensity) -> void;
		auto GetLightSceneId() const -> FLightSceneId { return LightSceneId; }

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		auto EnsureLightSceneId() -> FLightSceneId;
		auto MarkLightRenderStateDirty() -> void;

		FLightSceneId LightSceneId;
		DPROPERTY(Edit, MetaData="HideAlpha")
		FLinearColor Color{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		float AmbientIntensity = 0.08f;

		// Editor preview assistance. Runtime directional lights leave this disabled.
		float RimLightIntensity = 0.0f;
	};
}
