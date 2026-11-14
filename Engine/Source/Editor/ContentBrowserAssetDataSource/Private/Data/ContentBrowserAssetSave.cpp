#include "Data/ContentBrowserAssetSave.h"
#include "Misc/ContentMountPaths.h"
#include "AssetTools/IAssetTools.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "Misc/MountPaths.h"

namespace Durin::ContentBrowser::AssetDataSource::Private
{
	namespace
	{
		auto SetQueryError(std::string* Error, std::string_view Message) -> bool
		{ if (Error) *Error = Message; return false; }
	}
	auto FContentBrowserAssetSaveOperation::CanSaveItem(const FContentBrowserNativeItem& Item, EAssetSaveMode Mode, std::string* OutErrorMsg) const -> bool
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
		if (Mode == EAssetSaveMode::CanonicalResave)
		{
			const auto Package = FindPackageExact(Item.PackagePath);
			return Package && Mount->Mount->bAutoScan ? true
				: SetQueryError(OutErrorMsg, "Canonical resave requires a saved package in a scanned content mount.");
		}
		const auto* Package = FindResidentPackage(Item.PackagePath);
		return Package && Package->IsDirty() ? true : SetQueryError(OutErrorMsg, "Save requires a resident dirty package.");
	}
	auto FContentBrowserAssetSaveOperation::BulkSaveItems(std::span<const FContentBrowserNativeItem> Items, EAssetSaveMode Mode) -> bool
	{
		bool Success = false;
		auto Report = [&](ELogLevel Level, std::string_view Message) {
			if (Host.ReportDiagnostic) Host.ReportDiagnostic(Level, Message);
			else FFeedbackContext::Get().Serialize(Level, Message);
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
			if (!CanSaveItem(Item, Mode, &Error))
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
			const bool Saved = IAssetTools::Get().SaveAssets({.AssetPaths = {Item.PackagePath}, .Mode = Mode,
				.OnSaved = [&](const FPackagePath& Path) {
					Committed = true;
					const auto Physical = ContentStorage::FContentMountPaths::VirtualToPhysical(Path.ToString() + ".dasset");
					Changes.Changes.push_back({EContentChangeKind::Modified, Physical, Physical, Path.ToString(), Path.ToString()});
				}}, &Feedback);
			Success |= Committed;
			if (!Saved || !Committed)
			{
				if (!ReportedFailure) Report(ELogLevel::Error, "Package save did not complete: " + Item.PackagePath.ToString());
				// The bool native service cannot qualify recovery after a failed write.
				// Preserve committed files; stop later packages when effects are unknown.
				Stopped = !Committed;
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
