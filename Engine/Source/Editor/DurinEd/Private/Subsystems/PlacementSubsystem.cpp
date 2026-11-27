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

	namespace
	{
		auto PlaceOne(const DPlacementSubsystem& Placement, DEditorEngine& Host, const FAssetPlacementInfo& Info,
			std::optional<FScopedTransaction>& Transaction, std::string* OutError) -> std::vector<TWeakObjectPtr<AActor>>
		{
			auto Reject = [&](const char* Message) -> std::vector<TWeakObjectPtr<AActor>> {
				if (OutError) *OutError = Message;
				return {};
			};
			if (Info.EditContext.bReadOnly) return Reject("The level is read-only.");
			const auto IsCurrentLevel = [&] {
				auto* Level = Info.PreferredLevel.Get();
				return Level && Host.GetEditorWorld() && Host.GetEditorWorld()->GetCurrentLevel() == Level;
			};
			if (!IsCurrentLevel()) return Reject("The target is not the current editor level.");

			// Reject catalogued assets without loading their packages or import data.
			const auto Resolution = ResolveAssetObjectPath(Info.AssetToPlace);
			if (Resolution.FinalPath.IsTopLevelAsset() && Resolution.FinalAssetData)
			{
				FScopedModularFeatureAccess Access;
				if (!Placement.FindAssetFactoryFromAssetData(*Resolution.FinalAssetData, Access))
					return {};
			}

			FAssetLoadReport Report;
			std::optional<UnifiedError::FError> Error;
			TStrongObjectPtr<DObject> Asset(LoadObject<DObject>(Info.AssetToPlace, &Report, OutError ? &Error : nullptr));
			if (!Asset)
			{
				if (OutError && Error) *OutError = Error->ToString();
				return {};
			}
			// Loading may run editor callbacks. Dispatch against the current registrations
			// and actual object class, including for resident assets absent from the catalog.
			FAssetData Data;
			if (Resolution.FinalAssetData && Resolution.FinalPath == Report.ResolvedObjectPath)
				Data = *Resolution.FinalAssetData;
			Data.AssetPath = Report.ResolvedObjectPath.GetAssetPath();
			Data.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString();
			FScopedModularFeatureAccess Access;
			const auto* Factory = Placement.FindAssetFactoryFromAssetData(Data, Access);
			if (!Factory) return {};
			if (!IsCurrentLevel() || Host.IsShutdownRequested() || Host.GetEditorSubsystem<DPlacementSubsystem>() != &Placement)
				return Reject("The placement target changed while loading the asset.");
			auto* Actors = Host.GetEditorSubsystem<DEditorActorSubsystem>();
			if (!Actors) return Reject("The actor editor subsystem is unavailable.");
			if (!Transaction) Transaction.emplace("Place assets");
			if (!Transaction->IsRecordingCurrent()) return Reject("Editor history is unavailable for placement.");
			return Factory->PlaceAsset(*Actors, Info, *Asset, OutError);
		}
	}

	auto DPlacementSubsystem::PlaceAsset(const FAssetPlacementInfo& Info, std::string* OutError) const
		-> std::vector<TWeakObjectPtr<AActor>>
	{
		return PlaceAssets(std::span(&Info, 1), OutError);
	}

	auto DPlacementSubsystem::PlaceAssets(std::span<const FAssetPlacementInfo> Infos, std::string* OutError) const
		-> std::vector<TWeakObjectPtr<AActor>>
	{
		if (OutError) OutError->clear();
		if (!IsInGameThread())
		{
			if (OutError) *OutError = "Asset placement requires the game thread.";
			return {};
		}
		auto* Host = GetEditor();
		if (!Host || Host->GetEditorSubsystem<DPlacementSubsystem>() != this || Host->IsShutdownRequested())
		{
			if (OutError) *OutError = "The placement subsystem is unavailable.";
			return {};
		}
		std::vector<TWeakObjectPtr<AActor>> Placed;
		std::optional<FScopedTransaction> Transaction;
		for (const auto& Info : Infos)
		{
			std::string Error;
			Placed.append_range(PlaceOne(*this, *Host, Info, Transaction, OutError ? &Error : nullptr));
			if (!Error.empty())
			{
				if (!OutError->empty()) OutError->append("\n");
				OutError->append(std::format("{}: {}", Info.AssetToPlace.ToString(), Error));
			}
			if (Host->IsShutdownRequested() || (Transaction && !Transaction->IsRecordingCurrent())) break;
		}
		return Placed;
	}
}
