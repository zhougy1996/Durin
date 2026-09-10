#pragma once

#include "Components/SceneComponent.h"
#include "SkyLightComponent.gen.h"

namespace Durin
{
	class DTextureCube;
	struct FSkyLightSceneProxy;
	struct FSkyLightUpdateStatus;

	// Selects the independent radiance source for diffuse and specular sky lighting.
	DENUM()
	enum class ESkyLightSourceMode : uint8
	{
		SpecifiedCube,
		CapturedSky,
	};

	// Publishes immutable lighting settings; filtering and GPU lifetime belong to Renderer.
	DCLASS(DisplayName = "Sky Light")
	class DSkyLightComponent final : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSkyLightComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto SetSource(ESkyLightSourceMode Mode, DTextureCube* Cube) -> void;
		ENGINE_API auto SetIntensity(float Value) -> void;
		ENGINE_API auto SetPriority(int32 Value) -> void;
		ENGINE_API auto SetEnabled(bool Value) -> void;
		ENGINE_API auto SetRefreshPolicy(bool bAutomatic, float Interval) -> void;
		// Coalesces with pending automatic requests; does not wait for GPU work.
		ENGINE_API auto Recapture() -> void;
		ENGINE_API auto RefreshEligibilityDiagnostic() -> void;
		ENGINE_API auto RefreshReloadedAssetBindings() -> void;
		auto GetUpdateStatus() const -> std::shared_ptr<const FSkyLightUpdateStatus> { return UpdateStatus; }
		auto GetSourceMode() const -> ESkyLightSourceMode { return SourceMode; }
		auto GetTextureCube() const -> DTextureCube* { return TextureCube.Get(); }
		auto GetIntensity() const -> float { return Intensity; }
		auto GetPriority() const -> int32 { return Priority; }
		auto GetPersistentId() const -> const FGuid& { return SkyLightSceneId; }
		auto GetEligibilityStatus() const -> const std::string& { return EligibilityStatus; }

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;

	private:
		ENGINE_API auto CreateSceneProxy() -> std::shared_ptr<const FSkyLightSceneProxy>;
		auto Publish() -> void;
		auto Remove() -> void;

		DPROPERTY(Edit)
		bool bEnabled = true;

		DPROPERTY(Edit)
		int32 Priority = 0;

		DPROPERTY(Edit)
		ESkyLightSourceMode SourceMode = ESkyLightSourceMode::SpecifiedCube;

		DPROPERTY(Edit)
		TObjectPtr<DTextureCube> TextureCube;

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		DPROPERTY(Edit)
		bool bAutomaticRefresh = true;

		DPROPERTY(Edit, Units = "Seconds")
		float RefreshInterval = 1.0f;

		DPROPERTY(Edit, ReadOnly, Transient)
		std::string EligibilityStatus;

		DPROPERTY()
		FGuid SkyLightSceneId;

		// Registration ownership remains distinct from duplicated persistent IDs.
		uint64 InstanceId = 0;
		uint64 SourceEpoch = 1;
		uint64 RequestSerial = 1;
		std::shared_ptr<FSkyLightUpdateStatus> UpdateStatus;
		const FSkyLightSceneProxy* SceneProxy = nullptr;
		friend class FScene;
	};
}
