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
		// First matching registration wins. The factory is borrowed for the given scope.
		DURINED_API auto FindAssetFactoryFromAssetData(const FAssetData& AssetData,
			const FScopedModularFeatureAccess& Access) const -> const IAssetFactoryInterface*;
		DURINED_API auto PlaceAsset(const FAssetPlacementInfo& Info, const FActorEditContext& Context) const
			-> std::expected<AActor*, std::string>;
	};
}
