#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/Catalog.h"
#include "Math/Transform.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class AActor;
	class DEditorActorSubsystem;
	class DLevel;
	class DObject;
	struct FActorEditContext;

	// The caller resolves the placement transform from its viewport or other input.
	struct FAssetPlacementInfo
	{
		FSoftObjectPath AssetToPlace;
		DLevel& PreferredLevel;
		FTransform FinalizedTransform{};
	};

	// Module-owned factories register with RegisterModularFeature for their lifetime.
	class IAssetFactoryInterface : public IModularFeature
	{
	public:
		static constexpr const char* FeatureName = "AssetPlacementFactory";
		virtual auto CanPlaceElementsFromAssetData(const FAssetData& AssetData) const -> bool = 0;
		virtual auto PlaceAsset(DEditorActorSubsystem& Editor, const FAssetPlacementInfo& Info,
			DObject& Asset, const FActorEditContext& Context) const -> std::expected<AActor*, std::string> = 0;
	};
}
