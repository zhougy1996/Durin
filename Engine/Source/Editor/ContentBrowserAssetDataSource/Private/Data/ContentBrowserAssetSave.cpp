#include "Data/ContentBrowserAssetSave.h"
#include "Misc/ContentMountPaths.h"
#include "FileHelpers.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/MountPaths.h"

namespace Durin::ContentBrowser::AssetDataSource::Private
{
	namespace
	{
		auto SetQueryError(std::string* Error, std::string_view Message) -> bool
		{ if (Error) *Error = Message; return false; }
	}
	auto FContentBrowserAssetSaveOperation::CanSaveItem(const FContentBrowserNativeItem& Item, bool IncludeClean, std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		if (Host.CanMutateContent && !Host.CanMutateContent()) return SetQueryError(OutErrorMsg, "Content mutation is currently disabled.");
		if (Item.Kind != EContentBrowserItemKind::Asset || !Item.PackagePath.IsValid())
			return SetQueryError(OutErrorMsg, "Save requires an asset item.");
		const auto Mount = FMountPaths::FindMountForVirtualPath(Item.PackagePath.GetView());
		if (!Mount || !Mount->Mount->bContentWritable)
			return SetQueryError(OutErrorMsg, "Save requires a writable content mount.");
		if (IsPackageLoading(Item.PackagePath))
			return SetQueryError(OutErrorMsg, "An incomplete package cannot be saved.");
		if (IncludeClean)
		{
			return FindResidentPackage(Item.PackagePath) || FindPackageExact(Item.PackagePath) ? true
				: SetQueryError(OutErrorMsg, "Save requires an available package.");
		}
		const auto* Package = FindResidentPackage(Item.PackagePath);
		return Package && Package->IsDirty() ? true : SetQueryError(OutErrorMsg, "Save requires a resident dirty package.");
	}
	auto FContentBrowserAssetSaveOperation::BulkSaveItems(std::span<const FContentBrowserNativeItem> Items, bool IncludeClean) -> bool
	{
		bool Success = false;
		auto& FallbackFeedback = FFeedbackContext::Get();
		auto Report = [&](ELogLevel Level, std::string_view Message) {
			if (Host.ReportDiagnostic) Host.ReportDiagnostic(Level, Message);
			else FallbackFeedback.Serialize(Level, Message);
		};
		std::vector<FContentBrowserNativeItem> Packages;
		for (const auto& Item : Items)
		{
			if (Item.Kind != EContentBrowserItemKind::Asset || !Item.PackagePath.IsValid())
			{
				Report(ELogLevel::Error, "Save requires asset items.");
				return false;
			}
			if (std::ranges::none_of(Packages, [&](const auto& Entry) { return Entry.PackagePath == Item.PackagePath; })) Packages.push_back(Item);
		}
		std::ranges::sort(Packages, {}, [](const auto& Item) { return Item.PackagePath.ToString(); });
		FContentChangeBatch Changes;
		bool Stopped = false;
		for (const auto& Item : Packages)
		{
			if (Stopped) break;
			if (Host.CanMutateContent && !Host.CanMutateContent())
			{
				Report(ELogLevel::Error, "Save stopped because mutation admission was lost.");
				break;
			}
			std::string Error;
			if (!CanSaveItem(Item, IncludeClean, &Error))
			{
				// Policy callbacks may withdraw admission while capabilities are queried.
				Stopped = Host.CanMutateContent && !Host.CanMutateContent();
				Report(ELogLevel::Error, Item.PackagePath.ToString() + ": " + Error);
				continue;
			}
			bool Committed = false;
			bool ReportedFailure = false;
			FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
				ReportedFailure |= Level >= ELogLevel::Error;
				Report(Level, Message);
			});
			FScopedFeedbackContext Scope(Feedback);
			const auto OnSaved = [&](const FPackagePath& Path) {
				Committed = true;
				const auto Physical = ContentStorage::FContentMountPaths::VirtualToPhysical(Path.ToString() + ".dasset");
				Changes.Changes.push_back({EContentChangeKind::Modified, Physical, Physical, Path.ToString(), Path.ToString()});
			};
			FScopedPackageSaveObserver Observer(OnSaved);
			auto* Package = FindResidentPackage(Item.PackagePath);
			std::optional<UnifiedError::FError> LoadError;
			if (!Package && IncludeClean) Package = LoadPackage(Item.PackagePath, nullptr, &LoadError);
			if (!Package && LoadError) Feedback.Error(LoadError->ToString());
			std::vector<FStrongObjectPtr> Owners;
			if (Package)
			{
				Owners.emplace_back(Package);
				for (auto* Asset : Package->GetTopLevelAssets()) Owners.emplace_back(Asset);
			}
			if (Host.CanMutateContent && !Host.CanMutateContent()) { Report(ELogLevel::Error, "Save admission was withdrawn during loading."); break; }
			const bool Saved = Package && DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, !IncludeClean);

			Success |= Committed;
			if (!Saved || !Committed)
			{
				if (!ReportedFailure) Report(ELogLevel::Error, "Package save did not complete: " + Item.PackagePath.ToString());
				// The bool native service cannot qualify recovery after a failed write.
				// Preserve committed files; stop later packages when effects are unknown.
				Stopped = Package && !Committed;
				Changes.bFullRefresh = true;
			}
		}
		if (Changes.bFullRefresh || !Changes.Changes.empty())
		{
			if (Host.NotifyContentMutation) Host.NotifyContentMutation(std::move(Changes));
		}
		return Success;
	}

}
