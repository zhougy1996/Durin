#include "Operations/ContentBrowserOperationService.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Panels/ContentBrowserChanges.h"

#include "Asset/PackageSerialization.h"
#include "AssetTools/AssetDeletion.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"


namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		constexpr uint64 FnvOffset = 14695981039346656037ull;
		constexpr uint64 FnvPrime = 1099511628211ull;
		using ContentBrowserFilesystem::NormalizePath;

		auto Failure(EAssetWriteError Error, std::string Message)
			-> FContentBrowserOperationResult
		{
			return {{Error, std::move(Message)}};
		}

		auto HashAppend(uint64 Hash, std::string_view Value) -> uint64
		{
			for (const unsigned char Byte : Value)
			{
				Hash ^= Byte;
				Hash *= FnvPrime;
			}
			return Hash;
		}

		auto CalculateFingerprintDigest(
			const FContentDeletionFingerprint& Fingerprint) -> uint64
		{
			uint64 Hash = HashAppend(FnvOffset, Fingerprint.PhysicalPath);
			Hash = HashAppend(
				Hash, std::to_string(static_cast<uint8>(Fingerprint.Kind)));
			Hash = HashAppend(Hash, std::to_string(Fingerprint.FileSize));
			Hash = HashAppend(
				Hash, std::to_string(Fingerprint.LastWriteTimeTicks));
			Hash = HashAppend(Hash, std::to_string(Fingerprint.ByteIdentity.HashLow));
			return HashAppend(
				Hash, std::to_string(Fingerprint.ByteIdentity.HashHigh));
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
			OutFingerprint.Digest = CalculateFingerprintDigest(OutFingerprint);
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

	FContentBrowserOperationService::FContentBrowserOperationService(
		FContentBrowserPaths InPaths,
		FMoveAssets InMoveAssets,
		FRemoveDirectory InRemoveDirectory,
		FFixUpAssets InFixUpAssets,
		std::function<void()> InNotifyMountedContentMutation,
		std::function<bool()> InCanMutate, FContentBrowserAssetServices InAssets)
		: Assets(std::move(InAssets)), CanMutate(std::move(InCanMutate)), Paths(std::move(InPaths))
		, MoveAssets(std::move(InMoveAssets))
		, FixUpAssets(std::move(InFixUpAssets))
		, NotifyMountedContentMutation(std::move(InNotifyMountedContentMutation))
		, RemoveDirectory(std::move(InRemoveDirectory))
	{
		if (!MoveAssets) MoveAssets = [this](std::span<const FEditorAssetMove> Moves) {
			std::vector<FAssetRelocation> Mappings;
			for (const auto& Move : Moves) Mappings.push_back({Move.OldPath, Move.NewPath});
			return FContentBrowserOperationResult(Assets.RelocateAssets({.Mappings = std::move(Mappings)}));
		};
		if (!FixUpAssets) FixUpAssets = [this](std::span<const FPackagePath> Redirectors) {
			return FContentBrowserOperationResult(Assets.FixUpRedirectors(
				{.Redirectors = {Redirectors.begin(), Redirectors.end()}}));
		};
		if (!RemoveDirectory)
			RemoveDirectory = [](const std::filesystem::path& Path,
				std::error_code& Error) {
				return std::filesystem::remove(Path, Error);
			};
	}

	auto FContentBrowserOperationService::Rename(
		const FContentBrowserItem& Item,
		std::string_view NewName) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		if (NewName.empty() || NewName == "." || NewName == ".."
			|| NewName.find_first_of("/\\:*") != std::string_view::npos)
			return Failure(
				EAssetWriteError::InvalidPath,
				"The new name is empty or contains invalid path characters.");

		if (Item.Kind == EContentBrowserItemKind::Redirector)
			return Failure(
				EAssetWriteError::InvalidPath,
				"Redirectors cannot be renamed or moved directly. Fix Up the redirector or move its final asset.");

		const std::string_view CurrentName = Item.Kind == EContentBrowserItemKind::Asset
			? Item.PackagePath.GetPackageName() : std::string_view(Item.Name);
		if (NewName == CurrentName) return {};

		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const std::string OldPackagePath = Item.PackagePath.ToString();
			const size_t Slash = OldPackagePath.find_last_of('/');
			const FPackagePath& OldPath = Item.PackagePath;
			FPackagePath NewPath;
			if (!OldPath.IsValid() || Slash == std::string::npos
				|| !FPackagePath::TryCreate(
					OldPackagePath.substr(0, Slash + 1) + std::string(NewName),
					NewPath))
				return Failure(
					EAssetWriteError::InvalidPath,
					"The resulting asset path is invalid.");

			const FEditorAssetMove Move{OldPath, NewPath};
			if (const auto Allowed = ValidateMoves(std::span{&Move, 1}); !Allowed) return Allowed;
			const auto Before = FindAssetExact(OldPath);
			const FContentBrowserOperationResult Result =
				MoveAssets(std::span{&Move, 1});
			if (!Result) return Publish(Result);
			FContentBrowserOperationResult Outcome = Result;
			Outcome.FocusPhysicalPath =
				Paths.VirtualToPhysical(NewPath.ToString() + ".dasset");
			FTopLevelAssetPath OldAssetPath;
			FTopLevelAssetPath NewAssetPath;
			if (FTopLevelAssetPath::TryCreate(Item.VirtualPath, OldAssetPath)
				&& FTopLevelAssetPath::TryCreate(
					NewPath,
					OldAssetPath.GetAssetName(),
					NewAssetPath))
				Outcome.RevealAssetPath = NewAssetPath.ToString();
			if (Before)
				for (const auto& Asset : Before->TopLevelAssets)
				{
					FTopLevelAssetPath Destination;
					if (!FTopLevelAssetPath::TryCreate(NewPath, Asset.AssetPath.GetAssetName(), Destination))
					{ Outcome.Changes.bFullRefresh = true; continue; }
					Outcome.Changes.Changes.push_back({EContentChangeKind::Renamed, Item.PhysicalPath,
						Outcome.FocusPhysicalPath, Asset.AssetPath.ToString(), Destination.ToString()});
				}
			else Outcome.Changes.bFullRefresh = true;
			Outcome.bContentChanged = true;
		return Publish(std::move(Outcome));
		}

		if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			std::string Warning;
			return Publish(RenameFolder(Item, NewName, Warning));
		}

		FAssetCompanionOwnership Ownership;
		const FAssetWriteResult OwnershipResult =
			QueryAssetCompanionOwnership(Item.PhysicalPath, Ownership);
		if (!OwnershipResult)
			return Failure(
				OwnershipResult.Error,
				std::format(
					"Could not determine whether this file is asset-managed: {}",
					OwnershipResult.Message));
		if (Ownership.State == EAssetCompanionOwnershipState::Ambiguous)
			return Failure(
				EAssetWriteError::InUse,
				"This file is claimed by multiple assets. Resolve companion ownership before renaming it.");
		if (Ownership.State == EAssetCompanionOwnershipState::Owned)
			return Failure(
				EAssetWriteError::InUse,
				std::format(
					"This file is managed by {}. Rename or move the owning asset instead.",
					Ownership.Owners.front().ToString()));

		std::filesystem::path Destination =
			std::filesystem::path(Item.PhysicalPath).parent_path()
			/ std::filesystem::path(NewName);
		if (Destination.extension().empty()) Destination += Item.Extension;
		const FContentBrowserPaths::FMountPath SourceMount =
			Paths.ResolveMountPath(Item.PhysicalPath);
		const FContentBrowserPaths::FMountPath DestinationMount =
			Paths.ResolveMountPath(Destination.generic_string());
		if (!SourceMount || !DestinationMount
			|| SourceMount.Mount != DestinationMount.Mount)
			return Failure(
				EAssetWriteError::InvalidPath,
				"File renames must stay inside the same automatically scanned content mount.");
		if (!SourceMount.Mount->bContentWritable)
			return Failure(
				EAssetWriteError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before renaming the file.");
		const ContentBrowserFilesystem::FPathProbe DestinationProbe =
			ContentBrowserFilesystem::Probe(Destination);
		if (DestinationProbe.Error)
			return Failure(
				EAssetWriteError::IoError,
				std::format("Could not inspect the rename destination: {}", DestinationProbe.Error.message()));
		if (DestinationProbe.Exists())
			return Failure(
				EAssetWriteError::InvalidPath,
				"An item with that name already exists.");

		std::error_code Ec;
		std::filesystem::rename(Item.PhysicalPath, Destination, Ec);
		if (Ec)
			return Failure(
				EAssetWriteError::IoError,
				std::format("Rename failed: {}", Ec.message()));
		FContentBrowserOperationResult Outcome;
		Outcome.Changes.Changes.push_back({EContentChangeKind::Renamed, Item.PhysicalPath,
			NormalizePath(Destination.generic_string())});
		Outcome.FocusPhysicalPath = NormalizePath(Destination.generic_string());
		Outcome.bContentChanged = true;
		return Publish(std::move(Outcome));
	}

	auto FContentBrowserOperationService::Duplicate(
		const FContentBrowserItem& Item) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		if (Item.Kind != EContentBrowserItemKind::Asset)
			return Failure(
				EAssetWriteError::InvalidData,
				"Only real assets can be duplicated.");
		FTopLevelAssetPath SourcePath;
		if (!FTopLevelAssetPath::TryCreate(Item.VirtualPath, SourcePath))
			return Failure(EAssetWriteError::InvalidPath,
				"The source top-level asset path is invalid.");
		if (!SourcePath.IsValid())
			return Failure(
				EAssetWriteError::InvalidPath,
				"The source asset path is invalid.");
		const std::string SourcePackagePath = SourcePath.GetPackagePath().ToString();
		const size_t Slash = SourcePackagePath.find_last_of('/');
		if (Slash == std::string::npos)
			return Failure(
				EAssetWriteError::InvalidPath,
				"The source asset has no valid destination directory.");
		return Duplicate(SourcePath, SourcePackagePath.substr(0, Slash + 1));
	}

	auto FContentBrowserOperationService::Duplicate(
		const FTopLevelAssetPath& SourcePath,
		std::string_view DestinationDirectory)
		-> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		if (!SourcePath.IsValid() || DestinationDirectory.empty())
			return Failure(
				EAssetWriteError::InvalidPath,
				"Asset paste requires a valid source and destination folder.");
		std::string Directory(DestinationDirectory);
		if (!Directory.ends_with('/')) Directory.push_back('/');
		if (const auto Available = QueryDuplicate(SourcePath); !Available) return Available;
		const auto SourceData = FindTopLevelAssetExact(SourcePath);
		const std::string DestinationDirectoryPhysical =
			Paths.VirtualToPhysical(Directory);
		const FContentBrowserPaths::FMountPath Mount =
			Paths.ResolveMountPath(DestinationDirectoryPhysical);
		if (!Mount)
			return Failure(
				EAssetWriteError::InvalidPath,
				"The paste destination is outside an automatically scanned content mount.");
		if (!Mount.Mount->bContentWritable)
			return Failure(
				EAssetWriteError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before pasting the asset.");

		const FAssetOperationResult Result = Assets.DuplicateAsset({
			.SourcePath = SourcePath,
			.DestinationDirectory = Directory,
			.ResolvePhysicalPackagePath = [this](const FPackagePath& Path) {
				return Paths.VirtualToPhysical(Path.ToString() + ".dasset");
			}});
		FContentBrowserOperationResult Outcome(Result);
		if (!Result || !Result.Asset) return Publish(std::move(Outcome));
		Outcome.Changes.Changes.push_back({EContentChangeKind::Added, {}, Result.PhysicalPath});
		Outcome.FocusPhysicalPath = Result.PhysicalPath;
		Outcome.RevealAssetPath = Result.Asset->GetObjectPath();
		Outcome.OpenAssetClassName = SourceData->AssetClassName;
		Outcome.bContentChanged = true;
		return Publish(std::move(Outcome));
	}

	auto FContentBrowserOperationService::CopyItems(
		std::span<const FContentBrowserItem> Items, std::string_view PhysicalDirectory)
		-> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		using namespace ContentBrowserChanges;
		const auto Destination = Paths.ResolveMountPath(PhysicalDirectory);
		if (!Destination || !Destination.Mount->bContentWritable)
			return {EAssetWriteError::ReadOnlyMode, "Paste requires a writable content folder."};
		std::error_code Ec;
		std::filesystem::path Reparse;
		if (!std::filesystem::is_directory(PhysicalDirectory, Ec) || Ec
			|| IsReparsePoint(Destination.Mount->PhysicalRoot, Ec) || Ec
			|| FindReparsePointInPath(Destination.Mount->PhysicalRoot, PhysicalDirectory, Reparse, Ec) || Ec)
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
			const auto SourceMount = Paths.ResolveMountPath(Source);
			if (!SourceMount || SamePath(Source, SourceMount.Mount->PhysicalRoot))
				return {EAssetWriteError::InvalidPath, "Copy sources must be items inside a scanned content mount."};
			if (Item.Kind == EContentBrowserItemKind::Redirector)
				return {EAssetWriteError::InvalidPath, "Redirectors cannot be copied."};
			if (IsReparsePoint(SourceMount.Mount->PhysicalRoot, Ec) || Ec
				|| FindReparsePointInPath(SourceMount.Mount->PhysicalRoot, Source, Reparse, Ec) || Ec)
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
				const auto EntryMount = Paths.ResolveMountPath(Path.generic_string());
				const auto TargetMount = Paths.ResolveMountPath(To.generic_string());
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
					if (Data.EntryKind == EAssetRegistryEntryKind::Redirector || Data.TopLevelAssets.size() != 1)
						return {EAssetWriteError::InvalidData, "Copy requires a real package with one top-level asset."};
					const auto Asset = Data.TopLevelAssets.front().AssetPath;
					if (const auto Available = QueryDuplicate(Asset); !Available) return Available;
					AssetCopies.push_back({Asset, Paths.PhysicalToVirtualDirectory(To.parent_path().generic_string())});
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
		auto FailCopy = [&](FContentBrowserOperationResult Result) {
			if (!bAssetEffects)
			{
				for (auto It = CreatedFiles.rbegin(); It != CreatedFiles.rend(); ++It)
				{
					std::filesystem::remove(*It, Ec);
					if (Ec) Result.Warning += " Could not remove " + It->generic_string() + ": " + Ec.message();
				}
				for (auto It = CreatedDirectories.rbegin(); It != CreatedDirectories.rend(); ++It)
				{
					std::filesystem::remove(*It, Ec);
					if (Ec) Result.Warning += " Could not remove " + It->generic_string() + ": " + Ec.message();
				}
			}
			else Result.Warning += " Earlier copies were retained because asset publication has already started. Inspect the destination before retrying.";
			Result.bContentChanged = bAssetEffects || !Result.Warning.empty();
			Result.Changes.bFullRefresh = Result.bContentChanged;
			if (Result.bContentChanged) Result.Status.Effect = EAssetWriteEffect::ContentUncertain;
			return Publish(std::move(Result));
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
				FContentBrowserOperationResult Result{EAssetWriteError::IoError,
					"Could not copy " + File.Source.generic_string() + ": " + Ec.message()};
				// A failed copy may leave a partial destination; do not delete a competing writer's file.
				Result.Warning = " Inspect failed destination: " + File.Target.generic_string();
				return FailCopy(std::move(Result));
			}
			CreatedFiles.push_back(File.Target);
		}
		FContentBrowserOperationResult Outcome;
		for (const auto& Copy : AssetCopies)
		{
			auto Result = Assets.DuplicateAsset({.SourcePath = Copy.Source, .DestinationDirectory = Copy.Directory,
				.ResolvePhysicalPackagePath = [this](const FPackagePath& Path) {
					return Paths.VirtualToPhysical(Path.ToString() + ".dasset");
				}});
			bAssetEffects |= static_cast<bool>(Result)
				|| Result.State == EAssetOperationTerminalState::ContentCommittedProjectionPending
				|| Result.State == EAssetOperationTerminalState::ContentUncertain
				|| Result.State == EAssetOperationTerminalState::PartiallyWritten;
			if (Result.State != EAssetOperationTerminalState::Completed)
			{
				Result.Message = "Copy stopped at " + Copy.Source.ToString() + " into " + Copy.Directory + ": " + Result.Message;
				return FailCopy(FContentBrowserOperationResult(std::move(Result)));
			}
			for (const auto& Warning : Result.Warnings) Outcome.Warning += Warning.Details + "\n";
			Outcome.FocusPhysicalPath = Result.PhysicalPath;
			Outcome.Changes.Changes.push_back({EContentChangeKind::Added, {}, Result.PhysicalPath});
		}
		for (const auto& Path : CreatedFiles)
			Outcome.Changes.Changes.push_back({EContentChangeKind::Added, {}, Path.generic_string()});
		for (const auto& Path : CreatedDirectories)
			Outcome.Changes.Changes.push_back({EContentChangeKind::Added, {}, Path.generic_string(), {}, {}, true});
		if (Outcome.FocusPhysicalPath.empty() && !Targets.empty()) Outcome.FocusPhysicalPath = Targets.front();
		Outcome.bContentChanged = !Outcome.Changes.Changes.empty();
		return Publish(std::move(Outcome));
	}

	auto FContentBrowserOperationService::RenameFolder(
		const FContentBrowserItem& Item, std::string_view NewName,
		std::string& OutWarning) -> FContentBrowserOperationResult
	{
		const FContentMove Request{Item,
			(std::filesystem::path(Item.PhysicalPath).parent_path() / NewName).generic_string()};
		auto Result = MoveContent(std::span{&Request, 1});
		OutWarning = Result.Warning;
		return Result;
	}

	// All physical paths and package mappings are collected before touching disk.
	auto FContentBrowserOperationService::MoveItems(
		std::span<const FContentBrowserItem> Items, std::string_view PhysicalDirectory)
		-> FContentBrowserOperationResult
	{
		std::vector<FContentMove> Requests;
		for (const auto& Item : Items)
			Requests.push_back({Item, (std::filesystem::path(PhysicalDirectory)
				/ std::filesystem::path(Item.PhysicalPath).filename()).generic_string()});
		return Publish(MoveContent(Requests));
	}

	auto FContentBrowserOperationService::MoveContent(std::span<const FContentMove> Requests)
		-> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		using namespace ContentBrowserChanges;
		std::vector<FContentMove> Roots;
		for (auto Request : Requests)
		{
			Request.Item.PhysicalPath = NormalizePath(Request.Item.PhysicalPath);
			Request.Destination = NormalizePath(Request.Destination);
			if (SamePath(Request.Item.PhysicalPath, Request.Destination)) continue;
			const bool Covered = std::ranges::any_of(Requests, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder
					&& !SamePath(Other.Item.PhysicalPath, Request.Item.PhysicalPath)
					&& Within(Request.Item.PhysicalPath, Other.Item.PhysicalPath);
			});
			if (Covered) continue;
			if (std::ranges::any_of(Roots, [&](const auto& Other) {
				return SamePath(Other.Item.PhysicalPath, Request.Item.PhysicalPath);
			})) continue;
			Roots.push_back(std::move(Request));
		}
		if (Roots.empty()) return {};
		struct FPhysicalMove { std::filesystem::path Source; std::filesystem::path Destination; };
		std::vector<FPhysicalMove> Files;
		std::vector<std::filesystem::path> Directories;
		std::vector<std::filesystem::path> OldDirectories;
		std::vector<FEditorAssetMove> AssetMoves;
		struct FCompanion { std::string Path; FAssetCompanionOwnership Ownership; bool bIndependent; };
		std::vector<FCompanion> Companions;
		const auto Catalog = CaptureAssetCatalogSnapshot();
		FContentBrowserOperationResult Outcome;
		std::error_code Ec;
		for (const auto& Root : Roots)
		{
			const auto& Source = Root.Item.PhysicalPath;
			const auto& Destination = Root.Destination;
			const auto SourceMount = Paths.ResolveMountPath(Source);
			const auto DestinationMount = Paths.ResolveMountPath(Destination);
			if (!SourceMount || !DestinationMount || SourceMount.Mount != DestinationMount.Mount)
				return {EAssetWriteError::InvalidPath, "Content moves must stay inside the same automatically scanned content mount."};
			if (!SourceMount.Mount->bContentWritable)
				return {EAssetWriteError::ReadOnlyMode, "Content moves require a writable content mount."};
			if (SamePath(Source, SourceMount.Mount->PhysicalRoot))
				return {EAssetWriteError::InvalidPath, "Content mount roots cannot be moved."};
			for (const auto& Other : Roots)
			{
				if (Within(Destination, Other.Item.PhysicalPath))
					return {EAssetWriteError::InvalidPath, "A destination cannot be inside a selected source."};
				if (&Other != &Root && SamePath(Destination, Other.Destination))
					return {EAssetWriteError::AlreadyExists, "Selected items have the same destination."};
			}
			const auto Probe = ContentBrowserFilesystem::Probe(Destination);
			if (Probe.Error) return {EAssetWriteError::IoError, Probe.Error.message()};
			if (Probe.Exists()) return {EAssetWriteError::AlreadyExists, "An item with that name already exists."};
			const auto Parent = std::filesystem::path(Destination).parent_path();
			if (!std::filesystem::is_directory(Parent, Ec) || Ec)
				return {EAssetWriteError::InvalidPath, "The destination directory does not exist."};
			std::filesystem::path Reparse;
			if (IsReparsePoint(SourceMount.Mount->PhysicalRoot, Ec) || Ec
				|| FindReparsePointInPath(SourceMount.Mount->PhysicalRoot, Source, Reparse, Ec) || Ec
				|| FindReparsePointInPath(SourceMount.Mount->PhysicalRoot, Parent, Reparse, Ec) || Ec)
				return {EAssetWriteError::InvalidPath, "Content moves cannot traverse reparse points or unreadable paths."};
			const bool bFolder = Root.Item.Kind == EContentBrowserItemKind::Folder;
			if (std::filesystem::is_directory(Source, Ec) != bFolder || Ec)
				return {EAssetWriteError::InvalidPath, "The source content type has changed."};
			auto Collect = [&](const std::filesystem::path& Path, const std::filesystem::path& Target,
				bool bIndependent) -> FAssetWriteResult {
				if (IsReparsePoint(Path, Ec) || Ec)
					return {EAssetWriteError::InvalidPath, "Content moves cannot include reparse points or unreadable entries."};
				const auto EntryMount = Paths.ResolveMountPath(Path.generic_string());
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
					if (Data.EntryKind == EAssetRegistryEntryKind::Redirector)
						return {EAssetWriteError::InvalidPath, "Fix Up redirectors before moving this content."};
					FPackagePath NewPath;
					auto Virtual = Paths.ResolveMountPath(Target.generic_string()).VirtualPath;
					if (!Virtual.ends_with(".dasset"))
						return {EAssetWriteError::InvalidPath, "The destination package extension is invalid."};
					Virtual.resize(Virtual.size() - 7);
					if (!FPackagePath::TryCreate(Virtual, NewPath))
						return {EAssetWriteError::InvalidPath, "The destination asset path is invalid."};
					if (FindAssetExact(NewPath) || FindResidentPackage(NewPath))
						return {EAssetWriteError::AlreadyExists, "The destination asset path is already occupied."};
					AssetMoves.push_back({Package, NewPath});
					for (const auto& Asset : Data.TopLevelAssets)
					{
						FTopLevelAssetPath NewAsset;
						if (FTopLevelAssetPath::TryCreate(NewPath, Asset.AssetPath.GetAssetName(), NewAsset))
							Outcome.Changes.Changes.push_back({EContentChangeKind::Renamed, Physical,
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
			Outcome.Changes.Changes.push_back({EContentChangeKind::Renamed, Source, Destination,
				bFolder ? Paths.PhysicalToVirtualDirectory(Source) : std::string{},
				bFolder ? Paths.PhysicalToVirtualDirectory(Destination) : std::string{}, bFolder});
		}
		for (const auto& Companion : Companions)
		{
			if (Companion.bIndependent || Companion.Ownership.State == EAssetCompanionOwnershipState::Ambiguous
				|| std::ranges::any_of(Companion.Ownership.Owners, [&](const auto& Owner) {
					return std::ranges::none_of(AssetMoves, [&](const auto& Move) { return Move.OldPath == Owner; });
				}))
				return {EAssetWriteError::InUse, "Move the owning asset instead of its managed companion: " + Companion.Path};
		}
		if (const auto Allowed = ValidateMoves(AssetMoves); !Allowed) return Allowed;
		std::vector<std::filesystem::path> CreatedDirectories;
		size_t MovedFiles = 0;
		auto RollBack = [&](FContentBrowserOperationResult Result) {
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
				Result.Status.Message += Errors;
				Result.Warning += Errors;
				Result.Status.Effect = EAssetWriteEffect::ContentUncertain;
				if (Result.AssetResult)
				{
					Result.AssetResult->State = EAssetOperationTerminalState::ContentUncertain;
					Result.AssetResult->Message += Errors;
				}
				Result.bContentChanged = true;
			}
			Result.Changes = {};
			Result.Changes.bFullRefresh = Result.bContentChanged;
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
			auto Result = MoveAssets(AssetMoves);
			const bool bCommitted = Result.Status.Effect == EAssetWriteEffect::ContentCommittedProjectionPending
				|| (Result.AssetResult && Result.AssetResult->State == EAssetOperationTerminalState::ContentCommittedProjectionPending);
			if (!Result && !bCommitted)
			{
				// AssetTools owns partial-effect diagnostics and backups. Never attempt inverse relocation.
				Result.bContentChanged = true;
				return RollBack(std::move(Result));
			}
			Outcome.AssetResult = std::move(Result.AssetResult);
			Outcome.Status = std::move(Result.Status);
			Outcome.Warning = std::move(Result.Warning);
			if (bCommitted) Outcome.Changes.bFullRefresh = true;
		}
		std::ranges::sort(OldDirectories, [](const auto& A, const auto& B) { return A.native().size() > B.native().size(); });
		for (const auto& Directory : OldDirectories)
		{
			Ec.clear();
			if (!RemoveDirectory(Directory, Ec) || Ec)
				Outcome.Warning = "Content was moved successfully, but a source folder could not be removed (redirectors or cleanup failed): "
					+ Directory.generic_string();
		}
		Outcome.FocusPhysicalPath = Roots.front().Destination;
		Outcome.bContentChanged = true;
		return Outcome;
	}

	auto FContentBrowserOperationService::CreateFolder(
		std::string_view PhysicalDirectory) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		const std::string NormalizedDirectory = NormalizePath(PhysicalDirectory);
		const FContentBrowserPaths::FMountPath DirectoryMount =
			Paths.ResolveMountPath(NormalizedDirectory);
		if (!DirectoryMount)
			return Failure(
				EAssetWriteError::InvalidPath,
				"Folders can only be created inside an automatically scanned content mount.");
		if (!DirectoryMount.Mount->bContentWritable)
			return Failure(
				EAssetWriteError::ReadOnlyMode,
				"This content mount is read-only. Choose a writable mount before creating a folder.");

		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0
				? "New Folder"
				: std::format("New Folder ({})", Suffix + 1);
			const std::filesystem::path Path =
				std::filesystem::path(NormalizedDirectory) / Name;
			const ContentBrowserFilesystem::FPathProbe CandidateProbe =
				ContentBrowserFilesystem::Probe(Path);
			if (CandidateProbe.Error)
				return Failure(
					EAssetWriteError::IoError,
					std::format("Could not inspect a folder candidate: {}", CandidateProbe.Error.message()));
			if (CandidateProbe.Exists()) continue;
			const FContentBrowserPaths::FMountPath DestinationMount =
				Paths.ResolveMountPath(Path.generic_string());
			if (!DestinationMount || DestinationMount.Mount != DirectoryMount.Mount)
				return Failure(
					EAssetWriteError::InvalidPath,
					"The new folder would be outside its automatically scanned content mount.");
			std::error_code Ec;
			if (!std::filesystem::create_directory(Path, Ec) || Ec)
				return Failure(
					EAssetWriteError::IoError,
					std::format("Could not create folder: {}", Ec.message()));
			FContentBrowserOperationResult Outcome;
			Outcome.Changes.Changes.push_back({EContentChangeKind::Added, {}, NormalizePath(Path.generic_string()), {}, {}, true});
			Outcome.FocusPhysicalPath = NormalizePath(Path.generic_string());
			Outcome.bContentChanged = true;
			return Publish(std::move(Outcome));
		}
		return Failure(
			EAssetWriteError::AlreadyExists,
			"Could not find a unique folder name in this directory.");
	}

	auto FContentBrowserOperationService::Move(std::span<const FEditorAssetMove> Moves)
		-> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		std::vector<FEditorAssetMove> EffectiveMoves;
		for (const auto& Move : Moves)
			if (Move.OldPath != Move.NewPath) EffectiveMoves.push_back(Move);
		if (EffectiveMoves.empty()) return {};
		if (const auto Allowed = ValidateMoves(EffectiveMoves); !Allowed) return Allowed;
		const auto Before = CaptureAssetCatalogSnapshot();
		auto Result = MoveAssets(EffectiveMoves);
		if (Result)
			for (const auto& Move : EffectiveMoves)
			{
				const auto* Package = Before.FindExact(Move.OldPath);
				if (!Package) { Result.Changes.bFullRefresh = true; continue; }
				for (const auto& Asset : Package->TopLevelAssets)
				{
					FTopLevelAssetPath NewAsset;
					if (!FTopLevelAssetPath::TryCreate(Move.NewPath, Asset.AssetPath.GetAssetName(), NewAsset))
					{ Result.Changes.bFullRefresh = true; continue; }
					Result.Changes.Changes.push_back({EContentChangeKind::Renamed, Package->PhysicalPath,
						Paths.VirtualToPhysical(Move.NewPath.ToString() + ".dasset"), Asset.AssetPath.ToString(), NewAsset.ToString()});
				}
			}
		return Publish(std::move(Result));
	}

	auto FContentBrowserOperationService::CollectRedirectors(
		std::string_view VirtualDirectory) const -> std::vector<FPackagePath>
	{
		std::string Prefix(VirtualDirectory);
		if (!Prefix.empty() && !Prefix.ends_with('/')) Prefix += '/';
		std::vector<FPackagePath> Redirectors;
		for (const auto& [Path, Data]
			: CaptureAssetCatalogSnapshot().Assets)
		{
			if (Data.EntryKind != EAssetRegistryEntryKind::Redirector)
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

	auto FContentBrowserOperationService::FixUpRedirectorsInFolder(
		std::string_view VirtualDirectory) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		if (VirtualDirectory.empty())
			return {
				EAssetWriteError::InvalidPath,
				"Fix Up in Folder requires a mounted virtual directory."};
		const std::vector<FPackagePath> Redirectors =
			CollectRedirectors(VirtualDirectory);
		if (Redirectors.empty()) return {};
		return Publish(FixUpAssets(Redirectors));
	}

	auto FContentBrowserOperationService::FixUpRedirectors(
		std::span<const FPackagePath> Redirectors) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		if (Redirectors.empty()) return {};
		return Publish(FixUpAssets(Redirectors));
	}

	auto FContentBrowserOperationService::FixUpAllRedirectors()
		-> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		Paths.RefreshMountSnapshot();
		const std::vector<FPackagePath> Redirectors = CollectRedirectors("/");
		return Redirectors.empty() ? FContentBrowserOperationResult{}
			: Publish(FixUpAssets(Redirectors));
	}

	auto FContentBrowserOperationService::AnalyzeDeletion(
		std::span<const FContentBrowserItem> Items,
		FAssetDeletionOperation& OutOperation) const
		-> FContentDeletionPlanPtr
	{
		auto Plan = std::make_shared<FContentDeletionPlan>();
		Plan->RegistryRevision = GetAssetCatalogRevision();

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

		Paths.RefreshMountSnapshot();
		struct FSelectedRoot
		{
			const FContentBrowserItem* Item = nullptr;
			std::string PhysicalPath;
			const FContentBrowserPaths::FMountSnapshot* Mount = nullptr;
		};
		std::vector<FSelectedRoot> SelectedRoots;
		for (const FContentBrowserItem& Item : Items)
		{
			const std::string PhysicalPath = NormalizePath(Item.PhysicalPath);
			const FContentBrowserPaths::FMountPath Resolved =
				Paths.ResolveMountPath(PhysicalPath);
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
			const FContentBrowserPaths::FMountSnapshot* Mount = Resolved.Mount;
			if (AreSamePath(PhysicalPath, Mount->PhysicalRoot))
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
		std::unordered_map<std::string, const FAssetData*> AssetsByPhysicalPath;
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
				.OriginalPath = Root.PhysicalPath,
				.Kind = bDirectory
					? EContentDeletionEntryKind::Directory
					: EContentDeletionEntryKind::OrdinaryFile});
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
		const FAssetOperationResult AssetResult = Assets.PrepareDeletion({
			.AssetPaths = AssetPaths, .PhysicalRoots = PhysicalRoots},
			OutOperation);
		if (!AssetResult && OutOperation.GetBlockers().empty())
			AddBlocker(
				EContentDeletionBlocker::InspectionFailed,
				"Assets", {}, {}, AssetResult.Message);

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

			const FContentBrowserPaths::FMountSnapshot* SelectedMount =
				MaximalRoots.empty() ? nullptr : MaximalRoots.front().Mount;
			std::filesystem::path SelectedReparsePoint;
			if (SelectedMount
				&& FPaths::IsLexicalDescendantPath(
					CompanionPath, SelectedMount->PhysicalRoot, true)
				&& FindReparsePointInPath(
					SelectedMount->PhysicalRoot,
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
				Paths.ResolveMountPath(CompanionPath);
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
			const FContentBrowserPaths::FMountSnapshot* CompanionMount = Resolved.Mount;
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
				CompanionMount->PhysicalRoot, CompanionPath, ReparsePoint, Ec);
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
				.OriginalPath = CompanionPath,
				.Kind = EContentDeletionEntryKind::ManagedCompanion,
				.Fingerprint = Fingerprint});
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
		for (FContentDeletionFingerprint& Entry : Plan->Entries)
			Entry.Digest = CalculateFingerprintDigest(Entry);
		for (FContentDeletionFingerprint& Directory : Plan->Entries)
		{
			if (Directory.Kind != EContentDeletionEntryKind::Directory) continue;
			uint64 Digest = FnvOffset;
			FXxHash128Builder ByteIdentity;
			for (const FContentDeletionFingerprint& Descendant : Plan->Entries)
			{
				if (!FPaths::IsLexicalDescendantPath(
						Descendant.PhysicalPath, Directory.PhysicalPath, true))
					continue;
				const std::string Relative = std::filesystem::path(
					Descendant.PhysicalPath).lexically_relative(
						Directory.PhysicalPath).generic_string();
				Digest = HashAppend(Digest, Relative);
				Digest = HashAppend(Digest, std::to_string(
					static_cast<uint8>(Descendant.Kind)));
				Digest = HashAppend(Digest, std::to_string(Descendant.FileSize));
				Digest = HashAppend(
					Digest, std::to_string(Descendant.LastWriteTimeTicks));
				ByteIdentity.Update(Relative);
				ByteIdentity.UpdateValue(Descendant.Kind);
				ByteIdentity.UpdateValue(Descendant.FileSize);
				ByteIdentity.UpdateValue(Descendant.LastWriteTimeTicks);
				ByteIdentity.UpdateValue(Descendant.ByteIdentity.HashLow);
				ByteIdentity.UpdateValue(Descendant.ByteIdentity.HashHigh);
			}
			Directory.ByteIdentity = ByteIdentity.Finalize();
			Directory.Digest = HashAppend(
				HashAppend(Digest, std::to_string(Directory.ByteIdentity.HashLow)),
				std::to_string(Directory.ByteIdentity.HashHigh));
		}
		for (FContentDeletionRoot& Root : Plan->MaximalRoots)
		{
			const auto Fingerprint = std::ranges::find(
				Plan->Entries,
				Root.OriginalPath,
				&FContentDeletionFingerprint::PhysicalPath);
			if (Fingerprint != Plan->Entries.end())
			{
				Root.Kind = Fingerprint->Kind;
				Root.Fingerprint = *Fingerprint;
			}
		}
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

	auto FContentBrowserOperationService::IsDeletionPlanCurrent(
		const FContentDeletionPlan& Plan) const -> bool
	{
		const auto Found = DeletionSessions.find(Plan.SessionId);
		return Found != DeletionSessions.end()
			&& Found->second.Confirmation.get() == &Plan
			&& Found->second.Execution->IsConfirmationCurrent();
	}
} // namespace Durin::Editor::ContentBrowser::Private

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserOperationService::QueryMutation() const -> FAssetWriteResult
	{
		if (!bAccepting) return {EAssetWriteError::ShuttingDown, "Content operations are stopping."};
		if (CanMutate && !CanMutate()) return {EAssetWriteError::ReadOnlyMode, "Content mutation is currently disabled."};
		return {};
	}

	auto FContentBrowserOperationService::QuerySave(const FPackagePath& Path) const -> FAssetWriteResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		const auto* Package = FindResidentPackage(Path);
		return Package && Package->IsDirty() ? FAssetWriteResult{}
			: FAssetWriteResult{EAssetWriteError::InUse, "Save requires a resident dirty package."};
	}

	auto FContentBrowserOperationService::Publish(FContentBrowserOperationResult Result)
		-> FContentBrowserOperationResult
	{
		if (Result.AssetResult)
		{
			const auto& Asset = *Result.AssetResult;
			for (const auto& Warning : Asset.Warnings)
			{
				if (!Result.Warning.empty()) Result.Warning += "\n";
				Result.Warning += Warning.Details;
			}
			Result.bContentChanged |= !Asset.AffectedAssets.empty()
				&& (Asset.State == EAssetOperationTerminalState::Completed
					|| Asset.State == EAssetOperationTerminalState::PartiallyWritten
					|| Asset.Persistence == EAssetOperationPersistenceState::PartiallyPersisted
					|| Asset.State == EAssetOperationTerminalState::ContentCommittedProjectionPending);
		}
		if (Result.bContentChanged)
		{
			// AffectedAssets is not a committed-path ledger on partial failure.
			if (Result.AssetResult && Result.AssetResult->State != EAssetOperationTerminalState::Completed)
				Result.Changes.bFullRefresh = true;
			if (Result.Changes.Changes.empty() && Result.AssetResult && !Result.Changes.bFullRefresh)
				for (const auto& Path : Result.AssetResult->AffectedAssets)
					Result.Changes.Changes.push_back({EContentChangeKind::Modified,
						Paths.VirtualToPhysical(Path.ToString() + ".dasset"),
						Paths.VirtualToPhysical(Path.ToString() + ".dasset"), Path.ToString(), Path.ToString()});
			if (Result.Changes.Changes.empty()) Result.Changes.bFullRefresh = true;
			if (NotifyScopedContentMutation) NotifyScopedContentMutation(Result.Changes);
			else if (NotifyMountedContentMutation) NotifyMountedContentMutation();
		}
		return Result;
	}

	auto FContentBrowserOperationService::Save(std::vector<FPackagePath> Packages, EAssetSaveMode Mode)
		-> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		std::ranges::sort(Packages, {}, &FPackagePath::ToString);
		Packages.erase(std::unique(Packages.begin(), Packages.end()), Packages.end());
		return Publish(Assets.SaveAssets({.AssetPaths = std::move(Packages), .Mode = Mode}));
	}
}

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserOperationService::BuildDeletionPlan(std::span<const FContentBrowserItem> Items)
		-> FContentDeletionPlanPtr
	{
		FAssetDeletionOperation AssetOperation;
		auto Plan = std::make_shared<FContentDeletionPlan>(*AnalyzeDeletion(Items, AssetOperation));
		Plan->SessionId = ++NextDeletionSession;
		DeletionSessions.emplace(Plan->SessionId, FDeletionSession{
			.Confirmation = Plan, .Request = {Items.begin(), Items.end()},
			.Execution = std::make_unique<FContentDeletionOperation>(Plan, std::move(AssetOperation))});
		return Plan;
	}

	auto FContentBrowserOperationService::DismissDeletion(FContentDeletionPlanPtr Confirmation) -> void
	{
		if (!Confirmation) return;
		const auto Found = DeletionSessions.find(Confirmation->SessionId);
		if (Found != DeletionSessions.end() && Found->second.Confirmation == Confirmation) DeletionSessions.erase(Found);
	}

	auto FContentBrowserOperationService::ExecuteDeletion(
		FContentDeletionPlanPtr Confirmation, FContentDeletionHooks Hooks) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		if (!Confirmation) return {EAssetWriteError::StaleData, "Deletion confirmation is unavailable."};
		const auto Found = DeletionSessions.find(Confirmation->SessionId);
		if (Found == DeletionSessions.end() || Found->second.Confirmation != Confirmation)
			return {EAssetWriteError::StaleData, "Deletion confirmation was retired."};
		auto& Session = Found->second;
		if (!Confirmation->CanExecute()) return {EAssetWriteError::InUse, "Deletion is blocked."};
		Paths.RefreshMountSnapshot();
		for (const auto& Root : Confirmation->MaximalRoots)
		{
			const auto Mount = Paths.ResolveMountPath(Root.OriginalPath);
			if (!Mount || !Mount.Mount->bContentWritable
				|| AreSamePath(Root.OriginalPath, Mount.Mount->PhysicalRoot))
				return {EAssetWriteError::ReadOnlyMode, "Deletion mount policy changed."};
			std::filesystem::path Reparse;
			std::error_code Error;
			// Check surviving ancestors even after a confirmed root was removed.
			auto Parent = std::filesystem::path(Root.OriginalPath).parent_path();
			while (!std::filesystem::exists(Parent, Error) && !Error
				&& Parent != Parent.parent_path()) Parent = Parent.parent_path();
			if (Error || FindReparsePointInPath(Mount.Mount->PhysicalRoot, Parent, Reparse, Error) || Error)
				return {EAssetWriteError::InvalidPath, "Deletion ancestor changed or cannot be inspected."};
		}
		FContentBrowserOperationResult Result(Session.Execution->Execute(std::move(Hooks)));
		const auto State = Result.AssetResult->State;
		if (State == EAssetOperationTerminalState::Rejected && !Session.Execution->HasStarted())
		{
			const auto Request = Session.Request;
			DeletionSessions.erase(Found);
			Result.ReplacementConfirmation = BuildDeletionPlan(Request);
			return Result;
		}
		if (State == EAssetOperationTerminalState::Completed
			|| State == EAssetOperationTerminalState::ContentCommittedProjectionPending)
		{
			for (const auto& Entry : Confirmation->Entries)
				Result.Changes.Changes.push_back({EContentChangeKind::Removed, Entry.PhysicalPath, {}, {}, {},
					Entry.Kind == EContentDeletionEntryKind::Directory});
			Result.bContentChanged = true;
			Result = Publish(std::move(Result));
			DeletionSessions.erase(Found);
		}
		else
		{
			// Retire failed destructive attempts. Partial changes need a fresh analysis.
			DeletionSessions.erase(Found);
			Result.bContentChanged = true;
			Result.Changes.bFullRefresh = true;
			Result = Publish(std::move(Result));
		}
		return Result;
	}
}

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserAssetServices::Default() -> FContentBrowserAssetServices
	{
		return {
			.SaveAssets = [](const auto& Request) { return IAssetTools::Get().SaveAssets(Request); },
			.DuplicateAsset = [](const auto& Request) { return IAssetTools::Get().DuplicateAsset(Request); },
			.RelocateAssets = [](const auto& Request) { return IAssetTools::Get().RelocateAssets(Request); },
			.FixUpRedirectors = [](const auto& Request) { return IAssetTools::Get().FixUpRedirectors(Request); },
			.PrepareDeletion = [](const auto& Request, auto& Operation) {
				return IAssetTools::Get().PrepareDeletion(Request, Operation);
			}};
	}

	auto FContentBrowserOperationService::QueryDuplicate(const FTopLevelAssetPath& Source) const -> FAssetWriteResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		const auto Entry = FindTopLevelAssetExact(Source);
		if (!Entry || Entry->IsRedirector())
			return {EAssetWriteError::NotFound, "The copied source is no longer an available real asset."};
		return {};
	}
}

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserOperationService::ValidateMoves(std::span<const FEditorAssetMove> Moves) const
		-> FAssetWriteResult
	{
		for (const auto& Move : Moves)
			for (const auto& Path : {Move.OldPath, Move.NewPath})
			{
				const auto Mount = Paths.ResolveMountPath(Paths.VirtualToPhysical(Path.ToString() + ".dasset"));
				if (!Mount || !Mount.Mount->bContentWritable)
					return {EAssetWriteError::ReadOnlyMode, "Asset moves require writable browser content mounts."};
			}
		return {};
	}
}
