#pragma once

#include "CoreMinimal.h"
#include "WaterAPI.h"
#include "Math/Vector.h"
#include "DObject/ObjectMacros.h"
#include "WaterSurfaceTypes.gen.h"

namespace Durin
{
	inline constexpr uint32 WaterWaveDataStart = 4;
	inline constexpr uint32 WaterWaveDataStride = 4;
	inline constexpr uint32 PrimitiveWaterWaves = 7;
	inline constexpr uint32 MaxWaterWaves = 16;
	inline constexpr uint32 InstanceWaterWaveStride = 3;
	using FWaterInstanceWaveData = std::array<float, (MaxWaterWaves - PrimitiveWaterWaves) * InstanceWaterWaveStride>;
	using FWaterWaveData = std::array<float, WaterWaveDataStart + WaterWaveDataStride * PrimitiveWaterWaves>;

	DSTRUCT()
	struct FWaterWave
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FVector2 Direction{1, 0};
		DPROPERTY(Edit)
		float AmplitudeRatio = 0;
		DPROPERTY(Edit)
		float WavenumberRatio = 1;

		auto operator==(const FWaterWave&) const -> bool = default;
	};

	DSTRUCT()
	struct FWaterSurfaceSettings
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FVector2 Extent{32, 32};
		DPROPERTY(Edit)
		float Elevation = 0;
		DPROPERTY(Edit)
		float Speed = 1;
		DPROPERTY(Edit)
		float Steepness = .3f;

		auto operator==(const FWaterSurfaceSettings&) const -> bool = default;
	};

	// Snapshot shared by the CPU query, mesh builder and material upload.
	struct FResolvedWaterSurface : FWaterSurfaceSettings
	{
		float Amplitude = 0;
		float Wavelength = 8;
		std::vector<FWaterWave> Waves;
	};

	struct FWaterSurfaceSample
	{
		FVector3 Position{0};
		FVector3 Normal{0, 0, 1};
	};

	WATER_API auto ValidateWaterSurface(const FResolvedWaterSurface& Settings) -> std::expected<void, std::string>;
	// Packs validated settings for the default material. Inactive slots are zero.
	WATER_API auto PackWaterWaveData(const FResolvedWaterSurface& Settings) -> FWaterWaveData;
	WATER_API auto PackWaterInstanceWaveData(const FResolvedWaterSurface& Settings) -> FWaterInstanceWaveData;
	// Parametric evaluation requires validated settings and finite coordinates/time.
	WATER_API auto EvaluateWaterSurface(const FResolvedWaterSurface& Settings, FVector2 UndisplacedXY, double Seconds) -> FWaterSurfaceSample;
	// Outside the deformed rectangle, invalid settings/coordinates/time: no surface.
	WATER_API auto QueryWaterSurface(const FResolvedWaterSurface& Settings, FVector2 DisplacedXY, double Seconds) -> std::optional<FWaterSurfaceSample>;
	WATER_API auto GetWaterMaterialTime() -> double;
}
