#pragma once

#include "CoreMinimal.h"
#include "AssetDefinition/AssetDefinition.h"
#include "WaterEditorAPI.h"
#include "WaterWavesAssetDefinition.gen.h"

namespace Durin
{
	DCLASS()
	class DWaterWavesAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DWaterWavesAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
	};
}
