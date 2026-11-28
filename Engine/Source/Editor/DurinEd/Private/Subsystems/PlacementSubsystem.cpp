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

	namespace
	{
		auto FindFactory(const FAssetData& Data, const FScopedModularFeatureAccess&) -> const IAssetFactoryInterface*
		{
			for (auto* Factory : IModularFeatures::Get().GetModularFeatureImplementations<IAssetFactoryInterface>(
				FName(IAssetFactoryInterface::FeatureName)))
				if (Factory->CanPlaceElementsFromAssetData(Data)) return Factory;
			return nullptr;
		}

		auto PlaceOne(const DPlacementSubsystem& Placement, DEditorEngine& Host, FAssetPlacementInfo Info,
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

			auto& Data = Info.AssetToPlace;
			if (Data.IsRedirector())
			{
				auto Target = ResolveAssetObjectPath(FSoftObjectPath(Data.AssetPath));
				if (Target && Target.FinalAssetData) Data = std::move(*Target.FinalAssetData);
			}
			// A broken redirect or an incomplete drag payload still receives the loader's diagnostic.
			if (!Data.IsRedirector() && !Data.AssetClassName.empty())
			{
				FScopedModularFeatureAccess Access;
				if (!FindFactory(Data, Access)) return {};
			}

			FAssetLoadReport Report;
			std::optional<UnifiedError::FError> Error;
			TStrongObjectPtr<DObject> Asset(LoadObject<DObject>(Data.AssetPath, &Report, OutError ? &Error : nullptr));
			if (!Asset)
			{
				if (OutError && Error) *OutError = Error->ToString();
				return {};
			}
			// A rename or reimport since the drag may have changed the path or class.
			const auto& LoadedPath = Report.ResolvedObjectPath.GetAssetPath();
			const auto LoadedClass = Asset->GetClass()->GetQualifiedName().ToString();
			if (Data.AssetPath != LoadedPath || Data.AssetClassName != LoadedClass)
			{
				Data = FindAssetExact(LoadedPath).Asset.value_or(FAssetData{});
				Data.AssetPath = LoadedPath;
				Data.AssetClassName = LoadedClass;
			}
			// Reacquire the factory after load callbacks have finished.
			FScopedModularFeatureAccess Access;
			const auto* Factory = FindFactory(Data, Access);
			if (!Factory) return {};
			if (!IsCurrentLevel() || Host.IsShutdownRequested() || Host.GetEditorSubsystem<DPlacementSubsystem>() != &Placement)
				return Reject("The placement target changed while loading the asset.");
			auto* Actors = Host.GetEditorSubsystem<DEditorActorSubsystem>();
			if (!Actors) return Reject("The actor editor subsystem is unavailable.");
			if (!Transaction && !Transaction.emplace("Place assets").IsOutstanding())
				return Reject("Editor history is unavailable for placement.");
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
			if (OutError && !Error.empty())
			{
				if (!OutError->empty()) OutError->append("\n");
				OutError->append(std::format("{}: {}", Info.AssetToPlace.AssetPath.ToString(), Error));
			}
			if (Host->IsShutdownRequested() || (Transaction && !Transaction->IsOutstanding())) break;
		}
		return Placed;
	}
}
