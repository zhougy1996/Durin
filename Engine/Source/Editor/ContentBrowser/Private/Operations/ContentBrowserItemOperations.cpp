#include "Data/ContentBrowserRequestContext.h"
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
		auto IsReparsePoint(
			const std::filesystem::path& Path,
			std::error_code& OutError) -> bool
		{
			OutError.clear();
			const std::filesystem::file_status Status =
				std::filesystem::symlink_status(Path, OutError);
			if (OutError || std::filesystem::is_symlink(Status))
				return !OutError;
#ifdef _WIN32
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES)
			{
				OutError = std::error_code(
					static_cast<int>(GetLastError()), std::system_category());
				return false;
			}
			return (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			return false;
#endif
		}

		auto FindReparsePointInPath(
			const std::filesystem::path& Root,
			const std::filesystem::path& Candidate,
			std::filesystem::path& OutReparsePoint,
			std::error_code& OutError) -> bool
		{
			OutReparsePoint.clear();
			OutError.clear();
			std::filesystem::path Relative;
			if (!FPaths::TryMakeLexicalRelativePath(
					Candidate, Root, Relative))
			{
				OutError = std::make_error_code(std::errc::invalid_argument);
				return false;
			}
			std::filesystem::path Current = Root;
			for (const std::filesystem::path& Component : Relative)
			{
				Current /= Component;
				if (IsReparsePoint(Current, OutError))
				{
					OutReparsePoint = std::move(Current);
					return true;
				}
				if (OutError) return false;
			}
			return false;
		}

		auto MakeFingerprint(
			const std::filesystem::path& Path,
			EContentDeletionEntryKind Kind,
			FContentDeletionFingerprint& OutFingerprint,
			std::error_code& OutError) -> bool
		{
			OutError.clear();
			OutFingerprint = {};
			OutFingerprint.PhysicalPath = NormalizePath(Path.generic_string());
			OutFingerprint.Kind = Kind;
			if (Kind != EContentDeletionEntryKind::Directory)
			{
				OutFingerprint.FileSize = std::filesystem::file_size(Path, OutError);
				if (OutError) return false;
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, OutError);
			if (OutError) return false;
			OutFingerprint.LastWriteTimeTicks =
				static_cast<int64>(WriteTime.time_since_epoch().count());
			if (Kind != EContentDeletionEntryKind::Directory)
			{
				auto Identity = FFileHelper::HashFileXx128(Path);
				if (!Identity)
				{
					OutError = Identity.error().NativeError;
					return false;
				}
				OutFingerprint.ByteIdentity = *Identity;
				const uintmax_t FinalSize = std::filesystem::file_size(Path, OutError);
				if (OutError) return false;
				const auto FinalWriteTime =
					std::filesystem::last_write_time(Path, OutError);
				if (OutError) return false;
				if (FinalSize != OutFingerprint.FileSize
					|| static_cast<int64>(FinalWriteTime.time_since_epoch().count())
						!= OutFingerprint.LastWriteTimeTicks)
				{
					OutError = std::make_error_code(std::errc::state_not_recoverable);
					return false;
				}
			}
			return true;
		}

		auto AreSamePath(std::string_view A, std::string_view B) -> bool
		{
			std::filesystem::path Relative;
			return FPaths::TryMakeLexicalRelativePath(
				std::filesystem::path(A), std::filesystem::path(B), Relative)
				&& Relative.empty();
		}
	} // namespace


	auto FContentBrowserRequestContext::MakeMoveRequest(
		const FContentBrowserNativeItem& Item, std::string_view Directory,
		std::optional<std::string_view> NewName, FContentMove& OutRequest) const -> FAssetWriteResult
	{
		if (NewName && (NewName->empty() || *NewName == "." || *NewName == ".."
			|| NewName->find_first_of("/\\:*") != std::string_view::npos))
			return {EAssetWriteError::InvalidPath, "The new name is empty or contains invalid path characters."};
		OutRequest.Item = Item;
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const auto Package = FindPackageExact(Item.PackagePath);
			if (!Package || (!Item.PhysicalPath.empty() && !AreSamePath(Item.PhysicalPath, Package->PhysicalPath)))
				return {EAssetWriteError::StaleData, "The source asset no longer matches the selected item."};
			OutRequest.Item.PhysicalPath = Package->PhysicalPath;
		}
		if (OutRequest.Item.PhysicalPath.empty()) return {EAssetWriteError::InvalidPath, "The source path is empty."};
		OutRequest.Item.PhysicalPath = NormalizePath(OutRequest.Item.PhysicalPath);
		// Mixed drag payloads carry physical paths; recover registered package identity.
		if (Item.Kind == EContentBrowserItemKind::File)
		{
			const auto Classified = FMountPaths::ClassifyAssetPath(OutRequest.Item.PhysicalPath);
			FPackagePath PackagePath;
			if (Classified && Classified->NormalizedVirtualPath.ends_with(".dasset")
				&& FPackagePath::TryCreate(std::string_view(Classified->NormalizedVirtualPath).substr(
					0, Classified->NormalizedVirtualPath.size() - 7), PackagePath))
			{
				const auto Package = FindPackageExact(PackagePath);
				if (Package && AreSamePath(Package->PhysicalPath, OutRequest.Item.PhysicalPath))
				{
					OutRequest.Item.Kind = EContentBrowserItemKind::Asset;
					OutRequest.Item.PackagePath = PackagePath;
					if (Package->TopLevelAssets.size() == 1) OutRequest.Item.VirtualPath = Package->TopLevelAssets.front().AssetPath.ToString();
				}
			}
		}
		const std::filesystem::path Source(OutRequest.Item.PhysicalPath);
		std::filesystem::path Target;
		if (NewName)
		{
			Target = Source.parent_path() / *NewName;
			if (OutRequest.Item.Kind == EContentBrowserItemKind::Asset) Target += ".dasset";
			else if (OutRequest.Item.Kind == EContentBrowserItemKind::File && Target.extension().empty()) Target += Source.extension();
		}
		else
		{
			if (Directory.empty()) return {EAssetWriteError::InvalidPath, "The destination directory is empty."};
			Target = std::filesystem::path(Directory) / Source.filename();
		}
		OutRequest.Destination = NormalizePath(Target.generic_string());
		return {};
	}

	auto FContentBrowserRequestContext::ValidateMoveRequest(const FContentMove& Request) const -> FAssetWriteResult
	{
		const auto& Item = Request.Item;
		if (Item.Kind == EContentBrowserItemKind::Redirector)
			return {EAssetWriteError::InvalidPath, "Redirectors cannot be renamed or moved directly. Fix Up the redirector or move its final asset."};
		const auto Source = FContentBrowserPaths::ResolveMountPath(Item.PhysicalPath);
		const auto Target = FContentBrowserPaths::ResolveMountPath(Request.Destination);
		if (!Source || !Target) return {EAssetWriteError::InvalidPath, "Moving requires scanned content mounts."};
		if (!Source.Mount->bContentWritable || !Target.Mount->bContentWritable)
			return {EAssetWriteError::ReadOnlyMode, "Moving requires writable content mounts."};
		if (Item.Kind != EContentBrowserItemKind::Asset && Source.Mount != Target.Mount)
			return {EAssetWriteError::InvalidPath, "Ordinary files and folders must stay inside the same content mount."};
		if (AreSamePath(Item.PhysicalPath, Source.Mount->GetContentDir().generic_string()))
			return {EAssetWriteError::InvalidPath, "Content mount roots cannot be moved."};
		std::error_code Error;
		const auto SourceStatus = std::filesystem::symlink_status(Item.PhysicalPath, Error);
		if (Error || !std::filesystem::exists(SourceStatus)
			|| std::filesystem::is_directory(SourceStatus) != (Item.Kind == EContentBrowserItemKind::Folder))
			return {EAssetWriteError::InvalidPath, "The source content type has changed or is unavailable."};
		const auto Parent = std::filesystem::path(Request.Destination).parent_path();
		if (!std::filesystem::is_directory(Parent, Error) || Error)
			return {EAssetWriteError::InvalidPath, "The destination directory does not exist."};
		std::filesystem::path Reparse;
		if (IsReparsePoint(Source.Mount->GetContentDir().generic_string(), Error) || Error
			|| IsReparsePoint(Target.Mount->GetContentDir().generic_string(), Error) || Error
			|| FindReparsePointInPath(Source.Mount->GetContentDir().generic_string(), Item.PhysicalPath, Reparse, Error) || Error
			|| FindReparsePointInPath(Target.Mount->GetContentDir().generic_string(), Parent, Reparse, Error) || Error)
			return {EAssetWriteError::InvalidPath, "Content moves cannot traverse reparse points or unreadable paths."};
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const auto Package = FindPackageExact(Item.PackagePath);
			if (!Package) return {EAssetWriteError::StaleData, "The source asset is unavailable."};
			if (!Package->TopLevelAssets.empty() && std::ranges::all_of(Package->TopLevelAssets, &FAssetData::IsRedirector))
				return {EAssetWriteError::InvalidPath, "Fix Up redirectors before moving this content."};
		}
		if (Item.Kind == EContentBrowserItemKind::File)
		{
			if (!std::filesystem::is_regular_file(SourceStatus)) return {EAssetWriteError::InvalidPath, "The source is not a regular file."};
			if (StringUtils::FoldAscii(std::filesystem::path(Item.PhysicalPath).extension().string()) == ".dasset")
				return {EAssetWriteError::InvalidPath, "An unregistered asset package cannot be moved as an ordinary file."};
			FAssetCompanionOwnership Ownership;
			if (const auto Result = QueryAssetCompanionOwnership(Item.PhysicalPath, Ownership); !Result) return Result;
			if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
				return {EAssetWriteError::InUse, Ownership.State == EAssetCompanionOwnershipState::Owned
					? std::format("This file is managed by {}. Move the owning asset instead.", Ownership.Owners.front().ToString())
					: "This file is claimed by multiple assets. Resolve companion ownership before moving it."};
		}
		if (AreSamePath(Item.PhysicalPath, Request.Destination)) return {};
		if (ContentBrowserChanges::Within(Request.Destination, Item.PhysicalPath))
			return {EAssetWriteError::InvalidPath, "A destination cannot be inside a selected source."};
		const auto Probe = ContentBrowserFilesystem::Probe(Request.Destination);
		if (Probe.Error) return {EAssetWriteError::IoError, Probe.Error.message()};
		if (Probe.Exists()) return {EAssetWriteError::AlreadyExists, "An item with that name already exists."};
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			FPackagePath Package;
			std::string Virtual = Target.VirtualPath;
			if (!Virtual.ends_with(".dasset")) return {EAssetWriteError::InvalidPath, "The destination package extension is invalid."};
			Virtual.resize(Virtual.size() - 7);
			if (!FPackagePath::TryCreate(Virtual, Package)) return {EAssetWriteError::InvalidPath, "The destination asset path is invalid."};
			if (FindPackageExact(Package) || FindResidentPackage(Package))
				return {EAssetWriteError::AlreadyExists, "The destination asset path is already occupied."};
		}
		return {};
	}

	auto FContentBrowserRequestContext::RenameItemImpl(
		const FContentBrowserNativeItem& Item, std::string_view NewName,
		FContentBrowserNativeItem& OutNewItem, FContentChangeBatch& Changes) -> FAssetWriteResult
	{
		FContentMove Request;
		if (const auto Result = MakeMoveRequest(Item, {}, NewName, Request); !Result) return Result;
		const auto Result = MoveContent(std::span{&Request, 1}, Changes);
		if (!Result && Result.Effect != EAssetWriteEffect::ContentCommittedProjectionPending) return Result;
		OutNewItem = Request.Item;
		if (Request.Item.Kind == EContentBrowserItemKind::Asset)
		{
			std::string Virtual = FContentBrowserPaths::ResolveMountPath(Request.Destination).VirtualPath;
			Virtual.resize(Virtual.size() - 7);
			FPackagePath::TryCreate(Virtual, OutNewItem.PackagePath);
			FTopLevelAssetPath OldAsset, NewAsset;
			if (FTopLevelAssetPath::TryCreate(Request.Item.VirtualPath, OldAsset)
				&& FTopLevelAssetPath::TryCreate(OutNewItem.PackagePath, OldAsset.GetAssetName(), NewAsset))
				OutNewItem.VirtualPath = NewAsset.ToString();
			SetNewItem(OutNewItem, Request.Destination, OutNewItem.VirtualPath);
		}
		else SetNewItem(OutNewItem, Request.Destination);
		return Result;
	}

	auto FContentBrowserRequestContext::DuplicateItem(
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

	auto FContentBrowserRequestContext::DuplicateItem(
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

	auto FContentBrowserRequestContext::CopyItemsImpl(
		std::span<const FContentBrowserNativeItem> Items, std::string_view PhysicalDirectory, FContentChangeBatch& Changes)
		-> FAssetWriteResult
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Failure(EAssetWriteError::ReadOnlyMode, AdmissionError);
		using namespace ContentBrowserChanges;
		const auto Destination = FContentBrowserPaths::ResolveMountPath(PhysicalDirectory);
		if (!Destination || !Destination.Mount->bContentWritable)
			return {EAssetWriteError::ReadOnlyMode, "Paste requires a writable content folder."};
		std::error_code Ec;
		std::filesystem::path Reparse;
		if (!std::filesystem::is_directory(PhysicalDirectory, Ec) || Ec
			|| IsReparsePoint(Destination.Mount->GetContentDir().generic_string(), Ec) || Ec
			|| FindReparsePointInPath(Destination.Mount->GetContentDir().generic_string(), PhysicalDirectory, Reparse, Ec) || Ec)
			return {EAssetWriteError::InvalidPath, "The paste destination must be an existing folder without reparse points."};
		struct FFileCopy { std::filesystem::path Source; std::filesystem::path Target; };
		struct FAssetCopy { FTopLevelAssetPath Source; std::string Directory; };
		struct FCompanion { FAssetCompanionOwnership Ownership; bool bIndependent; };
		std::vector<FFileCopy> Files;
		std::vector<FAssetCopy> AssetCopies;
		std::vector<FCompanion> Companions;
		std::vector<std::filesystem::path> Directories;
		std::vector<std::string> Sources;
		std::vector<std::string> Targets;
		const auto Catalog = CaptureAssetCatalogSnapshot();
		for (const auto& Item : Items)
		{
			const auto Source = NormalizePath(Item.PhysicalPath);
			if (std::ranges::any_of(Sources, [&](const auto& Other) { return SamePath(Source, Other); })) continue;
			if (std::ranges::any_of(Items, [&](const auto& Other) {
				return Other.Kind == EContentBrowserItemKind::Folder
					&& !SamePath(Source, Other.PhysicalPath) && Within(Source, Other.PhysicalPath);
			})) continue;
			const auto SourceMount = FContentBrowserPaths::ResolveMountPath(Source);
			if (!SourceMount || SamePath(Source, SourceMount.Mount->GetContentDir().generic_string()))
				return {EAssetWriteError::InvalidPath, "Copy sources must be items inside a scanned content mount."};
			if (Item.Kind == EContentBrowserItemKind::Redirector)
				return {EAssetWriteError::InvalidPath, "Redirectors cannot be copied."};
			if (IsReparsePoint(SourceMount.Mount->GetContentDir().generic_string(), Ec) || Ec
				|| FindReparsePointInPath(SourceMount.Mount->GetContentDir().generic_string(), Source, Reparse, Ec) || Ec)
				return {EAssetWriteError::InvalidPath, "Copy cannot traverse reparse points or unreadable paths."};
			const bool bFolder = Item.Kind == EContentBrowserItemKind::Folder;
			if (std::filesystem::is_directory(Source, Ec) != bFolder || Ec)
				return {EAssetWriteError::InvalidPath, "The copied source type has changed."};
			if (bFolder && Within(Destination.NormalizedPhysicalPath, Source))
				return {EAssetWriteError::InvalidPath, "A folder cannot be pasted into itself or its descendants."};
			const auto Name = std::filesystem::path(Source).filename();
			std::filesystem::path Target;
			for (int Suffix = 0; Suffix < 1000; ++Suffix)
			{
				const auto CandidateName = Suffix == 0 ? Name.string()
					: (bFolder ? Name.string() : Name.stem().string()) + "_Copy"
						+ (Suffix == 1 ? std::string{} : std::to_string(Suffix))
						+ (bFolder ? std::string{} : Name.extension().string());
				const auto Candidate = std::filesystem::path(Destination.NormalizedPhysicalPath) / CandidateName;
				// A dangling link still occupies the name; never follow it to create a copy.
				const auto CandidateStatus = std::filesystem::symlink_status(Candidate, Ec);
				if (Ec == std::errc::no_such_file_or_directory) Ec.clear();
				if (Ec) return {EAssetWriteError::IoError, Ec.message()};
				if (std::filesystem::exists(CandidateStatus) || std::ranges::any_of(Targets, [&](const auto& Other) {
					return SamePath(Candidate.generic_string(), Other);
				})) continue;
				Target = Candidate;
				break;
			}
			if (Target.empty()) return {EAssetWriteError::AlreadyExists, "No free copy name is available."};
			Sources.push_back(Source);
			Targets.push_back(Target.generic_string());
			auto Collect = [&](const std::filesystem::path& Path, const std::filesystem::path& To,
				bool bIndependent) -> FAssetWriteResult {
				const auto EntryMount = FContentBrowserPaths::ResolveMountPath(Path.generic_string());
				const auto TargetMount = FContentBrowserPaths::ResolveMountPath(To.generic_string());
				if (!EntryMount || EntryMount.Mount != SourceMount.Mount
					|| !TargetMount || TargetMount.Mount != Destination.Mount)
					return {EAssetWriteError::InvalidPath, "Copy cannot cross nested mount boundaries."};
				if (IsReparsePoint(Path, Ec) || Ec)
					return {EAssetWriteError::InvalidPath, "Copy cannot include reparse points or unreadable entries."};
				if (std::filesystem::is_directory(Path, Ec))
				{
					Directories.push_back(To);
					return {};
				}
				if (Ec || !std::filesystem::is_regular_file(Path, Ec) || Ec)
					return {EAssetWriteError::NotFound, "The copied source is no longer a regular file."};
				for (const auto& [Package, Data] : Catalog.Assets)
				{
					if (!SamePath(Path.generic_string(), Data.PhysicalPath)) continue;
					if (Data.TopLevelAssets.size() != 1 || Data.TopLevelAssets.front().IsRedirector())
						return {EAssetWriteError::InvalidData, "Copy requires a real package with one top-level asset."};
					const auto Asset = Data.TopLevelAssets.front().AssetPath;
					std::string Error;
					if (!CanDuplicateAsset(Asset, &Error)) return Failure(EAssetWriteError::InvalidData, Error);
					AssetCopies.push_back({Asset, FContentBrowserPaths::PhysicalToVirtualDirectory(To.parent_path().generic_string())});
					return {};
				}
				const auto Extension = StringUtils::FoldAscii(Path.extension().string());
				if (Extension == ".dasset")
					return {EAssetWriteError::InvalidPath, "An unregistered package cannot be copied as an ordinary file."};
				FAssetCompanionOwnership Ownership;
				if (const auto Result = QueryAssetCompanionOwnership(Path.generic_string(), Ownership); !Result) return Result;
				if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
				{
					if (Extension != ".dbulk")
						return {EAssetWriteError::InvalidData, "Copy is not supported for this custom managed companion: " + Path.generic_string()};
					Companions.push_back({std::move(Ownership), bIndependent});
					return {};
				}
				if (Extension == ".dbulk")
					return {EAssetWriteError::InvalidPath, "An orphan bulk payload cannot be copied as an ordinary file."};
				Files.push_back({Path, To});
				return {};
			};
			if (const auto Result = Collect(Source, Target, !bFolder); !Result) return Result;
			if (bFolder)
			{
				for (std::filesystem::recursive_directory_iterator It(Source, Ec), End;
					!Ec && It != End; It.increment(Ec))
					if (const auto Result = Collect(It->path(), Target / It->path().lexically_relative(Source), false); !Result)
						return Result;
				if (Ec) return {EAssetWriteError::IoError, "Could not inspect the copied folder: " + Ec.message()};
			}
		}
		for (const auto& Companion : Companions)
			if (Companion.bIndependent || Companion.Ownership.State == EAssetCompanionOwnershipState::Ambiguous
				|| std::ranges::any_of(Companion.Ownership.Owners, [&](const auto& Owner) {
					return std::ranges::none_of(AssetCopies, [&](const auto& Copy) { return Copy.Source.GetPackagePath() == Owner; });
				}))
				return {EAssetWriteError::InUse, "Copy the owning asset instead of its managed companion."};

		std::vector<std::filesystem::path> CreatedFiles;
		std::vector<std::filesystem::path> CreatedDirectories;
		bool bAssetEffects = false;
		auto FailCopy = [&](FAssetWriteResult Result) {
			std::string Warning;
			if (!bAssetEffects)
			{
				for (auto It = CreatedFiles.rbegin(); It != CreatedFiles.rend(); ++It)
				{
					std::filesystem::remove(*It, Ec);
					if (Ec) Warning += " Could not remove " + It->generic_string() + ": " + Ec.message();
				}
				for (auto It = CreatedDirectories.rbegin(); It != CreatedDirectories.rend(); ++It)
				{
					std::filesystem::remove(*It, Ec);
					if (Ec) Warning += " Could not remove " + It->generic_string() + ": " + Ec.message();
				}
			}
			else Warning += " Created asset copies remain available. Inspect the destination before retrying.";
			Changes = {.bFullRefresh = Changes.bFullRefresh || bAssetEffects || !Warning.empty()};
			if (!Warning.empty()) Report(ELogLevel::Warn, Warning);
			return Result;
		};
		std::ranges::sort(Directories, {}, [](const auto& Path) { return Path.native().size(); });
		for (const auto& Directory : Directories)
		{
			if (!std::filesystem::create_directory(Directory, Ec) || Ec)
				return FailCopy({EAssetWriteError::IoError, "Could not create copied folder: " + Directory.generic_string()});
			CreatedDirectories.push_back(Directory);
		}
		for (const auto& File : Files)
		{
			if (!std::filesystem::copy_file(File.Source, File.Target, std::filesystem::copy_options::none, Ec) || Ec)
			{
				FAssetWriteResult Result{EAssetWriteError::IoError,
					"Could not copy " + File.Source.generic_string() + ": " + Ec.message()};
				// A failed copy may leave a partial destination; do not delete a competing writer's file.
				Report(ELogLevel::Warn, "Inspect failed destination: " + File.Target.generic_string());
				Changes.bFullRefresh = true;
				return FailCopy(std::move(Result));
			}
			CreatedFiles.push_back(File.Target);
		}

		for (const auto& Copy : AssetCopies)
		{
			auto Context = Feedback();
			auto* Asset = IAssetTools::Get().DuplicateAsset({.SourcePath = Copy.Source, .DestinationDirectory = Copy.Directory}, &Context);
			bAssetEffects |= Asset != nullptr;
			if (!Asset || !IAssetTools::Get().SaveAssets({.AssetPaths = {Asset->GetPackage()->GetPackagePathIdentity()}}, &Context))
				return FailCopy({EAssetWriteError::IoError, "Copy stopped at " + Copy.Source.ToString() + " into " + Copy.Directory});
			const auto Resolved = FMountPaths::ResolveAssetPath(Asset->GetPackage()->GetPackagePathIdentity().GetView());
			Changes.Changes.push_back({EContentChangeKind::Added, {}, Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{}});
		}

		for (const auto& Path : CreatedFiles)
			Changes.Changes.push_back({EContentChangeKind::Added, {}, Path.generic_string()});
		for (const auto& Path : CreatedDirectories)
			Changes.Changes.push_back({EContentChangeKind::Added, {}, Path.generic_string(), {}, {}, true});
		return {};
	}



	// All physical paths and package mappings are collected before touching disk.
	auto FContentBrowserRequestContext::BulkMoveItems(std::span<const FContentBrowserNativeItem> Items, std::string_view Directory) -> bool
	{
		std::vector<FContentMove> Requests;
		for (const auto& Item : Items)
		{
			FContentMove Request;
			if (const auto Result = MakeMoveRequest(Item, Directory, std::nullopt, Request); !Result) return Complete(Result);
			Requests.push_back(std::move(Request));
		}
		FContentChangeBatch Changes;
		const auto Status = MoveContent(Requests, Changes);
		return Complete(Status, std::move(Changes));
	}

	auto FContentBrowserRequestContext::MoveContent(std::span<const FContentMove> Requests, FContentChangeBatch& Changes)
		-> FAssetWriteResult
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Failure(EAssetWriteError::ReadOnlyMode, AdmissionError);
		using namespace ContentBrowserChanges;
		std::vector<FContentMove> Roots;
		for (auto Request : Requests)
		{
			Request.Item.PhysicalPath = NormalizePath(Request.Item.PhysicalPath);
			Request.Destination = NormalizePath(Request.Destination);
			const bool Covered = std::ranges::any_of(Requests, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder
					&& !SamePath(Other.Item.PhysicalPath, Request.Item.PhysicalPath)
					&& Within(Request.Item.PhysicalPath, Other.Item.PhysicalPath);
			});
			if (Covered) continue;
			if (const auto Result = ValidateMoveRequest(Request); !Result) return Result;
			if (SamePath(Request.Item.PhysicalPath, Request.Destination)) continue;
			if (std::ranges::any_of(Roots, [&](const auto& Other) {
				return SamePath(Other.Item.PhysicalPath, Request.Item.PhysicalPath);
			})) continue;
			Roots.push_back(std::move(Request));
		}
		if (Roots.empty()) return {};
		FContentChangeBatch PlannedChanges;
		struct FPhysicalMove { std::filesystem::path Source; std::filesystem::path Destination; };
		std::vector<FPhysicalMove> Files;
		std::vector<std::filesystem::path> Directories;
		std::vector<std::filesystem::path> OldDirectories;
		std::vector<FAssetRelocationMapping> AssetMoves;
		struct FCompanion { std::string Path; FAssetCompanionOwnership Ownership; bool bIndependent; };
		std::vector<FCompanion> Companions;
		const auto Catalog = CaptureAssetCatalogSnapshot();
		FAssetWriteResult Status;
		std::error_code Ec;
		for (const auto& Root : Roots)
		{
			const auto& Source = Root.Item.PhysicalPath;
			const auto& Destination = Root.Destination;
			const auto SourceMount = FContentBrowserPaths::ResolveMountPath(Source);
			for (const auto& Other : Roots)
			{
				if (Within(Destination, Other.Item.PhysicalPath))
					return {EAssetWriteError::InvalidPath, "A destination cannot be inside a selected source."};
				if (&Other != &Root && SamePath(Destination, Other.Destination))
					return {EAssetWriteError::AlreadyExists, "Selected items have the same destination."};
			}
			const bool bFolder = Root.Item.Kind == EContentBrowserItemKind::Folder;
			bool bPackageRoot = false;
			auto Collect = [&](const std::filesystem::path& Path, const std::filesystem::path& Target,
				bool bIndependent) -> FAssetWriteResult {
				if (IsReparsePoint(Path, Ec) || Ec)
					return {EAssetWriteError::InvalidPath, "Content moves cannot include reparse points or unreadable entries."};
				const auto EntryMount = FContentBrowserPaths::ResolveMountPath(Path.generic_string());
				if (!EntryMount || EntryMount.Mount != SourceMount.Mount)
					return {EAssetWriteError::InvalidPath, "A folder contains a different content mount."};
				if (std::filesystem::is_directory(Path, Ec))
				{
					Directories.push_back(Target);
					OldDirectories.push_back(Path);
					return {};
				}
				if (Ec || !std::filesystem::is_regular_file(Path, Ec) || Ec)
					return {EAssetWriteError::InvalidPath, "The source is not a regular file or directory."};
				const std::string Physical = NormalizePath(Path.generic_string());
				for (const auto& [Package, Data] : Catalog.Assets)
				{
					if (!SamePath(Physical, Data.PhysicalPath)) continue;
					if (bIndependent) bPackageRoot = true;
					if ((!Data.TopLevelAssets.empty() && std::ranges::all_of(Data.TopLevelAssets, &FAssetData::IsRedirector)))
						return {EAssetWriteError::InvalidPath, "Fix Up redirectors before moving this content."};
					FPackagePath NewPath;
					auto Virtual = FContentBrowserPaths::ResolveMountPath(Target.generic_string()).VirtualPath;
					if (!Virtual.ends_with(".dasset"))
						return {EAssetWriteError::InvalidPath, "The destination package extension is invalid."};
					Virtual.resize(Virtual.size() - 7);
					if (!FPackagePath::TryCreate(Virtual, NewPath))
						return {EAssetWriteError::InvalidPath, "The destination asset path is invalid."};
					if (FindPackageExact(NewPath) || FindResidentPackage(NewPath))
						return {EAssetWriteError::AlreadyExists, "The destination asset path is already occupied."};
					AssetMoves.push_back({Package, NewPath});
					for (const auto& Asset : Data.TopLevelAssets)
					{
						FTopLevelAssetPath NewAsset;
						if (FTopLevelAssetPath::TryCreate(NewPath, Asset.AssetPath.GetAssetName(), NewAsset))
							PlannedChanges.Changes.push_back({EContentChangeKind::Renamed, Physical,
								Target.generic_string(), Asset.AssetPath.ToString(), NewAsset.ToString()});
					}
					return {};
				}
				if (StringUtils::FoldAscii(Path.extension().string()) == ".dasset")
					return {EAssetWriteError::InvalidPath, "An unregistered asset package cannot be moved as an ordinary file."};
				FAssetCompanionOwnership Ownership;
				if (const auto Result = QueryAssetCompanionOwnership(Physical, Ownership); !Result) return Result;
				if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
				{
					Companions.push_back({Physical, std::move(Ownership), bIndependent});
					return {};
				}
				Files.push_back({Path, Target});
				return {};
			};
			if (const auto Result = Collect(Source, Destination, !bFolder); !Result) return Result;
			if (bFolder)
			{
				for (std::filesystem::recursive_directory_iterator It(Source, Ec), End;
					!Ec && It != End; It.increment(Ec))
				{
					const auto Relative = It->path().lexically_relative(Source);
					if (const auto Result = Collect(It->path(), std::filesystem::path(Destination) / Relative, false); !Result)
						return Result;
				}
				if (Ec) return {EAssetWriteError::IoError, "Could not inspect the entire source folder: " + Ec.message()};
			}
			if (!bPackageRoot) PlannedChanges.Changes.push_back({EContentChangeKind::Renamed, Source, Destination,
				bFolder ? FContentBrowserPaths::PhysicalToVirtualDirectory(Source) : std::string{},
				bFolder ? FContentBrowserPaths::PhysicalToVirtualDirectory(Destination) : std::string{}, bFolder});
		}
		for (const auto& Companion : Companions)
		{
			if (Companion.bIndependent || Companion.Ownership.State == EAssetCompanionOwnershipState::Ambiguous
				|| std::ranges::any_of(Companion.Ownership.Owners, [&](const auto& Owner) {
					return std::ranges::none_of(AssetMoves, [&](const auto& Move) { return Move.SourcePath == Owner; });
				}))
				return {EAssetWriteError::InUse, "Move the owning asset instead of its managed companion: " + Companion.Path};
		}
		if (const auto Allowed = ValidateMoves(AssetMoves); !Allowed) return Allowed;
		Changes = std::move(PlannedChanges);
		std::vector<std::filesystem::path> CreatedDirectories;
		size_t MovedFiles = 0;
		auto RollBack = [&](FAssetWriteResult Result) {
			std::string Errors;
			while (MovedFiles > 0)
			{
				const auto& File = Files[--MovedFiles];
				const auto Probe = ContentBrowserFilesystem::Probe(File.Source);
				if (Probe.Error || Probe.Exists())
					Errors += " Cannot restore " + File.Source.generic_string() + "; source occupied or unreadable.";
				else
				{
					std::filesystem::rename(File.Destination, File.Source, Ec);
					if (Ec) Errors += " Cannot restore " + File.Source.generic_string() + ": " + Ec.message();
				}
			}
			for (auto It = CreatedDirectories.rbegin(); It != CreatedDirectories.rend(); ++It)
			{
				std::filesystem::remove(*It, Ec);
				if (Ec) Errors += " Could not remove destination directory " + It->generic_string() + ": " + Ec.message();
			}
			if (!Errors.empty())
			{
				Result.Message += Errors;
				Report(ELogLevel::Warn, Errors);
				Result.Effect = EAssetWriteEffect::ContentUncertain;
			}
			Changes = {};
			Changes.bFullRefresh = Result.Effect != EAssetWriteEffect::None;
			return Result;
		};
		// Create parents before children; never merge into existing destinations.
		std::ranges::sort(Directories, {}, [](const auto& Path) { return Path.native().size(); });
		for (const auto& Directory : Directories)
		{
			if (!std::filesystem::create_directory(Directory, Ec) || Ec)
				return RollBack({EAssetWriteError::IoError, "Could not create destination directory: " + Directory.generic_string()});
			CreatedDirectories.push_back(Directory);
		}
		for (const auto& File : Files)
		{
			const auto Probe = ContentBrowserFilesystem::Probe(File.Destination);
			if (Probe.Error || Probe.Exists())
				return RollBack({EAssetWriteError::AlreadyExists, "The destination changed during the move."});
			std::filesystem::rename(File.Source, File.Destination, Ec);
			if (Ec) return RollBack({EAssetWriteError::IoError, "File move failed: " + Ec.message()});
			++MovedFiles;
		}
		if (!AssetMoves.empty())
		{
			auto Native = RelocateAssets(AssetMoves);
			ReportRecovery(Native);
			auto Result = std::move(Native.Result);
			const bool bCommitted = Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending;
			if (!Result && !bCommitted)
			{
				// AssetTools owns partial-effect diagnostics and backups. Never attempt inverse relocation.
				return RollBack(std::move(Result));
			}
			Status = std::move(Result);
			if (bCommitted) Changes.bFullRefresh = true;
		}
		std::ranges::sort(OldDirectories, [](const auto& A, const auto& B) { return A.native().size() > B.native().size(); });
		for (const auto& Directory : OldDirectories)
		{
			Ec.clear();
			if (!std::filesystem::remove(Directory, Ec) || Ec)
				Report(ELogLevel::Warn, "Content was moved successfully, but a source folder could not be removed (redirectors or cleanup failed): " + Directory.generic_string());
		}
		return Status;
	}

	auto FContentBrowserRequestContext::ValidateFolder(
		std::string_view PhysicalDirectory, std::string_view Name, FContentBrowserNativeItem& OutNewItem) const -> FAssetWriteResult
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Failure(EAssetWriteError::ReadOnlyMode, AdmissionError);
		if (Name.empty() || Name == "." || Name == ".."
			|| Name.find_first_of("/\\:*?\"<>|") != std::string_view::npos
			|| Name.back() == '.' || Name.back() == ' '
			|| std::ranges::any_of(Name, [](unsigned char Character) { return Character < 32; }))
			return Failure(EAssetWriteError::InvalidPath, "Enter a valid folder name without path separators.");
		const std::string Directory = NormalizePath(PhysicalDirectory);
		const auto Mount = FContentBrowserPaths::ResolveMountPath(Directory);
		if (!Mount)
			return Failure(EAssetWriteError::InvalidPath, "Folders require an automatically scanned content mount.");
		if (!Mount.Mount->bContentWritable)
			return Failure(EAssetWriteError::ReadOnlyMode, "This content mount is read-only.");
		std::error_code Error;
		std::filesystem::path Reparse;
		if (!std::filesystem::is_directory(Directory, Error) || Error
			|| FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Directory, Reparse, Error) || Error)
			return Failure(EAssetWriteError::InvalidPath, "The target must be an existing directory without reparse points.");
		const auto Path = (std::filesystem::path(Directory) / Name).generic_string();
		const auto Destination = FContentBrowserPaths::ResolveMountPath(Path);
		if (!Destination || Destination.Mount != Mount.Mount)
			return Failure(EAssetWriteError::InvalidPath, "The new folder would leave its content mount.");
		const auto Probe = ContentBrowserFilesystem::Probe(Path);
		if (Probe.Error) return Failure(EAssetWriteError::IoError, "Could not inspect the new folder path: " + Probe.Error.message());
		if (Probe.Exists()) return Failure(EAssetWriteError::AlreadyExists, "An item with this name already exists.");
		OutNewItem = {};
		OutNewItem.Kind = EContentBrowserItemKind::Folder;
		SetNewItem(OutNewItem, NormalizePath(Path));
		return {};
	}

	auto FContentBrowserRequestContext::PrepareFolder(
		std::string_view Directory, FContentBrowserNativeItem& OutNewItem) -> bool
	{
		OutNewItem = {};
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const auto Name = Suffix == 0 ? std::string("New Folder") : std::format("New Folder ({})", Suffix + 1);
			const auto Status = ValidateFolder(Directory, Name, OutNewItem);
			if (Status.Error == EAssetWriteError::AlreadyExists) continue;
			return Complete(Status);
		}
		return Complete(Failure(EAssetWriteError::AlreadyExists, "Could not find a unique folder name in this directory."));
	}

	auto FContentBrowserRequestContext::CollectRedirectors(
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

	auto FContentBrowserRequestContext::FixUpRedirectorsInFolder(
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

	auto FContentBrowserRequestContext::FixUpRedirectors(
		std::span<const FPackagePath> Redirectors) -> bool
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		if (Redirectors.empty()) return true;
		return CompleteMutation(Durin::FixUpRedirectors(Redirectors, EAssetRedirectorFixupMode::RewriteAndDelete));
	}

	auto FContentBrowserRequestContext::FixUpAllRedirectors()
		-> bool
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		const std::vector<FPackagePath> Redirectors = CollectRedirectors("/");
		return Redirectors.empty() ? true
			: CompleteMutation(Durin::FixUpRedirectors(Redirectors, EAssetRedirectorFixupMode::RewriteAndDelete));
	}

	auto FContentBrowserRequestContext::AnalyzeDeletion(
		std::span<const FContentBrowserNativeItem> Items,
		FAssetDeletionOperation& OutOperation) const
		-> FContentDeletionPlanPtr
	{
		auto Plan = std::make_shared<FContentDeletionPlan>();

		auto AddBlocker = [&](EContentDeletionBlocker Kind,
			std::string DisplayName,
			std::string PhysicalPath,
			std::string RelatedAssetPath,
			std::string Details) {
			Plan->Blockers.push_back({
				.Kind = Kind,
				.DisplayName = std::move(DisplayName),
				.PhysicalPath = std::move(PhysicalPath),
				.RelatedAssetPath = std::move(RelatedAssetPath),
				.Details = std::move(Details)});
		};

		if (Items.empty())
		{
			AddBlocker(
				EContentDeletionBlocker::InvalidSelection,
				"Selection", {}, {}, "No content is selected.");
			return Plan;
		}

		struct FSelectedRoot
		{
			const FContentBrowserNativeItem* Item = nullptr;
			std::string PhysicalPath;
			const FMountPoint* Mount = nullptr;
		};
		std::vector<FSelectedRoot> SelectedRoots;
		for (const FContentBrowserNativeItem& Item : Items)
		{
			const std::string PhysicalPath = NormalizePath(Item.PhysicalPath);
			const FContentBrowserPaths::FMountPath Resolved =
				FContentBrowserPaths::ResolveMountPath(PhysicalPath);
			if (!Resolved)
			{
				AddBlocker(
					EContentDeletionBlocker::OutsideMount,
					Item.Name,
					PhysicalPath,
					{},
					"Selected path is outside every mounted content root.");
				continue;
			}
			const FMountPoint* Mount = Resolved.Mount;
			if (AreSamePath(PhysicalPath, Mount->GetContentDir().generic_string()))
				AddBlocker(
					EContentDeletionBlocker::MountRoot,
					Item.Name,
					PhysicalPath,
					{},
					"A mounted content root cannot be deleted.");
			if (!Mount->bContentWritable)
				AddBlocker(
					EContentDeletionBlocker::ReadOnlyMount,
					Item.Name,
					PhysicalPath,
					{},
					"The selected mount is not content-writable.");
			SelectedRoots.push_back({&Item, PhysicalPath, Mount});
		}

		if (SelectedRoots.size() != Items.size())
			AddBlocker(
				EContentDeletionBlocker::InvalidSelection,
				"Selection",
				{},
				{},
				"One or more selected items are absent from the analyzed item set.");
		std::ranges::sort(
			SelectedRoots,
			[](const FSelectedRoot& A, const FSelectedRoot& B) {
				if (A.PhysicalPath.size() != B.PhysicalPath.size())
					return A.PhysicalPath.size() < B.PhysicalPath.size();
				return A.PhysicalPath < B.PhysicalPath;
			});
		std::vector<FSelectedRoot> MaximalRoots;
		for (const FSelectedRoot& Candidate : SelectedRoots)
		{
			if (!MaximalRoots.empty()
				&& Candidate.Mount != MaximalRoots.front().Mount)
				AddBlocker(
					EContentDeletionBlocker::UnsupportedMount,
					Candidate.Item->Name,
					Candidate.PhysicalPath,
					{},
					"One deletion plan cannot span multiple content mounts.");
			if (std::ranges::any_of(
					MaximalRoots,
					[&](const FSelectedRoot& Existing) {
						return FPaths::IsLexicalDescendantPath(
							Candidate.PhysicalPath, Existing.PhysicalPath, true);
					}))
				continue;
			MaximalRoots.push_back(Candidate);
		}
		if (MaximalRoots.size() == 1)
			Plan->DisplayName = MaximalRoots.front().Item->Name;
		else
			Plan->DisplayName = std::format("{} Items", MaximalRoots.size());

		const FAssetCatalogSnapshot Catalog =
			CaptureAssetCatalogSnapshot();
		std::unordered_map<std::string, const FPackageData*> AssetsByPhysicalPath;
		for (const auto& [Path, Data] : Catalog.Assets)
			AssetsByPhysicalPath.emplace(NormalizePath(Data.PhysicalPath), &Data);
		std::vector<FPackagePath> AssetPaths;
		std::vector<std::filesystem::path> PhysicalRoots;

		auto AddPhysicalEntry = [&](const std::filesystem::path& Physical,
			bool bDirectory) {
			const std::string Normalized = NormalizePath(Physical.generic_string());
			EContentDeletionEntryKind Kind = bDirectory
				? EContentDeletionEntryKind::Directory
				: EContentDeletionEntryKind::OrdinaryFile;
			if (!bDirectory && Physical.extension() == ".dasset")
			{
				if (const auto Asset = AssetsByPhysicalPath.find(Normalized);
					Asset != AssetsByPhysicalPath.end())
				{
					Kind = EContentDeletionEntryKind::AssetPackage;
					AssetPaths.push_back(Asset->second->PackagePath);
				}
				else
				{
					Kind = EContentDeletionEntryKind::UnknownPackage;
					AddBlocker(
						EContentDeletionBlocker::UnknownPackage,
						Physical.filename().generic_string(),
						Normalized,
						{},
						"Package file is not registered or has invalid metadata.");
				}
			}
			FContentDeletionFingerprint Fingerprint;
			std::error_code Ec;
			if (!MakeFingerprint(Physical, Kind, Fingerprint, Ec))
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					Physical.filename().generic_string(),
					Normalized,
					{},
					std::format("Could not fingerprint content: {}", Ec.message()));
				return;
			}
			Plan->Entries.push_back(std::move(Fingerprint));
		};

		for (const FSelectedRoot& Root : MaximalRoots)
		{
			const std::filesystem::path Physical(Root.PhysicalPath);
			PhysicalRoots.push_back(Physical);
			std::error_code Ec;
			const bool bReparse = IsReparsePoint(Physical, Ec);
			if (Ec)
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					std::format("Could not inspect selected path: {}", Ec.message()));
				continue;
			}
			if (bReparse)
			{
				AddBlocker(
					EContentDeletionBlocker::ReparsePoint,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					"Reparse points cannot be traversed or staged.");
				continue;
			}
			const bool bDirectory = std::filesystem::is_directory(Physical, Ec);
			if (Ec || (!bDirectory && !std::filesystem::is_regular_file(Physical, Ec)))
			{
				AddBlocker(
					EContentDeletionBlocker::InvalidSelection,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					"Selected content no longer exists as a supported file or directory.");
				continue;
			}
			AddPhysicalEntry(Physical, bDirectory);
			Plan->MaximalRoots.push_back({
				.OriginalPath = Root.PhysicalPath});
			if (!bDirectory) continue;

			for (std::filesystem::recursive_directory_iterator It(
					 Physical, std::filesystem::directory_options::none, Ec),
				 End;
				 !Ec && It != End;
				 It.increment(Ec))
			{
				std::error_code EntryEc;
				if (IsReparsePoint(It->path(), EntryEc))
				{
					AddBlocker(
						EContentDeletionBlocker::ReparsePoint,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						"Reparse points cannot be traversed or staged.");
					It.disable_recursion_pending();
					continue;
				}
				if (EntryEc)
				{
					AddBlocker(
						EContentDeletionBlocker::InspectionFailed,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						std::format("Could not inspect descendant: {}", EntryEc.message()));
					It.disable_recursion_pending();
					continue;
				}
				const bool bChildDirectory = It->is_directory(EntryEc);
				if (!EntryEc && (bChildDirectory || It->is_regular_file(EntryEc)))
					AddPhysicalEntry(It->path(), bChildDirectory);
				else if (EntryEc)
					AddBlocker(
						EContentDeletionBlocker::InspectionFailed,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						std::format("Could not classify descendant: {}", EntryEc.message()));
			}
			if (Ec)
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					std::format("Could not enumerate folder contents: {}", Ec.message()));
		}

		std::ranges::sort(AssetPaths, [](const FPackagePath& A, const FPackagePath& B) {
			return A.GetView() < B.GetView();
		});
		AssetPaths.erase(std::unique(AssetPaths.begin(), AssetPaths.end()), AssetPaths.end());
		std::string AdmissionError;
		FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) { if (Level >= ELogLevel::Error) AdmissionError += Message; });
		const bool bPrepared = IAssetTools::Get().PrepareDeletion({.AssetPaths = AssetPaths, .PhysicalRoots = PhysicalRoots}, OutOperation, &Feedback);
		if (!bPrepared && OutOperation.GetBlockers().empty())
			AddBlocker(EContentDeletionBlocker::InspectionFailed, "Assets", {}, {}, AdmissionError);

		std::unordered_set<std::string> CompanionPaths;
		for (const FAssetDeletionEntry& Entry : OutOperation.GetEntries())
			for (const std::filesystem::path& Companion : Entry.CompanionFiles)
				CompanionPaths.insert(NormalizePath(Companion.generic_string()));
		for (FContentDeletionFingerprint& Entry : Plan->Entries)
			if (CompanionPaths.contains(Entry.PhysicalPath)
				&& Entry.Kind == EContentDeletionEntryKind::OrdinaryFile)
				Entry.Kind = EContentDeletionEntryKind::ManagedCompanion;

		for (const std::string& CompanionPath : CompanionPaths)
		{
			if (std::ranges::any_of(
					Plan->Entries,
					[&](const FContentDeletionFingerprint& Entry) {
						return Entry.PhysicalPath == CompanionPath;
					}))
				continue;
			std::error_code Ec;
			if (!std::filesystem::is_regular_file(CompanionPath, Ec))
			{
				if (Ec
					&& Ec != std::errc::no_such_file_or_directory
					&& Ec != std::errc::not_a_directory)
					AddBlocker(
						EContentDeletionBlocker::InspectionFailed,
						std::filesystem::path(CompanionPath).filename().generic_string(),
						CompanionPath,
						{},
						std::format("Could not classify asset companion: {}", Ec.message()));
				continue;
			}

			const FMountPoint* SelectedMount =
				MaximalRoots.empty() ? nullptr : MaximalRoots.front().Mount;
			std::filesystem::path SelectedReparsePoint;
			if (SelectedMount
				&& FPaths::IsLexicalDescendantPath(
					CompanionPath, SelectedMount->GetContentDir().generic_string(), true)
				&& FindReparsePointInPath(
					SelectedMount->GetContentDir().generic_string(),
					CompanionPath,
					SelectedReparsePoint,
					Ec))
			{
				AddBlocker(
					EContentDeletionBlocker::ReparsePoint,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format(
						"Asset companion path traverses reparse point {}.",
						SelectedReparsePoint.generic_string()));
				continue;
			}
			if (Ec)
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not inspect asset companion: {}", Ec.message()));
				continue;
			}

			const FContentBrowserPaths::FMountPath Resolved =
				FContentBrowserPaths::ResolveMountPath(CompanionPath);
			if (!Resolved)
			{
				AddBlocker(
					EContentDeletionBlocker::OutsideMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"Asset companion is outside every mounted content root.");
				continue;
			}
			const FMountPoint* CompanionMount = Resolved.Mount;
			bool bCompanionRootSafe = true;
			if (!SelectedMount || CompanionMount != SelectedMount)
			{
				AddBlocker(
					EContentDeletionBlocker::UnsupportedMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"An external asset companion must belong to the selected content mount.");
				bCompanionRootSafe = false;
			}
			if (!CompanionMount->bContentWritable)
			{
				AddBlocker(
					EContentDeletionBlocker::ReadOnlyMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"The asset companion's content mount is not content-writable.");
				bCompanionRootSafe = false;
			}
			std::filesystem::path ReparsePoint;
			const bool bReparse = FindReparsePointInPath(
				CompanionMount->GetContentDir().generic_string(), CompanionPath, ReparsePoint, Ec);
			if (Ec)
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not inspect asset companion: {}", Ec.message()));
				continue;
			}
			if (bReparse)
			{
				AddBlocker(
					EContentDeletionBlocker::ReparsePoint,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format(
						"Asset companion path traverses reparse point {}.",
						ReparsePoint.generic_string()));
				bCompanionRootSafe = false;
			}
			if (!bCompanionRootSafe) continue;
			FContentDeletionFingerprint Fingerprint;
			if (!MakeFingerprint(
					CompanionPath,
					EContentDeletionEntryKind::ManagedCompanion,
					Fingerprint,
					Ec))
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not fingerprint companion: {}", Ec.message()));
				continue;
			}
			Plan->Entries.push_back(Fingerprint);
			Plan->MaximalRoots.push_back({
				.OriginalPath = CompanionPath});
		}

		for (const FAssetDeletionBlocker& Blocker : OutOperation.GetBlockers())
		{
			EContentDeletionBlocker Kind = EContentDeletionBlocker::InspectionFailed;
			switch (Blocker.Kind)
			{
			case EAssetDeletionBlocker::ExternalPersistentReference:
			case EAssetDeletionBlocker::ExternalLoadedReference:
				Kind = EContentDeletionBlocker::ExternalReference;
				break;
			case EAssetDeletionBlocker::RedirectorTargetNotSelected:
				Kind = EContentDeletionBlocker::RedirectorTargetNotSelected;
				break;
			case EAssetDeletionBlocker::TargetRedirectorsNotSelected:
				Kind = EContentDeletionBlocker::TargetRedirectorsNotSelected;
				break;
			case EAssetDeletionBlocker::LoadingPackage:
				Kind = EContentDeletionBlocker::LoadingPackage;
				break;
			case EAssetDeletionBlocker::DirtyPackage:
				Kind = EContentDeletionBlocker::DirtyPackage;
				break;
			case EAssetDeletionBlocker::ReferenceStoreInspectionFailed:
				Kind = EContentDeletionBlocker::ReferenceStoreInspectionFailed;
				break;
			case EAssetDeletionBlocker::CompanionInspectionFailed:
				Kind = EContentDeletionBlocker::CompanionInspectionFailed;
				break;
			case EAssetDeletionBlocker::CompanionOwnershipConflict:
				Kind = EContentDeletionBlocker::CompanionOwnershipConflict;
				break;
			case EAssetDeletionBlocker::ExternalCompanionOwner:
				Kind = EContentDeletionBlocker::ExternalCompanionOwner;
				break;
			default:
				break;
			}
			AddBlocker(
				Kind,
				Blocker.AssetPath.ToString(),
				NormalizePath(Blocker.PhysicalPath.generic_string()),
				Blocker.RelatedAssetPath.ToString(),
				Blocker.Details);
		}
		for (const FAssetDeletionWarning& Warning : OutOperation.GetWarnings())
			Plan->Warnings.push_back({
				.DisplayName = Warning.AssetPath.ToString(),
				.Details = Warning.Details});

		std::ranges::sort(
			Plan->Entries,
			{},
			&FContentDeletionFingerprint::PhysicalPath);
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
			switch (Entry.Kind)
			{
			case EContentDeletionEntryKind::Directory:
				++Plan->Summary.FolderCount;
				break;
			case EContentDeletionEntryKind::AssetPackage:
				++Plan->Summary.AssetCount;
				break;
			case EContentDeletionEntryKind::ManagedCompanion:
				++Plan->Summary.CompanionCount;
				break;
			case EContentDeletionEntryKind::OrdinaryFile:
			case EContentDeletionEntryKind::UnknownPackage:
				++Plan->Summary.FileCount;
				break;
			}
		std::ranges::sort(
			Plan->Blockers,
			[](const FContentDeletionBlocker& A, const FContentDeletionBlocker& B) {
				return std::tie(A.PhysicalPath, A.RelatedAssetPath, A.Kind, A.Details)
					< std::tie(B.PhysicalPath, B.RelatedAssetPath, B.Kind, B.Details);
			});
		Plan->Blockers.erase(
			std::unique(Plan->Blockers.begin(), Plan->Blockers.end(),
				[](const FContentDeletionBlocker& A,
					const FContentDeletionBlocker& B) {
					return A.Kind == B.Kind
						&& A.PhysicalPath == B.PhysicalPath
						&& A.RelatedAssetPath == B.RelatedAssetPath
						&& A.Details == B.Details;
				}),
			Plan->Blockers.end());
		return Plan;
	}

} // namespace Durin::ContentBrowser::Private

namespace Durin::ContentBrowser::Private
{







}

namespace Durin::ContentBrowser::Private
{
	auto FContentBrowserRequestContext::PrepareDeleteItems(std::span<const FContentBrowserNativeItem> Items)
		-> FContentDeletionPlanPtr
	{
		if (!bAccepting) return nullptr;
		FAssetDeletionOperation AssetOperation;
		auto Plan = std::make_shared<FContentDeletionPlan>(*AnalyzeDeletion(Items, AssetOperation));
		Plan->SessionId = ++NextDeletionSession;
		DeletionSessions.emplace(Plan->SessionId, FDeletionSession{
			.Confirmation = Plan, .Request = {Items.begin(), Items.end()},
			.Execution = std::unique_ptr<FContentDeletionOperation>(new FContentDeletionOperation(Plan, std::move(AssetOperation)))});
		return Plan;
	}

	auto FContentBrowserRequestContext::DismissDeletion(FContentDeletionPlanPtr Confirmation) -> void
	{
		if (!Confirmation) return;
		const auto Found = DeletionSessions.find(Confirmation->SessionId);
		if (Found != DeletionSessions.end() && Found->second.Confirmation == Confirmation) DeletionSessions.erase(Found);
	}

	auto FContentBrowserRequestContext::DeleteItems(
		FContentDeletionPlanPtr& Confirmation, FContentDeletionHooks Hooks) -> bool
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Complete(Failure(EAssetWriteError::ReadOnlyMode, AdmissionError));
		if (!Confirmation) return Complete({EAssetWriteError::StaleData, "Deletion confirmation is unavailable."});
		const auto Found = DeletionSessions.find(Confirmation->SessionId);
		if (Found == DeletionSessions.end() || Found->second.Confirmation != Confirmation)
			return Complete({EAssetWriteError::StaleData, "Deletion confirmation was retired."});
		auto& Session = Found->second;
		if (!Confirmation->CanExecute()) return Complete({EAssetWriteError::InUse, "Deletion is blocked."});
		for (const auto& Root : Confirmation->MaximalRoots)
		{
			const auto Mount = FContentBrowserPaths::ResolveMountPath(Root.OriginalPath);
			if (!Mount || !Mount.Mount->bContentWritable
				|| AreSamePath(Root.OriginalPath, Mount.Mount->GetContentDir().generic_string()))
				return Complete({EAssetWriteError::ReadOnlyMode, "Deletion mount policy changed."});
			std::filesystem::path Reparse;
			std::error_code Error;
			// Check surviving ancestors even after a confirmed root was removed.
			auto Parent = std::filesystem::path(Root.OriginalPath).parent_path();
			while (!std::filesystem::exists(Parent, Error) && !Error
				&& Parent != Parent.parent_path()) Parent = Parent.parent_path();
			if (Error || FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Parent, Reparse, Error) || Error)
				return Complete({EAssetWriteError::InvalidPath, "Deletion ancestor changed or cannot be inspected."});
		}
		const bool bSucceeded = Session.Execution->Execute(std::move(Hooks));
		const auto Result = Session.Execution->GetResult();
		FContentChangeBatch Changes;
		if (!bSucceeded && !Session.Execution->HasStarted())
		{
			if (Result.Error != EAssetWriteError::StaleData) return Complete(Result);
			const auto Request = Session.Request;
			DeletionSessions.erase(Found);
			Confirmation = PrepareDeleteItems(Request);
			Report(ELogLevel::Warn, std::format("{} Review and confirm the updated deletion plan.", Result.Message));
			return false;
		}
		if (bSucceeded || Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending)
		{
			for (const auto& Entry : Confirmation->Entries)
				Changes.Changes.push_back({EContentChangeKind::Removed, Entry.PhysicalPath, {}, {}, {},
					Entry.Kind == EContentDeletionEntryKind::Directory});
			DeletionSessions.erase(Found);
		}
		else
		{
			// Retire failed destructive attempts. Partial changes need a fresh analysis.
			DeletionSessions.erase(Found);
			Changes.bFullRefresh = true;
		}
		const bool bRemoved = bSucceeded || Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending;
		if (!Result) Report(bRemoved ? ELogLevel::Warn : ELogLevel::Error, Result.Message);
		NotifyItemsUpdated(std::move(Changes));
		Confirmation.reset();
		return bRemoved;
	}
}

namespace Durin::ContentBrowser::Private
{
	auto FContentBrowserRequestContext::ValidateMoves(std::span<const FAssetRelocationMapping> Moves) const
		-> FAssetWriteResult
	{
		for (const auto& Move : Moves)
			for (const auto& Path : {Move.SourcePath, Move.DestinationPath})
			{
				const auto Mount = FContentBrowserPaths::ResolveMountPath(FContentBrowserPaths::VirtualToPhysical(Path.ToString() + ".dasset"));
				if (!Mount || !Mount.Mount->bContentWritable)
					return {EAssetWriteError::ReadOnlyMode, "Asset moves require writable browser content mounts."};
			}
		return {};
	}
}

namespace Durin::ContentBrowser::Private
{
	auto FContentBrowserRequestContext::ReportRecovery(const FAssetMutationResultDetails& Native) const -> void
	{
		for (const auto& Path : Native.BackupLocations) Report(ELogLevel::Warn, "Recovery backup: " + Path.generic_string());
		if (!Native.Result)
			for (const auto& Path : Native.AffectedFiles) Report(ELogLevel::Warn, "Affected file: " + Path.generic_string());
	}
	auto FContentBrowserRequestContext::Report(ELogLevel Level, std::string_view Message) const -> void
	{
		if (Diagnostics) Diagnostics(Level, Message);
		else FFeedbackContext::Get().Serialize(Level, Message);
	}
	auto FContentBrowserRequestContext::Feedback() const -> FFeedbackContext
	{ return FFeedbackContext({}, [this](ELogLevel Level, std::string_view Message) { Report(Level, Message); }); }

	auto FContentBrowserRequestContext::NotifyItemsUpdated(FContentChangeBatch Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		if (ItemUpdates) ItemUpdates(std::move(Changes));
	}
	auto FContentBrowserRequestContext::Complete(const FAssetWriteResult& Status, FContentChangeBatch Changes) -> bool
	{
		if (!Status) Report(ELogLevel::Error, Status.Message);
		NotifyItemsUpdated(std::move(Changes));
		return bool(Status);
	}
	auto FContentBrowserRequestContext::CompleteMutation(const FAssetMutationResultDetails& Native, FContentChangeBatch Changes) -> bool
	{
		ReportRecovery(Native);
		if (!Native.Result && Native.Result.Effect != EAssetWriteEffect::None) Changes.bFullRefresh = true;
		if (!Native.AffectedFiles.empty() && Changes.Changes.empty()) Changes.bFullRefresh = true;
		return Complete(Native.Result, std::move(Changes));
	}
	auto FContentBrowserRequestContext::CanModify(std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		if (!bAccepting) return SetQueryError(OutErrorMsg, "Content operations are stopping.");
		if (MutationAllowed && !MutationAllowed()) return SetQueryError(OutErrorMsg, "Content mutation is currently disabled.");
		return true;
	}
	auto FContentBrowserRequestContext::CanSaveItem(const FContentBrowserNativeItem& Item, EAssetSaveMode Mode, std::string* OutErrorMsg) const -> bool
	{
		if (!CanModify(OutErrorMsg)) return false;
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
	auto FContentBrowserRequestContext::CanDuplicateAsset(const FTopLevelAssetPath& Source, std::string* OutErrorMsg) const -> bool
	{
		if (!CanModify(OutErrorMsg)) return false;
		const auto Entry = FindAssetExact(Source);
		return Entry && !Entry->IsRedirector() ? true : SetQueryError(OutErrorMsg, "The copied source is no longer an available real asset.");
	}
	auto FContentBrowserRequestContext::CanDuplicateItem(const FContentBrowserNativeItem& Item, std::string* OutErrorMsg) const -> bool
	{
		FTopLevelAssetPath Source;
		if (Item.Kind != EContentBrowserItemKind::Asset || !FTopLevelAssetPath::TryCreate(Item.VirtualPath, Source))
			return SetQueryError(OutErrorMsg, "Duplication requires a real asset item.");
		return CanDuplicateAsset(Source, OutErrorMsg);
	}
	auto FContentBrowserRequestContext::CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		FContentBrowserNativeItem Candidate;
		const auto Result = ValidateFolder(Directory, Name, Candidate);
		return Result ? true : SetQueryError(OutErrorMsg, Result.Message);
	}
	auto FContentBrowserRequestContext::CanCopyItem(const FContentBrowserNativeItem& Item, std::string_view Directory, std::string* OutErrorMsg) const -> bool
	{
		using namespace ContentBrowserChanges;
		if (!CanModify(OutErrorMsg)) return false;
		const auto Source = FContentBrowserPaths::ResolveMountPath(Item.PhysicalPath);
		const auto Destination = FContentBrowserPaths::ResolveMountPath(Directory);
		if (!Source || !Destination || !Destination.Mount->bContentWritable
			|| SamePath(Item.PhysicalPath, Source.Mount->GetContentDir().generic_string())
			|| Item.Kind == EContentBrowserItemKind::Redirector)
			return SetQueryError(OutErrorMsg, "Copy requires an ordinary item and a writable mounted destination.");
		std::error_code Error;
		std::filesystem::path Reparse;
		if (!std::filesystem::is_directory(Directory, Error) || Error
			|| FindReparsePointInPath(Destination.Mount->GetContentDir().generic_string(), Directory, Reparse, Error) || Error)
			return SetQueryError(OutErrorMsg, "Copy destination must be an existing directory without reparse points.");
		if (Item.Kind == EContentBrowserItemKind::Asset) return CanDuplicateItem(Item, OutErrorMsg);
		const auto Status = std::filesystem::symlink_status(Item.PhysicalPath, Error);
		if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::exists(Status)
			|| (Item.Kind == EContentBrowserItemKind::Folder && Within(std::string(Directory), Item.PhysicalPath)))
			return SetQueryError(OutErrorMsg, "Copy source is unavailable or contains its destination.");
		return true;
	}
	auto FContentBrowserRequestContext::CanMoveItem(const FContentBrowserNativeItem& Item, std::string_view Destination, std::string* OutErrorMsg) const -> bool
	{
		if (!CanModify(OutErrorMsg)) return false;
		FContentMove Request;
		if (const auto Result = MakeMoveRequest(Item, Destination, std::nullopt, Request); !Result)
			return SetQueryError(OutErrorMsg, Result.Message);
		if (const auto Result = ValidateMoveRequest(Request); !Result) return SetQueryError(OutErrorMsg, Result.Message);
		return true;
	}
	auto FContentBrowserRequestContext::CanRenameItem(const FContentBrowserNativeItem& Item, std::string_view Name, std::string* OutErrorMsg) const -> bool
	{
		if (!CanModify(OutErrorMsg)) return false;
		FContentMove Request;
		if (const auto Result = MakeMoveRequest(Item, {}, Name, Request); !Result) return SetQueryError(OutErrorMsg, Result.Message);
		if (const auto Result = ValidateMoveRequest(Request); !Result) return SetQueryError(OutErrorMsg, Result.Message);
		return true;
	}
	auto FContentBrowserRequestContext::BulkSaveItems(std::span<const FContentBrowserNativeItem> Items, EAssetSaveMode Mode) -> bool
	{
		std::string Error;
		if (!CanModify(&Error)) return Complete({EAssetWriteError::ReadOnlyMode, Error});
		std::vector<FPackagePath> Packages;
		for (const auto& Item : Items)
		{
			if (Item.Kind != EContentBrowserItemKind::Asset || !Item.PackagePath.IsValid())
				return Complete(Failure(EAssetWriteError::InvalidData, "Save requires asset items."));
			Packages.push_back(Item.PackagePath);
		}
		std::ranges::sort(Packages, {}, &FPackagePath::ToString);
		Packages.erase(std::unique(Packages.begin(), Packages.end()), Packages.end());
		FContentChangeBatch Changes;
		auto Context = Feedback();
		const auto Revision = GetAssetQueryRevision();
		const bool bSaved = IAssetTools::Get().SaveAssets({.AssetPaths = Packages, .Mode = Mode,
			.OnSaved = [&](const FPackagePath& Path) {
				const auto Physical = FContentBrowserPaths::VirtualToPhysical(Path.ToString() + ".dasset");
				Changes.Changes.push_back({EContentChangeKind::Modified, Physical, Physical, Path.ToString(), Path.ToString()});
			}}, &Context);
		auto QueryChanges = CaptureAssetQueryChanges(Revision);
		if (QueryChanges.bFullRefresh || !QueryChanges.Changes.empty()) Changes = std::move(QueryChanges);
		NotifyItemsUpdated(std::move(Changes));
		return bSaved;
	}
	auto FContentBrowserRequestContext::RenameItem(const FContentBrowserNativeItem& Item, std::string_view Name, FContentBrowserNativeItem& OutNewItem) -> bool
	{
		OutNewItem = {};
		FContentChangeBatch Changes;
		const auto Status = RenameItemImpl(Item, Name, OutNewItem, Changes);
		if (Status) OutNewItem.Name = Name;
		return Complete(Status, std::move(Changes));
	}
	auto FContentBrowserRequestContext::CreateFolder(std::string_view Directory, std::string_view Name, FContentBrowserNativeItem& OutNewItem) -> bool
	{
		OutNewItem = {};
		FContentChangeBatch Changes;
		FContentBrowserNativeItem Candidate;
		const auto Status = ValidateFolder(Directory, Name, Candidate);
		if (!Status) return Complete(Status);
		std::error_code Error;
		if (!std::filesystem::create_directory(Candidate.PhysicalPath, Error) || Error)
			return Complete(Failure(EAssetWriteError::IoError, "Could not create folder: " + Error.message()));
		OutNewItem = std::move(Candidate);
		Changes.Changes.push_back({EContentChangeKind::Added, {}, OutNewItem.PhysicalPath, {}, {}, true});
		return Complete(Status, std::move(Changes));
	}
	auto FContentBrowserRequestContext::BulkCopyItems(std::span<const FContentBrowserNativeItem> Items, std::string_view Directory) -> bool
	{
		FContentChangeBatch Changes;
		const auto Status = CopyItemsImpl(Items, Directory, Changes);
		return Complete(Status, std::move(Changes));
	}
}
