#include "Data/ContentBrowserAssetActions.h"
#include "Operations/ContentBrowserPathInspection.h"
#include "Operations/ContentBrowserPaths.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Panels/ContentBrowserChanges.h"

#include "Asset/PackageSerialization.h"
#include "AssetTools/AssetDeletion.h"
#include "AssetTools/Relocation.h"
#include "AssetTools/RedirectorFixup.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "AssetRegistry/Catalog.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"

namespace Durin::ContentBrowser::Private
{
	namespace
	{
		using ContentBrowserFilesystem::NormalizePath;

		auto Failure(EAssetWriteError Error, std::string Message) -> FAssetWriteResult
		{ return {Error, std::move(Message)}; }
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
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		if (Item.Kind != EContentBrowserItemKind::Asset)
			return Complete(Failure(
				EAssetWriteError::InvalidData,
				"Only real assets can be duplicated."));
		FTopLevelAssetPath SourcePath;
		if (!FTopLevelAssetPath::TryCreate(Item.VirtualPath, SourcePath))
			return Complete(Failure(EAssetWriteError::InvalidPath,
				"The source top-level asset path is invalid."));
		if (!SourcePath.IsValid())
			return Complete(Failure(
				EAssetWriteError::InvalidPath,
				"The source asset path is invalid."));
		const std::string SourcePackagePath = SourcePath.GetPackagePath().ToString();
		const size_t Slash = SourcePackagePath.find_last_of('/');
		if (Slash == std::string::npos)
			return Complete(Failure(
				EAssetWriteError::InvalidPath,
				"The source asset has no valid destination directory."));
		return DuplicateItem(Item, FContentBrowserPaths::VirtualToPhysical(SourcePackagePath.substr(0, Slash + 1)), OutNewItem);
	}
	auto FContentBrowserAssetActions::DuplicateItem(
		const FContentBrowserNativeItem& Item,
		std::string_view PhysicalDirectory, FContentBrowserNativeItem& OutNewItem)
		-> bool
	{
		OutNewItem = {};
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		FTopLevelAssetPath SourcePath;
		if (Item.Kind != EContentBrowserItemKind::Asset || !FTopLevelAssetPath::TryCreate(Item.VirtualPath, SourcePath) || PhysicalDirectory.empty())
			return Complete(Failure(
				EAssetWriteError::InvalidPath,
				"Asset paste requires a valid source and destination folder."));
		std::string Directory = FContentBrowserPaths::PhysicalToVirtualDirectory(PhysicalDirectory);
		if (!Directory.ends_with('/')) Directory.push_back('/');
		std::string Error;
		if (!CanDuplicateAsset(SourcePath, &Error)) return Complete({EAssetWriteError::InvalidData, Error});
		const auto SourceData = FindAssetExact(SourcePath);
		const std::string DestinationDirectoryPhysical =
			FContentBrowserPaths::VirtualToPhysical(Directory);
		const FContentBrowserPaths::FMountPath Mount =
			FContentBrowserPaths::ResolveMountPath(DestinationDirectoryPhysical);
		if (!Mount)
			return Complete(Failure(
				EAssetWriteError::InvalidPath,
				"The paste destination is outside an automatically scanned content mount."));
		if (!Mount.Mount->bContentWritable)
			return Complete(Failure(
				EAssetWriteError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before pasting the asset."));

		auto Context = Feedback();
		const auto Revision = GetAssetQueryRevision();
		auto* Copy = IAssetTools::Get().DuplicateAsset({.SourcePath = SourcePath, .DestinationDirectory = Directory}, &Context);
		if (!Copy) return false;
		OutNewItem.Kind = EContentBrowserItemKind::Asset;
		OutNewItem.PackagePath = Copy->GetPackage()->GetPackagePathIdentity();
		OutNewItem.AssetClassName = SourceData->AssetClassName;
		const auto Resolved = FMountPaths::ResolveAssetPath(OutNewItem.PackagePath.GetView());
		SetNewItem(OutNewItem, Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{}, Copy->GetObjectPath());
		const bool bSaved = IAssetTools::Get().SaveAssets({.AssetPaths = {OutNewItem.PackagePath}}, &Context);
		if (!bSaved) Report(ELogLevel::Warn, "The duplicated asset remains available. Retry saving its package.");
		NotifyItemsUpdated(CaptureAssetQueryChanges(Revision));
		return bSaved;
	}
	auto FContentBrowserAssetActions::CollectRedirectors(
		std::string_view VirtualDirectory) const -> std::vector<FPackagePath>
	{
		std::string Prefix(VirtualDirectory);
		if (!Prefix.empty() && !Prefix.ends_with('/')) Prefix += '/';
		std::vector<FPackagePath> Redirectors;
		for (const auto& [Path, Data]
			: CaptureAssetCatalogSnapshot().Assets)
		{
			if (!(!Data.TopLevelAssets.empty() && std::ranges::all_of(Data.TopLevelAssets, &FAssetData::IsRedirector)))
				continue;
			if (!Prefix.empty() && !Path.GetView().starts_with(Prefix)) continue;
			Redirectors.push_back(Path);
		}
		std::ranges::sort(
			Redirectors,
			[](const FPackagePath& A, const FPackagePath& B) {
				return A.GetView() < B.GetView();
			});
		return Redirectors;
	}
	auto FContentBrowserAssetActions::FixUpRedirectorsInFolder(
		std::string_view VirtualDirectory) -> bool
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		if (VirtualDirectory.empty())
			return Complete({EAssetWriteError::InvalidPath, "Fix Up in Folder requires a mounted virtual directory."});
		const std::vector<FPackagePath> Redirectors =
			CollectRedirectors(VirtualDirectory);
		if (Redirectors.empty()) return true;
		return CompleteMutation(Durin::FixUpRedirectors(Redirectors, EAssetRedirectorFixupMode::RewriteAndDelete));
	}
	auto FContentBrowserAssetActions::FixUpRedirectors(
		std::span<const FPackagePath> Redirectors) -> bool
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		if (Redirectors.empty()) return true;
		return CompleteMutation(Durin::FixUpRedirectors(Redirectors, EAssetRedirectorFixupMode::RewriteAndDelete));
	}
	auto FContentBrowserAssetActions::FixUpAllRedirectors()
		-> bool
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		const std::vector<FPackagePath> Redirectors = CollectRedirectors("/");
		return Redirectors.empty() ? true
			: CompleteMutation(Durin::FixUpRedirectors(Redirectors, EAssetRedirectorFixupMode::RewriteAndDelete));
	}
	auto FContentBrowserAssetActions::CanDuplicateAsset(const FTopLevelAssetPath& Source, std::string* OutErrorMsg) const -> bool
	{
		if (!CanModify(OutErrorMsg)) return false;
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

	auto FContentBrowserAssetActions::ReportRecovery(const FAssetMutationResultDetails& Native) const -> void
	{
		for (const auto& Path : Native.BackupLocations) Report(ELogLevel::Warn, "Recovery backup: " + Path.generic_string());
		if (!Native.Result)
			for (const auto& Path : Native.AffectedFiles) Report(ELogLevel::Warn, "Affected file: " + Path.generic_string());
	}
	auto FContentBrowserAssetActions::CompleteMutation(const FAssetMutationResultDetails& Native, FContentChangeBatch Changes) -> bool
	{
		ReportRecovery(Native);
		if (!Native.Result && Native.Result.Effect != EAssetWriteEffect::None) Changes.bFullRefresh = true;
		if (!Native.AffectedFiles.empty() && Changes.Changes.empty()) Changes.bFullRefresh = true;
		return Complete(Native.Result, std::move(Changes));
	}
}
