#include "Engine/VolumetricCloudSceneProxy.h"

#include <cmath>

namespace Durin
{
	namespace
	{
		template<typename TVector>
		auto IsFiniteVector(const TVector& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		template<>
		auto IsFiniteVector(const FVector3f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z);
		}
	}

	auto AreVolumetricCloudParametersValid(
		const FVolumetricCloudSceneData& Data) -> bool
	{
		return std::isfinite(Data.MinimumZ) && std::isfinite(Data.MaximumZ)
			&& std::isfinite(Data.MaximumDistance)
			&& Data.MinimumZ < Data.MaximumZ && Data.MaximumDistance > 0.0
			&& IsFiniteVector(Data.BaseFrequency)
			&& IsFiniteVector(Data.DetailFrequency)
			&& IsFiniteVector(Data.WindOffset)
			&& IsFiniteVector(Data.WeatherFrequency)
			&& IsFiniteVector(Data.WeatherOffset)
			&& std::isfinite(Data.Coverage) && Data.Coverage >= 0.0f
			&& Data.Coverage <= 1.0f
			&& std::isfinite(Data.DetailErosion) && Data.DetailErosion >= 0.0f
			&& Data.DetailErosion <= 1.0f
			&& std::isfinite(Data.Extinction) && Data.Extinction > 0.0f
			&& Data.Extinction <= 1.0f
			&& std::isfinite(Data.LightExtinction)
			&& Data.LightExtinction >= 0.0f && Data.LightExtinction <= 1.0f
			&& std::isfinite(Data.Ambient) && Data.Ambient >= 0.0f
			&& Data.Ambient <= 1.0f;
	}

	auto IsVolumetricCloudCandidateEligible(
		const FVolumetricCloudSceneData& Data) -> bool
	{
		return Data.bEnabled && Data.PersistentId.IsValid()
			&& Data.InstanceId != 0 && Data.PublicationRevision != 0
			&& Data.BaseDensityTexture != nullptr
			&& Data.DetailDensityTexture != nullptr
			&& AreVolumetricCloudParametersValid(Data);
	}
}
