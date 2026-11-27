#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "Factories/AssetFactoryInterface.h"
#include "PlacementSubsystem.gen.h"

namespace Durin
{
	// Resolves editor asset placement requests against registered factories.
	DCLASS()
	class DPlacementSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DPlacementSubsystem(const FObjectInitializer& Initializer);
		// Results follow request order. Unsupported assets contribute no actors or diagnostic.
		DURINED_API auto PlaceAsset(const FAssetPlacementInfo& Info, std::string* OutError = nullptr) const
			-> std::vector<TWeakObjectPtr<AActor>>;
		DURINED_API auto PlaceAssets(std::span<const FAssetPlacementInfo> Infos, std::string* OutError = nullptr) const
			-> std::vector<TWeakObjectPtr<AActor>>;
	};
}
