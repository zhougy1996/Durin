#pragma once

#include "CoreMinimal.h"
#include "Actors/ActorEditContext.h"
#include "AssetRegistry/Catalog.h"
#include "Math/Transform.h"
#include "DObject/WeakObjectPtr.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class AActor;
	class DEditorActorSubsystem;
	class DLevel;
	class DObject;

	// The caller resolves the placement transform from its viewport or other input.
	struct FAssetPlacementInfo
	{
		FSoftObjectPath AssetToPlace;
		TWeakObjectPtr<DLevel> PreferredLevel;
		FActorEditContext EditContext;
		FTransform FinalizedTransform{};
	};

	// Module-owned factories register with RegisterModularFeature for their lifetime.
	class IAssetFactoryInterface : public IModularFeature
	{
	public:
		static constexpr const char* FeatureName = "AssetPlacementFactory";
		virtual auto CanPlaceElementsFromAssetData(const FAssetData& AssetData) const -> bool = 0;
		virtual auto PlaceAsset(DEditorActorSubsystem& Editor, const FAssetPlacementInfo& Info,
			DObject& Asset, std::string* OutError) const -> std::vector<TWeakObjectPtr<AActor>> = 0;
	};
}
