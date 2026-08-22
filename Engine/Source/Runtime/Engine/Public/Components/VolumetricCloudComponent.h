#pragma once

#include "Components/SceneComponent.h"
#include "Math/Vector.h"

#include "VolumetricCloudComponent.gen.h"

namespace Durin
{
	class DTexture2D;
	class DVolumeTexture;

	DCLASS(DisplayName = "Volumetric Cloud Component")
	// Publishes one revisioned immutable global-cloud candidate.
	class DVolumetricCloudComponent final : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DVolumetricCloudComponent(
			const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto PreEditChangeProperty(
			FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(
			const FPropertyChangedEvent& Event) -> void override;

		auto IsEnabled() const -> bool { return bEnabled; }
		auto GetPriority() const -> int32 { return Priority; }
		auto GetBaseDensityTexture() const -> DVolumeTexture* { return BaseDensityTexture.Get(); }
		auto GetDetailDensityTexture() const -> DVolumeTexture* { return DetailDensityTexture.Get(); }
		auto GetWeatherTexture() const -> DTexture2D* { return WeatherTexture.Get(); }
		auto GetMinimumZ() const -> double { return MinimumZ; }
		auto GetMaximumZ() const -> double { return MaximumZ; }
		auto GetMaximumDistance() const -> double { return MaximumDistance; }
		auto GetBaseFrequency() const -> const FVector3f& { return BaseFrequency; }
		auto GetDetailFrequency() const -> const FVector3f& { return DetailFrequency; }
		auto GetWindOffset() const -> const FVector3f& { return WindOffset; }
		auto GetWeatherFrequency() const -> const FVector2f& { return WeatherFrequency; }
		auto GetWeatherOffset() const -> const FVector2f& { return WeatherOffset; }
		auto GetCoverage() const -> float { return Coverage; }
		auto GetDetailErosion() const -> float { return DetailErosion; }
		auto GetExtinction() const -> float { return Extinction; }
		auto GetLightExtinction() const -> float { return LightExtinction; }
		auto GetAmbient() const -> float { return Ambient; }
		auto GetVolumetricCloudSceneId() const -> const FGuid& { return VolumetricCloudSceneId; }
		auto GetVolumetricCloudInstanceId() const -> uint64 { return VolumetricCloudInstanceId; }
		auto GetPublicationRevision() const -> uint64 { return PublicationRevision; }
		auto GetEligibilityStatus() const -> const std::string& { return EligibilityStatus; }
		ENGINE_API auto RefreshEligibilityDiagnostic() -> void;

		ENGINE_API auto SetEnabled(bool bInEnabled) -> void;
		ENGINE_API auto SetPriority(int32 InPriority) -> void;
		ENGINE_API auto SetBaseDensityTexture(DVolumeTexture* Texture) -> void;
		ENGINE_API auto SetDetailDensityTexture(DVolumeTexture* Texture) -> void;
		ENGINE_API auto SetWeatherTexture(DTexture2D* Texture) -> void;
		ENGINE_API auto SetLayer(double InMinimumZ, double InMaximumZ,
			double InMaximumDistance) -> void;
		ENGINE_API auto SetDensityMapping(const FVector3f& InBaseFrequency,
			const FVector3f& InDetailFrequency, const FVector3f& InWindOffset,
			const FVector2f& InWeatherFrequency,
			const FVector2f& InWeatherOffset) -> void;
		ENGINE_API auto SetOpticalProperties(float InCoverage,
			float InDetailErosion, float InExtinction,
			float InLightExtinction, float InAmbient) -> void;

	private:
		auto MarkVolumetricCloudRenderStateDirty() -> void;

		DPROPERTY(Edit)
		bool bEnabled = true;
		DPROPERTY(Edit)
		int32 Priority = 0;
		DPROPERTY(Edit)
		TObjectPtr<DVolumeTexture> BaseDensityTexture;
		DPROPERTY(Edit)
		TObjectPtr<DVolumeTexture> DetailDensityTexture;
		DPROPERTY(Edit)
		TObjectPtr<DTexture2D> WeatherTexture;
		DPROPERTY(Edit)
		double MinimumZ = 1500.0;
		DPROPERTY(Edit)
		double MaximumZ = 3500.0;
		DPROPERTY(Edit)
		double MaximumDistance = 100000.0;
		DPROPERTY(Edit)
		FVector3f BaseFrequency{0.00008f};
		DPROPERTY(Edit)
		FVector3f DetailFrequency{0.00032f};
		DPROPERTY(Edit)
		FVector3f WindOffset{0.0f};
		DPROPERTY(Edit)
		FVector2f WeatherFrequency{0.00004f};
		DPROPERTY(Edit)
		FVector2f WeatherOffset{0.0f};
		DPROPERTY(Edit)
		float Coverage = 0.55f;
		DPROPERTY(Edit)
		float DetailErosion = 0.30f;
		DPROPERTY(Edit)
		float Extinction = 0.0015f;
		DPROPERTY(Edit)
		float LightExtinction = 0.0020f;
		DPROPERTY(Edit)
		float Ambient = 0.12f;

		DPROPERTY(Edit, ReadOnly, Transient)
		std::string EligibilityStatus;

		DPROPERTY()
		FGuid VolumetricCloudSceneId;
		uint64 VolumetricCloudInstanceId = 0;
		uint64 PublicationRevision = 0;
	};
}
