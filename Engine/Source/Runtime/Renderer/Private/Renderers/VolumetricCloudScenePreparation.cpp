#include "Renderers/VolumetricCloudScenePreparation.h"

#include <algorithm>
#include <bit>

namespace Durin
{
	auto BuildVolumetricCloudParameters(
		const FVolumetricCloudSceneData& Cloud,
		const FPreparedLightView& Lights
	)
		-> FVolumetricCloudSpatialRenderer::FParameters
	{
		FVolumetricCloudSpatialRenderer::FParameters Result;
		Result.MinimumZ = Cloud.MinimumZ;
		Result.MaximumZ = Cloud.MaximumZ;
		Result.MaximumDistance = Cloud.MaximumDistance;
		Result.BaseFrequency = Cloud.BaseFrequency;
		Result.DetailFrequency = Cloud.DetailFrequency;
		Result.WindOffset = Cloud.WindOffset;
		Result.WeatherFrequency = Cloud.WeatherFrequency;
		Result.WeatherOffset = Cloud.WeatherOffset;
		Result.Coverage = Cloud.Coverage;
		Result.DetailErosion = Cloud.DetailErosion;
		Result.Extinction = Cloud.Extinction;
		Result.LightExtinction = Cloud.LightExtinction;
		Result.Ambient = Cloud.Ambient;
		Result.LightColor = FVector3f(0.0f);
		Result.AmbientColor = FVector3f(0.0f);
		if (!Lights.Directional.empty())
		{
			const FDirectionalLightSceneData& Light = Lights.Directional.front().Data;
			Result.LightDirection = -FVector3f(Light.Direction);
			Result.LightColor = Light.Color * std::max(0.0f, Light.Intensity);
			Result.AmbientColor = Light.Color
								  * std::max(0.0f, Light.AmbientIntensity);
		}
		return Result;
	}

	auto CalculateVolumetricCloudLightingKey(
		const FPreparedLightView& Lights
	) -> uint64
	{
		if (Lights.Directional.empty()) return 0;
		const FPreparedDirectionalLight& Prepared = Lights.Directional.front();
		const FDirectionalLightSceneData& Light = Prepared.Data;
		uint64 Hash = 1469598103934665603ull;
		auto Mix = [&Hash](uint64 Value) {
			Hash ^= Value;
			Hash *= 1099511628211ull;
		};
		Mix(Prepared.Id.Value);
		for (double Value : {Light.Direction.x, Light.Direction.y, Light.Direction.z})
			Mix(std::bit_cast<uint64>(Value));
		for (float Value : {Light.Color.x, Light.Color.y, Light.Color.z, Light.Intensity, Light.AmbientIntensity})
			Mix(std::bit_cast<uint32>(Value));
		return Hash;
	}
} // namespace Durin
