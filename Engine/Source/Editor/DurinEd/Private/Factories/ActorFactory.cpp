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
		DObject& Asset, std::string* OutError) const -> std::vector<TWeakObjectPtr<AActor>>
	{
		auto* Level = Info.PreferredLevel.Get();
		if (!Level || !Asset.IsA(AssetClass)) return {};
		auto* Actor = Editor.SpawnActor(*Level, ActorClass, FName(Info.AssetToPlace.GetAssetPath().GetAssetName()),
			Info.FinalizedTransform, Info.EditContext, [&](AActor& Created) { ConfigureActor(Asset, Created); },
			ESpawnActorNameMode::Requested, {}, OutError);
		if (!Actor) return {};
		return {Actor};
	}
}
