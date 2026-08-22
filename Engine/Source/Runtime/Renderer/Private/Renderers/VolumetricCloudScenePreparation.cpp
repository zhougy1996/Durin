#include "Renderers/VolumetricCloudScenePreparation.h"

#include <algorithm>

namespace Durin
{
	auto BuildVolumetricCloudParameters(
		const FVolumetricCloudSceneData& Cloud,
		const FPreparedLightView& Lights)
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
		if (!Lights.Directional.empty())
		{
			const FDirectionalLightSceneData& Light = Lights.Directional.front().Data;
			Result.LightDirection = -FVector3f(Light.Direction);
			Result.LightColor = Light.Color * std::max(0.0f, Light.Intensity);
		}
		return Result;
	}
}
