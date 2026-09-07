#include "Operations/ContentBrowserOperationService.h"
#include "Panels/ContentBrowserFilesystem.h"

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

		auto Failure(EAssetError Error, std::string Message)
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
				if (!FFileHelper::HashFileXx128(
						Path, OutFingerprint.ByteIdentity, OutError))
					return false;
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
				EAssetError::InvalidPath,
				"The new name is empty or contains invalid path characters.");

		if (Item.Kind == EContentBrowserItemKind::Redirector)
			return Failure(
				EAssetError::InvalidPath,
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
					EAssetError::InvalidPath,
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
			const FContentBrowserOperationResult Result = RenameFolder(Item, NewName, Warning);
			if (!Result) return Publish(Result);
			FContentBrowserOperationResult Outcome = Result;
			Outcome.FocusPhysicalPath = NormalizePath(
				(std::filesystem::path(Item.PhysicalPath).parent_path()
					/ std::filesystem::path(NewName)).generic_string());
			Outcome.Warning = std::move(Warning);
			Outcome.Changes.Changes.push_back({EContentChangeKind::Renamed, Item.PhysicalPath,
				Outcome.FocusPhysicalPath, Item.VirtualPath, Outcome.RevealAssetPath,
				Item.Kind == EContentBrowserItemKind::Folder});
			Outcome.bContentChanged = true;
		return Publish(std::move(Outcome));
		}

		FAssetCompanionOwnership Ownership;
		const FAssetResult OwnershipResult =
			QueryAssetCompanionOwnership(Item.PhysicalPath, Ownership);
		if (!OwnershipResult)
			return Failure(
				OwnershipResult.Error,
				std::format(
					"Could not determine whether this file is asset-managed: {}",
					OwnershipResult.Message));
		if (Ownership.State == EAssetCompanionOwnershipState::Ambiguous)
			return Failure(
				EAssetError::InUse,
				"This file is claimed by multiple assets. Resolve companion ownership before renaming it.");
		if (Ownership.State == EAssetCompanionOwnershipState::Owned)
			return Failure(
				EAssetError::InUse,
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
				EAssetError::InvalidPath,
				"File renames must stay inside the same automatically scanned content mount.");
		if (!SourceMount.Mount->bContentWritable)
			return Failure(
				EAssetError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before renaming the file.");
		const ContentBrowserFilesystem::FPathProbe DestinationProbe =
			ContentBrowserFilesystem::Probe(Destination);
		if (DestinationProbe.Error)
			return Failure(
				EAssetError::IoError,
				std::format("Could not inspect the rename destination: {}", DestinationProbe.Error.message()));
		if (DestinationProbe.Exists())
			return Failure(
				EAssetError::InvalidPath,
				"An item with that name already exists.");

		std::error_code Ec;
		std::filesystem::rename(Item.PhysicalPath, Destination, Ec);
		if (Ec)
			return Failure(
				EAssetError::IoError,
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
				EAssetError::InvalidPackageType,
				"Only real assets can be duplicated.");
		FTopLevelAssetPath SourcePath;
		if (!FTopLevelAssetPath::TryCreate(Item.VirtualPath, SourcePath))
			return Failure(EAssetError::InvalidPath,
				"The source top-level asset path is invalid.");
		if (!SourcePath.IsValid())
			return Failure(
				EAssetError::InvalidPath,
				"The source asset path is invalid.");
		const std::string SourcePackagePath = SourcePath.GetPackagePath().ToString();
		const size_t Slash = SourcePackagePath.find_last_of('/');
		if (Slash == std::string::npos)
			return Failure(
				EAssetError::InvalidPath,
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
				EAssetError::InvalidPath,
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
				EAssetError::InvalidPath,
				"The paste destination is outside an automatically scanned content mount.");
		if (!Mount.Mount->bContentWritable)
			return Failure(
				EAssetError::ReadOnlyMode,
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

	auto FContentBrowserOperationService::RenameFolder(
		const FContentBrowserItem& Item,
		std::string_view NewName,
		std::string& OutWarning) -> FContentBrowserOperationResult
	{
		const std::filesystem::path OldFolder(Item.PhysicalPath);
		const std::filesystem::path NewFolder =
			OldFolder.parent_path() / std::filesystem::path(NewName);
		const FContentBrowserPaths::FMountPath OldMount =
			Paths.ResolveMountPath(OldFolder.generic_string());
		const FContentBrowserPaths::FMountPath NewMount =
			Paths.ResolveMountPath(NewFolder.generic_string());
		if (!OldMount || !NewMount || OldMount.Mount != NewMount.Mount)
			return {
				EAssetError::InvalidPath,
				"Folder moves must stay inside the same automatically scanned content mount."};
		if (!OldMount.Mount->bContentWritable)
			return {
				EAssetError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before renaming the folder."};
		const ContentBrowserFilesystem::FPathProbe NewFolderProbe =
			ContentBrowserFilesystem::Probe(NewFolder);
		if (NewFolderProbe.Error)
			return {
				EAssetError::IoError,
				std::format("Could not inspect the folder rename destination: {}", NewFolderProbe.Error.message())};
		if (NewFolderProbe.Exists())
			return {
				EAssetError::InvalidPath,
				"A folder with that name already exists."};

		const std::string OldVirtual =
			Paths.PhysicalToVirtualDirectory(OldFolder.generic_string());
		const std::string NewVirtual =
			Paths.PhysicalToVirtualDirectory(NewFolder.generic_string());
		if (OldVirtual.empty() || NewVirtual.empty())
			return {EAssetError::InvalidPath, "The folder path is invalid."};

		std::vector<FEditorAssetMove> Moves;
		std::unordered_set<std::string> ManagedFiles;
		std::vector<std::filesystem::path> RelativeDirectories;
		for (const auto& [Path, Data]
			: CaptureAssetCatalogSnapshot().Assets)
		{
			if (!FPaths::IsLexicalDescendantPath(
					NormalizePath(Data.PhysicalPath), Item.PhysicalPath, true))
				continue;
			if (!Path.GetView().starts_with(OldVirtual))
				return {
					EAssetError::InvalidPath,
					"An asset inside the folder has an inconsistent virtual path."};

			FPackagePath NewPath;
			if (!FPackagePath::TryCreate(
					NewVirtual
						+ std::string(Path.GetView().substr(OldVirtual.size())),
					NewPath))
				return {
					EAssetError::InvalidPath,
					"The destination contains an invalid asset path."};
			if (const FAssetCatalogEntry Existing =
					FindAssetExact(NewPath))
				return {
					EAssetError::AlreadyExists,
					Existing->EntryKind == EAssetRegistryEntryKind::Redirector
						? std::format(
							"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another folder name.",
							NewPath.ToString(), Existing->RedirectDestination.ToString())
						: std::format(
							"Asset {} already exists. Choose another folder name or remove the existing asset.",
							NewPath.ToString())};
			if (FindResidentPackage(NewPath))
				return {
					EAssetError::AlreadyExists,
					std::format(
						"A loaded package already uses {}. Close it or choose another folder name.",
						NewPath.ToString())};

			Moves.push_back({Path, NewPath});
			const std::filesystem::path AssetFile(Data.PhysicalPath);
			ManagedFiles.insert(NormalizePath(AssetFile.generic_string()));
		}

		std::error_code Ec;
		for (std::filesystem::recursive_directory_iterator It(
				 OldFolder,
				 std::filesystem::directory_options::skip_permission_denied, Ec),
			 End;
			 !Ec && It != End;
			 It.increment(Ec))
		{
			if (It->is_directory(Ec))
				RelativeDirectories.push_back(
					std::filesystem::relative(It->path(), OldFolder, Ec));
			if (It->is_regular_file(Ec))
			{
				const std::string PhysicalPath =
					NormalizePath(It->path().generic_string());
				if (ManagedFiles.contains(PhysicalPath)) continue;
				FAssetCompanionOwnership Ownership;
				const FAssetResult OwnershipResult =
					QueryAssetCompanionOwnership(PhysicalPath, Ownership);
				if (!OwnershipResult)
					return {
						OwnershipResult.Error,
						std::format(
							"Could not inspect ownership for {}: {}",
							It->path().filename().generic_string(),
							OwnershipResult.Message)};
				if (Ownership.State
					== EAssetCompanionOwnershipState::Ambiguous)
					return {
						EAssetError::InUse,
						std::format(
							"Folder file {} is claimed by multiple assets.",
							It->path().filename().generic_string())};
				if (Ownership.State == EAssetCompanionOwnershipState::Owned)
				{
					ManagedFiles.insert(PhysicalPath);
					continue;
				}
				return {
					EAssetError::IoError,
					std::format(
						"Folder contains an unmanaged file: {}. Move it separately before renaming the folder.",
						It->path().filename().generic_string())};
			}
		}
		if (Ec)
			return {
				EAssetError::IoError,
				std::format("Could not inspect folder contents: {}", Ec.message())};

		if (Moves.empty())
		{
			std::filesystem::rename(OldFolder, NewFolder, Ec);
			return Ec
				? FAssetResult{
					  EAssetError::IoError,
					  std::format("Folder rename failed: {}", Ec.message())}
				: FAssetResult{};
		}

		if (const auto Allowed = ValidateMoves(Moves); !Allowed) return Allowed;
		std::vector<std::filesystem::path> CreatedDirectories;
		for (const std::filesystem::path& RelativeDirectory : RelativeDirectories)
		{
			const std::filesystem::path DestinationDirectory =
				NewFolder / RelativeDirectory;
			const ContentBrowserFilesystem::FPathProbe DestinationDirectoryProbe =
				ContentBrowserFilesystem::Probe(DestinationDirectory);
			if (DestinationDirectoryProbe.Error)
				return {EAssetError::IoError, std::format(
					"Could not inspect an empty destination directory: {}",
					DestinationDirectoryProbe.Error.message())};
			const bool bExisted = DestinationDirectoryProbe.Exists();
			Ec.clear();
			std::filesystem::create_directories(DestinationDirectory, Ec);
			if (!Ec)
			{
				if (!bExisted) CreatedDirectories.push_back(DestinationDirectory);
				continue;
			}
			for (auto It = CreatedDirectories.rbegin();
				It != CreatedDirectories.rend(); ++It)
			{
				std::error_code RemoveError;
				std::filesystem::remove(*It, RemoveError);
			}
			return {EAssetError::IoError, std::format(
				"Could not prepare an empty destination directory: {}",
				Ec.message())};
		}

		const FContentBrowserOperationResult MoveResult = MoveAssets(Moves);
		if (!MoveResult)
		{
			for (auto It = CreatedDirectories.rbegin();
				It != CreatedDirectories.rend(); ++It)
			{
				std::error_code RemoveError;
				std::filesystem::remove(*It, RemoveError);
			}
			return MoveResult;
		}

		std::vector<std::filesystem::path> OldDirectories;
		Ec.clear();
		const ContentBrowserFilesystem::FPathProbe OldFolderProbe =
			ContentBrowserFilesystem::Probe(OldFolder);
		if (OldFolderProbe.Error)
		{
			OutWarning = std::format(
				"Assets were moved successfully, but the source folder could not be inspected for cleanup: {}",
				OldFolderProbe.Error.message());
			return MoveResult;
		}
		if (OldFolderProbe.Exists())
		{
			for (std::filesystem::recursive_directory_iterator It(
					 OldFolder,
					 std::filesystem::directory_options::skip_permission_denied,
					 Ec),
				 End;
				 !Ec && It != End;
				 It.increment(Ec))
				if (It->is_directory(Ec)) OldDirectories.push_back(It->path());
			if (Ec)
			{
				OutWarning = std::format(
					"Assets were moved successfully, but the source folder could not be inspected for cleanup: {}",
					Ec.message());
				return MoveResult;
			}
			std::ranges::sort(
				OldDirectories,
				[](const auto& A, const auto& B) {
					return A.native().size() > B.native().size();
				});
			for (const auto& Directory : OldDirectories)
			{
				Ec.clear();
				if (!RemoveDirectory(Directory, Ec) && !Ec) continue;
				if (Ec)
				{
					OutWarning = std::format(
						"Assets were moved successfully, but source-folder cleanup failed for {}: {}",
						Directory.generic_string(), Ec.message());
					return MoveResult;
				}
			}
			Ec.clear();
			if (!RemoveDirectory(OldFolder, Ec) || Ec)
			{
				OutWarning = std::format(
					"Assets were moved successfully, but the source folder is not empty or could not be removed: {}",
					Ec ? Ec.message() : OldFolder.generic_string());
				return MoveResult;
			}
		}
		return MoveResult;
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
				EAssetError::InvalidPath,
				"Folders can only be created inside an automatically scanned content mount.");
		if (!DirectoryMount.Mount->bContentWritable)
			return Failure(
				EAssetError::ReadOnlyMode,
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
					EAssetError::IoError,
					std::format("Could not inspect a folder candidate: {}", CandidateProbe.Error.message()));
			if (CandidateProbe.Exists()) continue;
			const FContentBrowserPaths::FMountPath DestinationMount =
				Paths.ResolveMountPath(Path.generic_string());
			if (!DestinationMount || DestinationMount.Mount != DirectoryMount.Mount)
				return Failure(
					EAssetError::InvalidPath,
					"The new folder would be outside its automatically scanned content mount.");
			std::error_code Ec;
			if (!std::filesystem::create_directory(Path, Ec) || Ec)
				return Failure(
					EAssetError::IoError,
					std::format("Could not create folder: {}", Ec.message()));
			FContentBrowserOperationResult Outcome;
			Outcome.Changes.Changes.push_back({EContentChangeKind::Added, {}, NormalizePath(Path.generic_string()), {}, {}, true});
			Outcome.FocusPhysicalPath = NormalizePath(Path.generic_string());
			Outcome.bContentChanged = true;
			return Publish(std::move(Outcome));
		}
		return Failure(
			EAssetError::AlreadyExists,
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
				EAssetError::InvalidPath,
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
		if (!Plan.CanExecute()
			|| Plan.RegistryRevision != GetAssetCatalogRevision())
			return false;
		std::vector<FContentBrowserItem> Items;
		std::unordered_set<std::string> Selection;
		Items.reserve(Plan.MaximalRoots.size());
		for (const FContentDeletionRoot& Root : Plan.MaximalRoots)
		{
			FContentBrowserItem Item{
				.Kind = Root.Kind == EContentDeletionEntryKind::Directory
					? EContentBrowserItemKind::Folder
					: EContentBrowserItemKind::File,
				.Name = std::filesystem::path(Root.OriginalPath)
					.filename().generic_string(),
				.PhysicalPath = Root.OriginalPath};
			Selection.insert(Item.StableId());
			Items.push_back(std::move(Item));
		}
		FAssetDeletionOperation CurrentOperation;
		const FContentDeletionPlanPtr Current =
			AnalyzeDeletion(Items, CurrentOperation);
		if (!Current || !Current->CanExecute()
			|| Current->Entries.size() != Plan.Entries.size())
			return false;
		if (Current->Warnings.size() != Plan.Warnings.size()) return false;
		for (size_t Index = 0; Index < Plan.Warnings.size(); ++Index)
			if (Current->Warnings[Index].DisplayName != Plan.Warnings[Index].DisplayName
				|| Current->Warnings[Index].Details != Plan.Warnings[Index].Details) return false;
		for (size_t Index = 0; Index < Plan.Entries.size(); ++Index)
		{
			const FContentDeletionFingerprint& Before = Plan.Entries[Index];
			const FContentDeletionFingerprint& After = Current->Entries[Index];
			if (Before.PhysicalPath != After.PhysicalPath
				|| Before.Kind != After.Kind
				|| Before.FileSize != After.FileSize
				|| Before.LastWriteTimeTicks != After.LastWriteTimeTicks
				|| Before.ByteIdentity != After.ByteIdentity
				|| Before.Digest != After.Digest)
				return false;
		}
		return true;
	}

} // namespace Durin::Editor::ContentBrowser::Private

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserOperationService::QueryMutation() const -> FAssetResult
	{
		if (!bAccepting) return {EAssetError::ShuttingDown, "Content operations are stopping."};
		if (CanMutate && !CanMutate()) return {EAssetError::ReadOnlyMode, "Content mutation is currently disabled."};
		return {};
	}

	auto FContentBrowserOperationService::QuerySave(const FPackagePath& Path) const -> FAssetResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		const auto* Package = FindResidentPackage(Path);
		return Package && Package->IsDirty() ? FAssetResult{}
			: FAssetResult{EAssetError::InUse, "Save requires a resident dirty package."};
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
					|| Asset.State == EAssetOperationTerminalState::ForwardPending
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
		if (Found != DeletionSessions.end() && Found->second.Confirmation == Confirmation
			&& !Found->second.Execution->HasStarted()) DeletionSessions.erase(Found);
	}

	auto FContentBrowserOperationService::GetPendingDeletion() const -> FContentDeletionPlanPtr
	{
		for (const auto& [Id, Session] : DeletionSessions)
			if (Session.Execution->HasStarted()) return Session.Confirmation;
		return {};
	}

	auto FContentBrowserOperationService::ExecuteDeletion(
		FContentDeletionPlanPtr Confirmation, FContentDeletionHooks Hooks) -> FContentBrowserOperationResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		if (!Confirmation) return {EAssetError::StaleData, "Deletion confirmation is unavailable."};
		const auto Found = DeletionSessions.find(Confirmation->SessionId);
		if (Found == DeletionSessions.end() || Found->second.Confirmation != Confirmation)
			return {EAssetError::StaleData, "Deletion confirmation was retired."};
		auto& Session = Found->second;
		if (!Confirmation->CanExecute()) return {EAssetError::InUse, "Deletion is blocked."};
		if (!Session.Execution->HasStarted() && !IsDeletionPlanCurrent(*Confirmation))
		{
			const auto Request = Session.Request;
			DeletionSessions.erase(Found);
			FContentBrowserOperationResult Result{EAssetError::StaleData, "Deletion scope changed. Confirm the updated scope."};
			Result.ReplacementConfirmation = BuildDeletionPlan(Request);
			return Result;
		}
		Paths.RefreshMountSnapshot();
		for (const auto& Root : Confirmation->MaximalRoots)
		{
			const auto Mount = Paths.ResolveMountPath(Root.OriginalPath);
			if (!Mount || !Mount.Mount->bContentWritable
				|| AreSamePath(Root.OriginalPath, Mount.Mount->PhysicalRoot))
				return {EAssetError::ReadOnlyMode, "Deletion mount policy changed."};
			std::filesystem::path Reparse;
			std::error_code Error;
			// Check surviving ancestors even after a confirmed root was removed.
			auto Parent = std::filesystem::path(Root.OriginalPath).parent_path();
			while (!std::filesystem::exists(Parent, Error) && !Error
				&& Parent != Parent.parent_path()) Parent = Parent.parent_path();
			if (Error || FindReparsePointInPath(Mount.Mount->PhysicalRoot, Parent, Reparse, Error) || Error)
				return {EAssetError::InvalidPath, "Deletion ancestor changed or cannot be inspected."};
		}
		FContentBrowserOperationResult Result(Session.Execution->Execute(std::move(Hooks)));
		const auto State = Result.AssetResult->State;
		if (State == EAssetOperationTerminalState::Completed
			|| State == EAssetOperationTerminalState::ContentCommittedProjectionPending)
		{
			if (!Session.bPublished)
			{
				for (const auto& Entry : Confirmation->Entries)
					Result.Changes.Changes.push_back({EContentChangeKind::Removed, Entry.PhysicalPath, {}, {}, {},
						Entry.Kind == EContentDeletionEntryKind::Directory});
				Result.bContentChanged = true;
				Result = Publish(std::move(Result));
				Session.bPublished = true;
			}
			DeletionSessions.erase(Found);
		}
		// Forward-pending content remains fenced. Do not reconcile away the original
		// asset safety snapshot until the same session finishes destructive work.
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

	auto FContentBrowserOperationService::QueryDuplicate(const FTopLevelAssetPath& Source) const -> FAssetResult
	{
		if (const auto Allowed = QueryMutation(); !Allowed) return Allowed;
		const auto Entry = FindTopLevelAssetExact(Source);
		if (!Entry || Entry->IsRedirector())
			return {EAssetError::NotFound, "The copied source is no longer an available real asset."};
		return {};
	}
}

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserOperationService::ValidateMoves(std::span<const FEditorAssetMove> Moves) const
		-> FAssetResult
	{
		for (const auto& Move : Moves)
			for (const auto& Path : {Move.OldPath, Move.NewPath})
			{
				const auto Mount = Paths.ResolveMountPath(Paths.VirtualToPhysical(Path.ToString() + ".dasset"));
				if (!Mount || !Mount.Mount->bContentWritable)
					return {EAssetError::ReadOnlyMode, "Asset moves require writable browser content mounts."};
			}
		return {};
	}
}
