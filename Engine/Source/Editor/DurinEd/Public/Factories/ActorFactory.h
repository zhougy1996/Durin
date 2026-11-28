#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Factories/AssetFactoryInterface.h"

namespace Durin
{
	class DClass;

	// Spawns and configures one actor using the caller's transaction, if active.
	class DURINED_API FActorFactory : public IAssetFactoryInterface
	{
	public:
		FActorFactory(DClass* InAssetClass, DClass* InActorClass);
		auto CanPlaceElementsFromAssetData(const FAssetData& AssetData) const -> bool override;
		auto PlaceAsset(DEditorActorSubsystem& Editor, const FAssetPlacementInfo& Info,
			DObject& Asset, std::string* OutError) const -> std::vector<TWeakObjectPtr<AActor>> override;
	protected:
		// Runs before the actor's components are registered and its history is captured.
		virtual auto ConfigureActor(DObject& Asset, AActor& Actor) const -> void = 0;
	private:
		DClass* AssetClass;
		DClass* ActorClass;
	};
}
