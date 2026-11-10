#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/Object.h"
#include "PrimaryDataAsset.gen.h"

namespace Durin
{
	// Native subclasses supply bundle metadata through UpdateAssetBundleData.
	DCLASS()
	class DPrimaryDataAsset : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DPrimaryDataAsset(const FObjectInitializer& Initializer);
		ENGINE_API auto GetPrimaryAssetId() const -> FPrimaryAssetId override;
		ENGINE_API auto GetAssetBundleData() const -> FAssetBundleData override;
		ENGINE_API virtual auto UpdateAssetBundleData(FAssetBundleData& Data) const -> void;
	};
}
