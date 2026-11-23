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
	// Keep the unrolled default material within its authored graph node limit.
	inline constexpr uint32 MaxWaterWaves = 5;
	using FWaterWaveData = std::array<float, WaterWaveDataStart + WaterWaveDataStride * MaxWaterWaves>;

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
		float Amplitude = .15f;
		DPROPERTY(Edit)
		float Wavelength = 8;
		DPROPERTY(Edit)
		float Speed = 1;
		DPROPERTY(Edit)
		float Steepness = .3f;
		DPROPERTY(Edit)
		std::vector<FWaterWave> Waves = {{{1, 0}, .6f, 1.f}, {{.6, .8}, .3f, 1.37f}, {{-.8, .6}, .1f, .73f}};

		auto operator==(const FWaterSurfaceSettings&) const -> bool = default;
	};

	struct FWaterSurfaceSample
	{
		FVector3 Position{0};
		FVector3 Normal{0, 0, 1};
	};

	// Generates an authored Gerstner wave group. The component copies and validates
	// the result, so generation never runs during surface queries or rendering.
	class IWaterWaveGenerator
	{
	public:
		virtual ~IWaterWaveGenerator() = default;
		virtual auto GenerateWaves() const -> std::vector<FWaterWave> = 0;
	};

	WATER_API auto ValidateWaterSurface(const FWaterSurfaceSettings& Settings) -> std::expected<void, std::string>;
	// Packs validated settings for the default material. Inactive slots are zero.
	WATER_API auto PackWaterWaveData(const FWaterSurfaceSettings& Settings) -> FWaterWaveData;
	// Parametric evaluation requires validated settings and finite coordinates/time.
	WATER_API auto EvaluateWaterSurface(const FWaterSurfaceSettings& Settings, FVector2 UndisplacedXY, double Seconds) -> FWaterSurfaceSample;
	// Outside the deformed rectangle, invalid settings/coordinates/time: no surface.
	WATER_API auto QueryWaterSurface(const FWaterSurfaceSettings& Settings, FVector2 DisplacedXY, double Seconds) -> std::optional<FWaterSurfaceSample>;
	WATER_API auto GetWaterMaterialTime() -> double;
}
