#include "Subsystems/PlacementSubsystem.h"
#include "Actors/EditorActorSubsystem.h"
#include "Asset/Load.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DPlacementSubsystem::DPlacementSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DPlacementSubsystem::FindAssetFactoryFromAssetData(const FAssetData& AssetData,
		const FScopedModularFeatureAccess&) const -> const IAssetFactoryInterface*
	{
		for (auto* Factory : IModularFeatures::Get().GetModularFeatureImplementations<IAssetFactoryInterface>(
			FName(IAssetFactoryInterface::FeatureName)))
			if (Factory->CanPlaceElementsFromAssetData(AssetData)) return Factory;
		return nullptr;
	}

	auto DPlacementSubsystem::PlaceAsset(const FAssetPlacementInfo& Info, const FActorEditContext& Context) const
		-> std::expected<AActor*, std::string>
	{
		if (!IsInGameThread()) return std::unexpected("Asset placement requires the game thread.");
		if (Context.bReadOnly) return std::unexpected("The level is read-only.");
		auto* Host = GetEditor();
		if (!Host || Host->GetEditorSubsystem<DPlacementSubsystem>() != this || Host->IsShutdownRequested())
			return std::unexpected("The placement subsystem is unavailable.");
		auto* Actors = Host->GetEditorSubsystem<DEditorActorSubsystem>();
		if (!Actors) return std::unexpected("The actor editor subsystem is unavailable.");
		if (!Host->GetEditorWorld() || Host->GetEditorWorld()->GetCurrentLevel() != &Info.PreferredLevel)
			return std::unexpected("The target is not the current editor level.");

		// Reject catalogued assets without loading their packages or import data.
		const auto Resolution = ResolveAssetObjectPath(Info.AssetToPlace);
		if (Resolution.FinalPath.IsTopLevelAsset() && Resolution.FinalAssetData)
		{
			FScopedModularFeatureAccess Access;
			if (!FindAssetFactoryFromAssetData(*Resolution.FinalAssetData, Access))
				return std::unexpected(std::format("No placement factory supports {}.", Resolution.FinalAssetData->AssetClassName));
		}

		FAssetLoadReport Report;
		std::optional<UnifiedError::FError> Error;
		TStrongObjectPtr<DObject> Asset(LoadObject<DObject>(Info.AssetToPlace, &Report, &Error));
		if (!Asset) return std::unexpected(Error->ToString());
		// Loading may run editor callbacks. Dispatch against the current registrations
		// and actual object class, including for resident assets absent from the catalog.
		FAssetData Data;
		if (Resolution.FinalAssetData && Resolution.FinalPath == Report.ResolvedObjectPath)
			Data = *Resolution.FinalAssetData;
		Data.AssetPath = Report.ResolvedObjectPath.GetAssetPath();
		Data.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString();
		FScopedModularFeatureAccess Access;
		const auto* Factory = FindAssetFactoryFromAssetData(Data, Access);
		if (!Factory) return std::unexpected(std::format("No placement factory supports {}.", Data.AssetClassName));
		return Factory->PlaceAsset(*Actors, Info, *Asset, Context);
	}
}
