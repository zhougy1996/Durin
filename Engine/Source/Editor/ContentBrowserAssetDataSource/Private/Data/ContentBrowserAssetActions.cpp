#include "Data/ContentBrowserAssetActions.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/ContentFilesystem.h"
#include "AssetRegistry/ContentChangeScopes.h"

#include "Asset/PackageSerialization.h"
#include "AssetTools/AssetDeleteExtensions.h"
#include "Asset/Redirector.h"

#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/Publication.h"
#include "AssetTools/IAssetTools.h"
#include "ObjectTools.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"

namespace Durin::ContentBrowser::AssetDataSource::Private
{
	namespace
	{
		using ContentStorage::Filesystem::NormalizePath;

		auto SetQueryError(std::string* OutError, std::string_view Message) -> bool
		{ if (OutError) *OutError = Message; return false; }
		auto SetNewItem(FContentBrowserNativeItem& Item, std::string Physical, std::string Virtual = {}) -> void
		{
			Item.PhysicalPath = std::move(Physical);
			Item.VirtualPath = std::move(Virtual);
			Item.Name = Item.Kind == EContentBrowserItemKind::Asset || Item.Kind == EContentBrowserItemKind::Redirector
				? std::string(Item.PackagePath.GetPackageName()) : std::filesystem::path(Item.PhysicalPath).filename().generic_string();
			Item.Extension = std::filesystem::path(Item.PhysicalPath).extension().generic_string();
		}

	} // namespace

	auto FContentBrowserAssetActions::DuplicateItem(
		const FContentBrowserNativeItem& Item, FContentBrowserNativeItem& OutNewItem) -> bool
	{
		OutNewItem = {};
		std::string AdmissionError;
		if (!ContentStorage::CanModifyContent(Policy, &AdmissionError)) return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(AssetWriteErrors::ReadOnlyMode, AdmissionError));
		if (Item.Kind != EContentBrowserItemKind::Asset)
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(
				AssetWriteErrors::InvalidData,
				"Only real assets can be duplicated."));
		FTopLevelAssetPath SourcePath;
		if (!FTopLevelAssetPath::TryCreate(Item.VirtualPath, SourcePath))
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(AssetWriteErrors::InvalidPath,
				"The source top-level asset path is invalid."));
		if (!SourcePath.IsValid())
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(
				AssetWriteErrors::InvalidPath,
				"The source asset path is invalid."));
		const std::string SourcePackagePath = SourcePath.GetPackagePath().ToString();
		const size_t Slash = SourcePackagePath.find_last_of('/');
		if (Slash == std::string::npos)
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(
				AssetWriteErrors::InvalidPath,
				"The source asset has no valid destination directory."));
		return DuplicateItem(Item, ContentStorage::FContentMountPaths::VirtualToPhysical(SourcePackagePath.substr(0, Slash + 1)), OutNewItem);
	}
	auto FContentBrowserAssetActions::DuplicateItem(
		const FContentBrowserNativeItem& Item,
		std::string_view PhysicalDirectory, FContentBrowserNativeItem& OutNewItem)
		-> bool
	{
		OutNewItem = {};
		std::string AdmissionError;
		if (!ContentStorage::CanModifyContent(Policy, &AdmissionError)) return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(AssetWriteErrors::ReadOnlyMode, AdmissionError));
		FTopLevelAssetPath SourcePath;
		if (Item.Kind != EContentBrowserItemKind::Asset || !FTopLevelAssetPath::TryCreate(Item.VirtualPath, SourcePath) || PhysicalDirectory.empty())
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(
				AssetWriteErrors::InvalidPath,
				"Asset paste requires a valid source and destination folder."));
		std::string Directory = ContentStorage::FContentMountPaths::PhysicalToVirtualDirectory(PhysicalDirectory);
		if (!Directory.ends_with('/')) Directory.push_back('/');
		std::string Error;
		if (!CanDuplicateAsset(SourcePath, &Error)) return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError{AssetWriteErrors::InvalidData, Error});
		const auto SourceData = FindAssetExact(SourcePath);
		const std::string DestinationDirectoryPhysical =
			ContentStorage::FContentMountPaths::VirtualToPhysical(Directory);
		const ContentStorage::FContentMountPaths::FMountPath Mount =
			ContentStorage::FContentMountPaths::ResolveMountPath(DestinationDirectoryPhysical);
		if (!Mount)
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(
				AssetWriteErrors::InvalidPath,
				"The paste destination is outside an automatically scanned content mount."));
		if (!Mount.Mount->bContentWritable)
			return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError(
				AssetWriteErrors::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before pasting the asset."));

		auto Context = ContentStorage::MakeContentMutationFeedback(Policy);
		FScopedFeedbackContext FeedbackScope(Context);
		std::optional<UnifiedError::FError> LoadError;
		auto* Source = LoadObject(SourcePath, nullptr, nullptr, &LoadError);
		if (!Source) return ContentStorage::ReportContentMutationFailure(Policy, LoadError.value_or(UnifiedError::FError{AssetWriteErrors::NotFound, "The duplicate source is unavailable."}));
		const TStrongObjectPtr<DObject> SourceOwner(Source);
		if (!ContentStorage::CanModifyContent(Policy, &Error)) return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError{AssetWriteErrors::ReadOnlyMode, Error});
		std::vector<DObject*> Copies;
		ObjectTools::DuplicateObjects(std::span{&Source, 1}, {}, Directory, false, &Copies);
		auto* Copy = Copies.empty() ? nullptr : Copies.front();
		if (!Copy) return false;
		OutNewItem.Kind = EContentBrowserItemKind::Asset;
		OutNewItem.PackagePath = Copy->GetPackage()->GetPackagePathIdentity();
		OutNewItem.AssetClassName = SourceData->AssetClassName;
		const auto Resolved = FMountPaths::ResolveAssetPath(OutNewItem.PackagePath.GetView());
		SetNewItem(OutNewItem, Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{}, Copy->GetObjectPath());
		ContentStorage::NotifyContentMutation(Policy, {.Changes = {{EContentChangeKind::Added, {}, OutNewItem.PhysicalPath, {}, OutNewItem.VirtualPath}}});
		return true;
	}
	auto FContentBrowserAssetActions::CollectRedirectors(std::string_view Directory) const -> std::vector<FTopLevelAssetPath>
	{
		std::string Prefix(Directory); if (!Prefix.empty() && !Prefix.ends_with('/')) Prefix += '/';
		std::vector<FTopLevelAssetPath> Result;
		for (const auto& [Path, Asset] : CaptureAssetQuerySnapshot().Assets)
			if (Asset.IsRedirector() && (Prefix.empty() || Path.GetPackagePath().GetView().starts_with(Prefix))) Result.push_back(Path);
		std::ranges::sort(Result);
		return Result;
	}
	auto FContentBrowserAssetActions::FixUpRedirectorsInFolder(std::string_view Directory) -> bool
	{
		if (Directory.empty()) return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError{AssetWriteErrors::InvalidPath, "Fix Up in Folder requires a mounted virtual directory."});
		return FixUpRedirectors(CollectRedirectors(Directory));
	}
	auto FContentBrowserAssetActions::FixUpRedirectors(std::span<const FTopLevelAssetPath> Paths) -> bool
	{
		std::string AdmissionError;
		if (!ContentStorage::CanModifyContent(Policy, &AdmissionError)) return ContentStorage::ReportContentMutationFailure(Policy, UnifiedError::FError{AssetWriteErrors::ReadOnlyMode, AdmissionError});
		if (Paths.empty()) return true;
		auto Context = ContentStorage::MakeContentMutationFeedback(Policy); FScopedFeedbackContext Scope(Context);
		std::vector<DAssetRedirector*> Objects;
		std::vector<FStrongObjectPtr> Owners;
		for (const auto& Path : Paths)
			if (auto* Package = LoadPackage(Path.GetPackagePath()))
				if (auto* Alias = Cast<DAssetRedirector>(Package->FindTopLevelAsset(FName(Path.GetAssetName()))))
				{ Objects.push_back(Alias); Owners.emplace_back(Alias); }
		if (!ContentStorage::CanModifyContent(Policy, &AdmissionError)) return false;
		// The synchronous backend retains selection itself, then releases it for checked retirement.
		Owners.clear();
		const auto Revision = GetAssetCatalogRevision();
		const auto Fences = CaptureAssetRegistryProjectionFences();
		IAssetTools::Get().FixupReferencers(Objects);
		auto Changes = CaptureAssetCatalogChanges(Revision);
		// Failed publication can leave committed files outside the catalog projection.
		// Keep the conservative fallback for that uncertainty, not for every fix-up.
		for (const auto& Path : CaptureAssetRegistryProjectionFences())
			if (std::ranges::find(Fences, Path) == Fences.end()) { Changes.bFullRefresh = true; break; }
		ContentStorage::NotifyContentMutation(Policy, std::move(Changes));
		return true;
	}
	auto FContentBrowserAssetActions::FixUpAllRedirectors() -> bool { return FixUpRedirectors(CollectRedirectors("/")); }
	auto FContentBrowserAssetActions::CanDuplicateAsset(const FTopLevelAssetPath& Source, std::string* OutErrorMsg) const -> bool
	{
		if (!ContentStorage::CanModifyContent(Policy, OutErrorMsg)) return false;
		const auto Entry = FindAssetExact(Source);
		return Entry && !Entry->IsRedirector() ? true : SetQueryError(OutErrorMsg, "The copied source is no longer an available real asset.");
	}
	auto FContentBrowserAssetActions::CanDuplicateItem(const FContentBrowserNativeItem& Item, std::string* OutErrorMsg) const -> bool
	{
		FTopLevelAssetPath Source;
		if (Item.Kind != EContentBrowserItemKind::Asset || !FTopLevelAssetPath::TryCreate(Item.VirtualPath, Source))
			return SetQueryError(OutErrorMsg, "Duplication requires a real asset item.");
		return CanDuplicateAsset(Source, OutErrorMsg);
	}

}
