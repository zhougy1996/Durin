#pragma once

#include "Components/SceneComponent.h"
#include "Math/Vector.h"
#include "ProceduralSkyComponent.gen.h"

namespace Durin
{
	struct FProceduralSkySceneProxy;

	// Authors the analytic sky shared by visible background and Sky Light capture.
	DCLASS(DisplayName = "Procedural Sky")
	class DProceduralSkyComponent final : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DProceduralSkyComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		// Local to-sun direction, normalized before publication; zero and nonfinite inputs are rejected.
		ENGINE_API auto SetSunDirection(const FVector3f& Direction) -> void;
		ENGINE_API auto SetRadianceColors(const FVector3f& Zenith, const FVector3f& Horizon, const FVector3f& Ground, const FVector3f& Halo) -> void;
		ENGINE_API auto SetExposure(float Value) -> void;
		ENGINE_API auto SetEnabled(bool Value) -> void;
		ENGINE_API auto SetPriority(int32 Value) -> void;
		auto GetSunDirection() const -> const FVector3f& { return SunDirection; }
		auto GetPersistentId() const -> const FGuid& { return ProceduralSkySceneId; }

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;

	private:
		ENGINE_API auto CreateSceneProxy() -> std::shared_ptr<const FProceduralSkySceneProxy>;
		auto Publish() -> void;

		DPROPERTY(Edit)
		bool bEnabled = true;

		DPROPERTY(Edit)
		int32 Priority = 0;

		DPROPERTY(Edit)
		FVector3f SunDirection{-0.4f, 0.5f, 0.75f};

		DPROPERTY(Edit)
		FVector3f ZenithColor{0.18f, 0.28f, 0.50f};

		DPROPERTY(Edit)
		FVector3f HorizonColor{0.30f, 0.32f, 0.35f};

		DPROPERTY(Edit)
		FVector3f GroundColor{0.025f, 0.020f, 0.018f};

		DPROPERTY(Edit)
		FVector3f HaloColor{0.50f, 0.39f, 0.275f};

		DPROPERTY(Edit)
		FVector3f Tint{1};

		DPROPERTY(Edit)
		float HorizonExponent = 1;

		DPROPERTY(Edit)
		float HaloExponent = 16;

		DPROPERTY(Edit)
		float ExposureEV = 0;

		DPROPERTY()
		FGuid ProceduralSkySceneId;

		uint64 InstanceId = 0;
		uint64 Revision = 0;
		const FProceduralSkySceneProxy* SceneProxy = nullptr;
		friend class FScene;
	};
}
