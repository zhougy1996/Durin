#include "Factories/ActorFactory.h"
#include "Actors/EditorActorSubsystem.h"
#include "DObject/Class.h"

namespace Durin
{
	FActorFactory::FActorFactory(DClass* InAssetClass, DClass* InActorClass)
		: AssetClass(InAssetClass), ActorClass(InActorClass) {}

	auto FActorFactory::CanPlaceElementsFromAssetData(const FAssetData& AssetData) const -> bool
	{
		const auto* Class = FindClassByQualifiedName(AssetData.AssetClassName);
		return Class && Class->IsChildOf(AssetClass);
	}

	auto FActorFactory::PlaceAsset(DEditorActorSubsystem& Editor, const FAssetPlacementInfo& Info,
		DObject& Asset, const FActorEditContext& Context) const -> std::expected<AActor*, std::string>
	{
		if (!Asset.IsA(AssetClass)) return std::unexpected("The asset is incompatible with the actor factory.");
		std::string Error;
		auto* Actor = Editor.SpawnActor(Info.PreferredLevel, ActorClass, FName(Info.AssetToPlace.GetAssetPath().GetAssetName()),
			Info.FinalizedTransform, Context, [&](AActor& Created) { ConfigureActor(Asset, Created); },
			ESpawnActorNameMode::Requested, {}, &Error);
		if (!Actor) return std::unexpected(std::move(Error));
		return Actor;
	}
}
