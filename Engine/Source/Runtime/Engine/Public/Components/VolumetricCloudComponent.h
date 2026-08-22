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

		DPROPERTY(Edit, Category = "Activation", ToolTip = "Enables this global cloud candidate.")
		bool bEnabled = true;
		DPROPERTY(Edit, Category = "Activation", ToolTip = "Higher priority eligible clouds win deterministic scene selection.")
		int32 Priority = 0;
		DPROPERTY(Edit, Category = "Density Inputs", ToolTip = "Required low-frequency shape density volume.")
		TObjectPtr<DVolumeTexture> BaseDensityTexture;
		DPROPERTY(Edit, Category = "Density Inputs", ToolTip = "Required high-frequency erosion density volume.")
		TObjectPtr<DVolumeTexture> DetailDensityTexture;
		DPROPERTY(Edit, Category = "Density Inputs", ToolTip = "Optional two-dimensional coverage control; white is used when absent.")
		TObjectPtr<DTexture2D> WeatherTexture;
		DPROPERTY(Edit, Category = "Layer", Units = "Meters")
		double MinimumZ = 1500.0;
		DPROPERTY(Edit, Category = "Layer", Units = "Meters")
		double MaximumZ = 3500.0;
		DPROPERTY(Edit, Category = "Layer", Units = "Meters")
		double MaximumDistance = 100000.0;
		DPROPERTY(Edit, Category = "Mapping and Motion", ToolTip = "World-to-base-volume coordinate frequency.")
		FVector3f BaseFrequency{0.00008f};
		DPROPERTY(Edit, Category = "Mapping and Motion", ToolTip = "World-to-detail-volume coordinate frequency.")
		FVector3f DetailFrequency{0.00032f};
		DPROPERTY(Edit, Category = "Mapping and Motion", ToolTip = "Authored volume-coordinate translation used for wind animation.")
		FVector3f WindOffset{0.0f};
		DPROPERTY(Edit, Category = "Mapping and Motion")
		FVector2f WeatherFrequency{0.00004f};
		DPROPERTY(Edit, Category = "Mapping and Motion")
		FVector2f WeatherOffset{0.0f};
		DPROPERTY(Edit, Category = "Optical Response", ToolTip = "Global density coverage threshold.")
		float Coverage = 0.55f;
		DPROPERTY(Edit, Category = "Optical Response", ToolTip = "Detail-volume erosion strength.")
		float DetailErosion = 0.30f;
		DPROPERTY(Edit, Category = "Optical Response", Precision = 6, ToolTip = "View-ray extinction coefficient.")
		float Extinction = 0.0015f;
		DPROPERTY(Edit, Category = "Optical Response", Precision = 6, ToolTip = "Light-ray and receiver-shadow extinction coefficient.")
		float LightExtinction = 0.0020f;
		DPROPERTY(Edit, Category = "Optical Response", ToolTip = "Bounded ambient lighting contribution.")
		float Ambient = 0.12f;

		DPROPERTY(Edit, ReadOnly, Transient)
		std::string EligibilityStatus;

		DPROPERTY()
		FGuid VolumetricCloudSceneId;
		uint64 VolumetricCloudInstanceId = 0;
		uint64 PublicationRevision = 0;
	};
}
