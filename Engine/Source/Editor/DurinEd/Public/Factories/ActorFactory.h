#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Factories/AssetFactoryInterface.h"

namespace Durin
{
	class DClass;

	// Spawns and configures one actor within the editor's structural transaction.
	class DURINED_API FActorFactory : public IAssetFactoryInterface
	{
	public:
		FActorFactory(DClass* InAssetClass, DClass* InActorClass);
		auto CanPlaceElementsFromAssetData(const FAssetData& AssetData) const -> bool override;
		auto PlaceAsset(DEditorActorSubsystem& Editor, const FAssetPlacementInfo& Info,
			DObject& Asset, const FActorEditContext& Context) const -> std::expected<AActor*, std::string> override;
	protected:
		// Runs before the actor's components are registered and its history is captured.
		virtual auto ConfigureActor(DObject& Asset, AActor& Actor) const -> void = 0;
	private:
		DClass* AssetClass;
		DClass* ActorClass;
	};
}
