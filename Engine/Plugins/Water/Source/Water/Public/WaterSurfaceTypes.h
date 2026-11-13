#pragma once

#include "CoreMinimal.h"
#include "WaterAPI.h"
#include "Math/Vector.h"
#include "DObject/ObjectMacros.h"
#include "WaterSurfaceTypes.gen.h"

namespace Durin
{
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

		auto operator==(const FWaterSurfaceSettings&) const -> bool = default;
	};

	struct FWaterSurfaceSample
	{
		FVector3 Position{0};
		FVector3 Normal{0, 0, 1};
	};

	// Fixed spectral ratios are shared by the CPU evaluator and material recipe.
	struct FWaterWave
	{
		FVector2 Direction;
		float AmplitudeRatio;
		float WavenumberRatio;
	};
	inline const std::array<FWaterWave, 3> WaterWaves{{
		{{1, 0}, .6f, 1.f}, {{.6, .8}, .3f, 1.37f}, {{-.8, .6}, .1f, .73f}}};

	WATER_API auto ValidateWaterSurface(const FWaterSurfaceSettings& Settings) -> std::expected<void, std::string>;
	// Parametric evaluation requires validated settings and finite coordinates/time.
	WATER_API auto EvaluateWaterSurface(const FWaterSurfaceSettings& Settings, FVector2 UndisplacedXY, double Seconds) -> FWaterSurfaceSample;
	// Outside the deformed rectangle, invalid settings/coordinates/time: no surface.
	WATER_API auto QueryWaterSurface(const FWaterSurfaceSettings& Settings, FVector2 DisplacedXY, double Seconds) -> std::optional<FWaterSurfaceSample>;
	WATER_API auto GetWaterMaterialTime() -> double;
}
