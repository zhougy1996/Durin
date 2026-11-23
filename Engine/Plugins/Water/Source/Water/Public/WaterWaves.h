#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Engine/PrimaryDataAsset.h"
#include "WaterSurfaceTypes.h"
#include "WaterWaves.gen.h"

namespace Durin
{
	DSTRUCT()
	struct FWaterWavesSettings
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		uint32 Seed = 0;
		DPROPERTY(Edit)
		uint32 NumWaves = 16;
		DPROPERTY(Edit)
		float MinWavelength = 5.21f;
		DPROPERTY(Edit)
		float MaxWavelength = 60.f;
		DPROPERTY(Edit)
		float MinAmplitude = .04f;
		DPROPERTY(Edit)
		float MaxAmplitude = .8f;

		DPROPERTY(Edit)
		float Randomness = 0.f;
		DPROPERTY(Edit)
		float WavelengthFalloff = 2.f;
		DPROPERTY(Edit)
		float AmplitudeFalloff = 2.f;
		DPROPERTY(Edit)
		float WindAngleDeg = 0.f;
		DPROPERTY(Edit)
		float DirectionAngularSpreadDeg = 1325.f;

		auto operator==(const FWaterWavesSettings&) const -> bool = default;
	};

	struct FWaterWaveGroup
	{
		float BaseAmplitude;
		float BaseWavelength;
		std::vector<FWaterWave> Waves;
	};

	DCLASS(DisplayName = "Water Waves")
	class DWaterWaves final : public DPrimaryDataAsset
	{
		GENERATED_BODY()
	public:
		WATER_API explicit DWaterWaves(const FObjectInitializer& Initializer);
		auto GetGeneratorSettings() const -> const FWaterWavesSettings& { return Settings; }
		WATER_API auto SetGeneratorSettings(const FWaterWavesSettings& Value) -> std::expected<void, std::string>;
		// Owner-thread access. The result is valid until the next settings change.
		WATER_API auto GetWaveGroup() const -> const std::expected<FWaterWaveGroup, std::string>&;
		auto OnWavesChanged() -> TMulticastDelegate<void()>& { return WavesChanged; }

		WATER_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		WATER_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		WATER_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;

	private:
		DPROPERTY(Edit, Category = "Gerstner Generator")
		FWaterWavesSettings Settings;

		mutable std::optional<FWaterWavesSettings> GeneratedFrom;
		mutable std::expected<FWaterWaveGroup, std::string> GeneratedGroup;
		TMulticastDelegate<void()> WavesChanged;
	};
}
