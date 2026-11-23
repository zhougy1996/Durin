#include "WaterWavesAssetDefinition.h"

#include "DObject/Class.h"
#include "WaterWaves.h"

namespace Durin
{
	DWaterWavesAssetDefinition::DWaterWavesAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DWaterWavesAssetDefinition::GetAssetClassPath() const -> std::string
	{
		return DWaterWaves::StaticClass()->GetQualifiedName().ToString();
	}
}
