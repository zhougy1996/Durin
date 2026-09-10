#pragma once

#include "RHIResources.h"

namespace Durin
{
	// Linear infinite-sky radiance inputs, frozen together for every capture face.
	struct FProceduralSkyParameters
	{
		FVector3f ToSun{-0.405616f, 0.507020f, 0.760530f};
		FVector3f Up{0, 0, 1};
		FVector3f Zenith{0.18f, 0.28f, 0.50f};
		FVector3f Horizon{0.30f, 0.32f, 0.35f};
		FVector3f Ground{0.025f, 0.020f, 0.018f};
		FVector3f Halo{0.50f, 0.39f, 0.275f};
		FVector3f Tint{1};
		float HorizonExponent = 1;
		float HaloExponent = 16;
		float ExposureEV = 0;
	};

	// Scene-owned immutable analytic provider; translation and scale are absent by design.
	// Identical layout to ProceduralSky.slang. Both visible and capture shaders use it.
	struct FProceduralSkyUniform
	{
		FVector4f ToSunEnabled{0};
		FVector4f UpHorizonExponent{0};
		FVector4f ZenithExposure{0};
		FVector4f HorizonHaloExponent{0};
		FVector4f Ground{0};
		FVector4f Halo{0};
		FVector4f Tint{0};
	};

	inline auto MakeProceduralSkyUniform(const FProceduralSkyParameters& P) -> FProceduralSkyUniform
	{
		return {FVector4f(P.ToSun, 1), FVector4f(P.Up, P.HorizonExponent),
			FVector4f(P.Zenith, P.ExposureEV), FVector4f(P.Horizon, P.HaloExponent),
			FVector4f(P.Ground, 0), FVector4f(P.Halo, 0), FVector4f(P.Tint, 0)};
	}

	struct FProceduralSkySceneProxy
	{
		FGuid PersistentId;
		std::string SelectionKey;
		uint64 InstanceId = 0;
		uint64 Revision = 0;
		int32 Priority = 0;
		bool bEligible = false;
		FProceduralSkyParameters Parameters;
	};
}
