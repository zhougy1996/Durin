#include "Panels/ContentBrowserOperations.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "Asset/AssetOperations.h"
#include "Asset/Deletion.h"
#include "Asset/Mutation.h"
#include "Asset.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Class.h"
#include "Misc/LexicalPath.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonaImGui.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		constexpr uint64 FnvOffset = 14695981039346656037ull;
		constexpr uint64 FnvPrime = 1099511628211ull;
		using ContentBrowserFilesystem::NormalizePath;

		auto Failure(Asset::EAssetError Error, std::string Message)
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
			if (!PathUtilities::TryMakeLexicalRelativePath(
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

		auto IsSameVolume(
			const std::filesystem::path& A,
			const std::filesystem::path& B) -> bool
		{
			std::string Left = A.root_name().generic_string();
			std::string Right = B.root_name().generic_string();
#ifdef _WIN32
			std::ranges::transform(Left, Left.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			std::ranges::transform(Right, Right.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
#endif
			return Left == Right;
		}

		auto AreSamePath(std::string_view A, std::string_view B) -> bool
		{
			std::filesystem::path Relative;
			return PathUtilities::TryMakeLexicalRelativePath(
				std::filesystem::path(A), std::filesystem::path(B), Relative)
				&& Relative.empty();
		}
	} // namespace

	FContentBrowserOperations::FContentBrowserOperations(
		FContentBrowserModel& InModel,
		FMoveAssets InMoveAssets,
		FRemoveDirectory InRemoveDirectory,
		FFixUpAssets InFixUpAssets,
		std::function<void()> InNotifyMountedContentMutation)
		: Model(InModel)
		, MoveAssets(std::move(InMoveAssets))
		, FixUpAssets(std::move(InFixUpAssets))
		, NotifyMountedContentMutation(std::move(InNotifyMountedContentMutation))
		, RemoveDirectory(std::move(InRemoveDirectory))
	{
		if (!RemoveDirectory)
			RemoveDirectory = [](const std::filesystem::path& Path,
				std::error_code& Error) {
				return std::filesystem::remove(Path, Error);
			};
	}

	auto FContentBrowserOperations::Rename(
		const FContentBrowserItem& Item,
		std::string_view NewName) -> FContentBrowserOperationResult
	{
		if (NewName.empty() || NewName == "." || NewName == ".."
			|| NewName.find_first_of("/\\:*") != std::string_view::npos)
			return Failure(
				Asset::EAssetError::InvalidPath,
				"The new name is empty or contains invalid path characters.");

		if (Item.Kind == EContentBrowserItemKind::Redirector)
			return Failure(
				Asset::EAssetError::InvalidPath,
				"Redirectors cannot be renamed or moved directly. Fix Up the redirector or move its final asset.");

		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const size_t Slash = Item.VirtualPath.find_last_of('/');
			FAssetPath OldPath;
			FAssetPath NewPath;
			if (!FAssetPath::TryCreate(Item.VirtualPath, OldPath)
				|| Slash == std::string::npos
				|| !FAssetPath::TryCreate(
					Item.VirtualPath.substr(0, Slash + 1) + std::string(NewName),
					NewPath))
				return Failure(
					Asset::EAssetError::InvalidPath,
					"The resulting asset path is invalid.");

			const FEditorAssetMove Move{OldPath, NewPath};
			const Asset::FAssetResult Result =
				MoveAssets(std::span{&Move, 1});
			if (!Result) return {Result};
			FContentBrowserOperationResult Outcome;
			Outcome.FocusPhysicalPath =
				Model.VirtualToPhysical(NewPath.ToString() + ".dasset");
			return Outcome;
		}

		if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			std::string Warning;
			const Asset::FAssetResult Result = RenameFolder(Item, NewName, Warning);
			if (!Result) return {Result};
			FContentBrowserOperationResult Outcome;
			Outcome.FocusPhysicalPath = NormalizePath(
				(std::filesystem::path(Item.PhysicalPath).parent_path()
					/ std::filesystem::path(NewName)).generic_string());
			Outcome.Warning = std::move(Warning);
			return Outcome;
		}

		Asset::FAssetCompanionOwnership Ownership;
		const Asset::FAssetResult OwnershipResult =
			Asset::QueryAssetCompanionOwnership(Item.PhysicalPath, Ownership);
		if (!OwnershipResult)
			return Failure(
				OwnershipResult.Error,
				std::format(
					"Could not determine whether this file is asset-managed: {}",
					OwnershipResult.Message));
		if (Ownership.State == Asset::EAssetCompanionOwnershipState::Ambiguous)
			return Failure(
				Asset::EAssetError::InUse,
				"This file is claimed by multiple assets. Resolve companion ownership before renaming it.");
		if (Ownership.State == Asset::EAssetCompanionOwnershipState::Owned)
			return Failure(
				Asset::EAssetError::InUse,
				std::format(
					"This file is managed by {}. Rename or move the owning asset instead.",
					Ownership.Owners.front().ToString()));

		std::filesystem::path Destination =
			std::filesystem::path(Item.PhysicalPath).parent_path()
			/ std::filesystem::path(NewName);
		if (Destination.extension().empty()) Destination += Item.Extension;
		const FContentBrowserModel::FMountPath SourceMount =
			Model.ResolveMountPath(Item.PhysicalPath);
		const FContentBrowserModel::FMountPath DestinationMount =
			Model.ResolveMountPath(Destination.generic_string());
		if (!SourceMount || !DestinationMount
			|| SourceMount.Mount != DestinationMount.Mount)
			return Failure(
				Asset::EAssetError::InvalidPath,
				"File renames must stay inside the same automatically scanned content mount.");
		if (!SourceMount.Mount->bContentWritable)
			return Failure(
				Asset::EAssetError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before renaming the file.");
		const ContentBrowserFilesystem::FPathProbe DestinationProbe =
			ContentBrowserFilesystem::Probe(Destination);
		if (DestinationProbe.Error)
			return Failure(
				Asset::EAssetError::IoError,
				std::format("Could not inspect the rename destination: {}", DestinationProbe.Error.message()));
		if (DestinationProbe.Exists())
			return Failure(
				Asset::EAssetError::InvalidPath,
				"An item with that name already exists.");

		std::error_code Ec;
		std::filesystem::rename(Item.PhysicalPath, Destination, Ec);
		if (Ec)
			return Failure(
				Asset::EAssetError::IoError,
				std::format("Rename failed: {}", Ec.message()));
		FContentBrowserOperationResult Outcome;
		Outcome.FocusPhysicalPath = NormalizePath(Destination.generic_string());
		return Outcome;
	}

	auto FContentBrowserOperations::Duplicate(
		const FContentBrowserItem& Item) -> FContentBrowserOperationResult
	{
		if (Item.Kind != EContentBrowserItemKind::Asset)
			return Failure(
				Asset::EAssetError::InvalidPackageType,
				"Only real assets can be duplicated.");
		FAssetPath SourcePath;
		if (!FAssetPath::TryCreate(Item.VirtualPath, SourcePath))
			return Failure(
				Asset::EAssetError::InvalidPath,
				"The source asset path is invalid.");
		const size_t Slash = Item.VirtualPath.find_last_of('/');
		if (Slash == std::string::npos)
			return Failure(
				Asset::EAssetError::InvalidPath,
				"The source asset has no valid destination directory.");
		return Duplicate(SourcePath, Item.VirtualPath.substr(0, Slash + 1));
	}

	auto FContentBrowserOperations::Duplicate(
		const FAssetPath& SourcePath,
		std::string_view DestinationDirectory)
		-> FContentBrowserOperationResult
	{
		if (!SourcePath.IsValid() || DestinationDirectory.empty())
			return Failure(
				Asset::EAssetError::InvalidPath,
				"Asset paste requires a valid source and destination folder.");
		std::string Directory(DestinationDirectory);
		if (!Directory.ends_with('/')) Directory.push_back('/');
		const Asset::FAssetCatalogEntry SourceData =
			Asset::FindAssetExact(SourcePath);
		if (!SourceData
			|| SourceData->EntryKind != Asset::EAssetRegistryEntryKind::Asset)
			return Failure(
				Asset::EAssetError::NotFound,
				"The copied source is no longer an available real asset.");
		const std::string DestinationDirectoryPhysical =
			Model.VirtualToPhysical(Directory);
		const FContentBrowserModel::FMountPath Mount =
			Model.ResolveMountPath(DestinationDirectoryPhysical);
		if (!Mount)
			return Failure(
				Asset::EAssetError::InvalidPath,
				"The paste destination is outside an automatically scanned content mount.");
		if (!Mount.Mount->bContentWritable)
			return Failure(
				Asset::EAssetError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before pasting the asset.");

		const FAssetOperationResult Result = IAssetTools::Get().DuplicateAsset({
			.SourcePath = SourcePath,
			.DestinationDirectory = Directory,
			.ResolvePhysicalPackagePath = [this](const FAssetPath& Path) {
				return Model.VirtualToPhysical(Path.ToString() + ".dasset");
			},
			.Publish = [this](const FAssetOperationNotification&) {
				if (NotifyMountedContentMutation) NotifyMountedContentMutation();
			}});
		if (!Result)
			return Failure(Asset::EAssetError::IoError, Result.Message);

		FContentBrowserOperationResult Outcome;
		Outcome.FocusPhysicalPath = Result.PhysicalPath;
		Outcome.RevealAssetPath = Result.AffectedAssets.front().ToString();
		Outcome.OpenAssetClassName = SourceData->AssetClassName;
		return Outcome;
	}

	auto FContentBrowserOperations::RenameFolder(
		const FContentBrowserItem& Item,
		std::string_view NewName,
		std::string& OutWarning) -> Asset::FAssetResult
	{
		const std::filesystem::path OldFolder(Item.PhysicalPath);
		const std::filesystem::path NewFolder =
			OldFolder.parent_path() / std::filesystem::path(NewName);
		const FContentBrowserModel::FMountPath OldMount =
			Model.ResolveMountPath(OldFolder.generic_string());
		const FContentBrowserModel::FMountPath NewMount =
			Model.ResolveMountPath(NewFolder.generic_string());
		if (!OldMount || !NewMount || OldMount.Mount != NewMount.Mount)
			return {
				Asset::EAssetError::InvalidPath,
				"Folder moves must stay inside the same automatically scanned content mount."};
		if (!OldMount.Mount->bContentWritable)
			return {
				Asset::EAssetError::ReadOnlyMode,
				"This content mount is not content-writable. Choose a writable mount before renaming the folder."};
		const ContentBrowserFilesystem::FPathProbe NewFolderProbe =
			ContentBrowserFilesystem::Probe(NewFolder);
		if (NewFolderProbe.Error)
			return {
				Asset::EAssetError::IoError,
				std::format("Could not inspect the folder rename destination: {}", NewFolderProbe.Error.message())};
		if (NewFolderProbe.Exists())
			return {
				Asset::EAssetError::InvalidPath,
				"A folder with that name already exists."};

		const std::string OldVirtual =
			Model.PhysicalToVirtualDirectory(OldFolder.generic_string());
		const std::string NewVirtual =
			Model.PhysicalToVirtualDirectory(NewFolder.generic_string());
		if (OldVirtual.empty() || NewVirtual.empty())
			return {Asset::EAssetError::InvalidPath, "The folder path is invalid."};

		std::vector<FEditorAssetMove> Moves;
		std::unordered_set<std::string> ManagedFiles;
		std::vector<std::filesystem::path> RelativeDirectories;
		for (const auto& [Path, Data]
			: Asset::CaptureAssetCatalogSnapshot().Assets)
		{
			if (!PathUtilities::IsLexicalDescendantPath(
					NormalizePath(Data.PhysicalPath), Item.PhysicalPath, true))
				continue;
			if (!Path.GetView().starts_with(OldVirtual))
				return {
					Asset::EAssetError::InvalidPath,
					"An asset inside the folder has an inconsistent virtual path."};

			FAssetPath NewPath;
			if (!FAssetPath::TryCreate(
					NewVirtual
						+ std::string(Path.GetView().substr(OldVirtual.size())),
					NewPath))
				return {
					Asset::EAssetError::InvalidPath,
					"The destination contains an invalid asset path."};
			if (const Asset::FAssetCatalogEntry Existing =
					Asset::FindAssetExact(NewPath))
				return {
					Asset::EAssetError::AlreadyExists,
					Existing->EntryKind == Asset::EAssetRegistryEntryKind::Redirector
						? std::format(
							"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another folder name.",
							NewPath.ToString(), Existing->RedirectDestination.ToString())
						: std::format(
							"Asset {} already exists. Choose another folder name or remove the existing asset.",
							NewPath.ToString())};
			if (Asset::FindResidentPackage(NewPath))
				return {
					Asset::EAssetError::AlreadyExists,
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
				Asset::FAssetCompanionOwnership Ownership;
				const Asset::FAssetResult OwnershipResult =
					Asset::QueryAssetCompanionOwnership(PhysicalPath, Ownership);
				if (!OwnershipResult)
					return {
						OwnershipResult.Error,
						std::format(
							"Could not inspect ownership for {}: {}",
							It->path().filename().generic_string(),
							OwnershipResult.Message)};
				if (Ownership.State
					== Asset::EAssetCompanionOwnershipState::Ambiguous)
					return {
						Asset::EAssetError::InUse,
						std::format(
							"Folder file {} is claimed by multiple assets.",
							It->path().filename().generic_string())};
				if (Ownership.State == Asset::EAssetCompanionOwnershipState::Owned)
				{
					ManagedFiles.insert(PhysicalPath);
					continue;
				}
				return {
					Asset::EAssetError::IoError,
					std::format(
						"Folder contains an unmanaged file: {}. Move it separately before renaming the folder.",
						It->path().filename().generic_string())};
			}
		}
		if (Ec)
			return {
				Asset::EAssetError::IoError,
				std::format("Could not inspect folder contents: {}", Ec.message())};

		if (Moves.empty())
		{
			std::filesystem::rename(OldFolder, NewFolder, Ec);
			return Ec
				? Asset::FAssetResult{
					  Asset::EAssetError::IoError,
					  std::format("Folder rename failed: {}", Ec.message())}
				: Asset::FAssetResult{};
		}

		std::vector<std::filesystem::path> CreatedDirectories;
		for (const std::filesystem::path& RelativeDirectory : RelativeDirectories)
		{
			const std::filesystem::path DestinationDirectory =
				NewFolder / RelativeDirectory;
			const ContentBrowserFilesystem::FPathProbe DestinationDirectoryProbe =
				ContentBrowserFilesystem::Probe(DestinationDirectory);
			if (DestinationDirectoryProbe.Error)
				return {Asset::EAssetError::IoError, std::format(
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
			return {Asset::EAssetError::IoError, std::format(
				"Could not prepare an empty destination directory: {}",
				Ec.message())};
		}

		const Asset::FAssetResult MoveResult = MoveAssets(Moves);
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
			return {};
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
				return {};
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
					return {};
				}
			}
			Ec.clear();
			if (!RemoveDirectory(OldFolder, Ec) || Ec)
			{
				OutWarning = std::format(
					"Assets were moved successfully, but the source folder is not empty or could not be removed: {}",
					Ec ? Ec.message() : OldFolder.generic_string());
				return {};
			}
		}
		return {};
	}

	auto FContentBrowserOperations::CreateFolder(
		std::string_view PhysicalDirectory) -> FContentBrowserOperationResult
	{
		const std::string NormalizedDirectory = NormalizePath(PhysicalDirectory);
		const FContentBrowserModel::FMountPath DirectoryMount =
			Model.ResolveMountPath(NormalizedDirectory);
		if (!DirectoryMount)
			return Failure(
				Asset::EAssetError::InvalidPath,
				"Folders can only be created inside an automatically scanned content mount.");
		if (!DirectoryMount.Mount->bContentWritable)
			return Failure(
				Asset::EAssetError::ReadOnlyMode,
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
					Asset::EAssetError::IoError,
					std::format("Could not inspect a folder candidate: {}", CandidateProbe.Error.message()));
			if (CandidateProbe.Exists()) continue;
			const FContentBrowserModel::FMountPath DestinationMount =
				Model.ResolveMountPath(Path.generic_string());
			if (!DestinationMount || DestinationMount.Mount != DirectoryMount.Mount)
				return Failure(
					Asset::EAssetError::InvalidPath,
					"The new folder would be outside its automatically scanned content mount.");
			std::error_code Ec;
			if (!std::filesystem::create_directory(Path, Ec) || Ec)
				return Failure(
					Asset::EAssetError::IoError,
					std::format("Could not create folder: {}", Ec.message()));
			FContentBrowserOperationResult Outcome;
			Outcome.FocusPhysicalPath = NormalizePath(Path.generic_string());
			return Outcome;
		}
		return Failure(
			Asset::EAssetError::AlreadyExists,
			"Could not find a unique folder name in this directory.");
	}

	auto FContentBrowserOperations::Move(std::span<const FEditorAssetMove> Moves)
		-> Asset::FAssetResult
	{
		return MoveAssets(Moves);
	}

	auto FContentBrowserOperations::CollectRedirectors(
		std::string_view VirtualDirectory) const -> std::vector<FAssetPath>
	{
		std::string Prefix(VirtualDirectory);
		if (!Prefix.empty() && !Prefix.ends_with('/')) Prefix += '/';
		std::vector<FAssetPath> Redirectors;
		for (const auto& [Path, Data]
			: Asset::CaptureAssetCatalogSnapshot().Assets)
		{
			if (Data.EntryKind != Asset::EAssetRegistryEntryKind::Redirector)
				continue;
			if (!Prefix.empty() && !Path.GetView().starts_with(Prefix)) continue;
			Redirectors.push_back(Path);
		}
		std::ranges::sort(
			Redirectors,
			[](const FAssetPath& A, const FAssetPath& B) {
				return A.GetView() < B.GetView();
			});
		return Redirectors;
	}

	auto FContentBrowserOperations::FixUpRedirectorsInFolder(
		std::string_view VirtualDirectory) -> Asset::FAssetResult
	{
		if (VirtualDirectory.empty())
			return {
				Asset::EAssetError::InvalidPath,
				"Fix Up in Folder requires a mounted virtual directory."};
		const std::vector<FAssetPath> Redirectors =
			CollectRedirectors(VirtualDirectory);
		if (Redirectors.empty()) return {};
		return FixUpAssets ? FixUpAssets(Redirectors) : Asset::FAssetResult{
			Asset::EAssetError::ShuttingDown, "Redirector fix-up is unavailable."};
	}

	auto FContentBrowserOperations::FixUpRedirectors(
		std::span<const FAssetPath> Redirectors) -> Asset::FAssetResult
	{
		return FixUpAssets ? FixUpAssets(Redirectors) : Asset::FAssetResult{
			Asset::EAssetError::ShuttingDown, "Redirector fix-up is unavailable."};
	}

	auto FContentBrowserOperations::FixUpAllRedirectors()
		-> Asset::FAssetResult
	{
		const std::vector<FAssetPath> Redirectors = CollectRedirectors("/");
		return Redirectors.empty() ? Asset::FAssetResult{}
			: FixUpAssets ? FixUpAssets(Redirectors) : Asset::FAssetResult{
				Asset::EAssetError::ShuttingDown, "Redirector fix-up is unavailable."};
	}

	auto FContentBrowserOperations::BuildDeletionPlan(
		std::span<const FContentBrowserItem> Items,
		const std::unordered_set<std::string>& Selection) const
		-> FContentDeletionPlanPtr
	{
		auto Plan = std::make_shared<FContentDeletionPlan>();
		Plan->RegistryRevision = Asset::GetAssetCatalogRevision();
		Plan->StagingVolumeRoot = NormalizePath(
			(std::filesystem::path(FPaths::ProjectDir())
				/ "Saved/ContentBrowserUndo").generic_string());

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

		if (Selection.empty())
		{
			AddBlocker(
				EContentDeletionBlocker::InvalidSelection,
				"Selection", {}, {}, "No content is selected.");
			return Plan;
		}

		Model.RefreshMountSnapshot();
		struct FSelectedRoot
		{
			const FContentBrowserItem* Item = nullptr;
			std::string PhysicalPath;
			const FContentBrowserModel::FMountSnapshot* Mount = nullptr;
		};
		std::vector<FSelectedRoot> SelectedRoots;
		for (const FContentBrowserItem& Item : Items)
		{
			if (!Selection.contains(Item.StableId())) continue;
			const std::string PhysicalPath = NormalizePath(Item.PhysicalPath);
			const FContentBrowserModel::FMountPath Resolved =
				Model.ResolveMountPath(PhysicalPath);
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
			const FContentBrowserModel::FMountSnapshot* Mount = Resolved.Mount;
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
			if (!IsSameVolume(PhysicalPath, Plan->StagingVolumeRoot))
				AddBlocker(
					EContentDeletionBlocker::CrossVolumeStaging,
					Item.Name,
					PhysicalPath,
					{},
					"The selected root cannot be renamed into the Undo staging volume.");
			SelectedRoots.push_back({&Item, PhysicalPath, Mount});
		}

		if (SelectedRoots.size() != Selection.size())
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
						return PathUtilities::IsLexicalDescendantPath(
							Candidate.PhysicalPath, Existing.PhysicalPath, true);
					}))
				continue;
			MaximalRoots.push_back(Candidate);
		}
		if (MaximalRoots.size() == 1)
			Plan->DisplayName = MaximalRoots.front().Item->Name;
		else
			Plan->DisplayName = std::format("{} Items", MaximalRoots.size());

		const Asset::FAssetCatalogSnapshot Catalog =
			Asset::CaptureAssetCatalogSnapshot();
		std::unordered_map<std::string, const Asset::FAssetData*> AssetsByPhysicalPath;
		for (const auto& [Path, Data] : Catalog.Assets)
			AssetsByPhysicalPath.emplace(NormalizePath(Data.PhysicalPath), &Data);
		std::vector<FAssetPath> AssetPaths;
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

		std::ranges::sort(AssetPaths, [](const FAssetPath& A, const FAssetPath& B) {
			return A.GetView() < B.GetView();
		});
		AssetPaths.erase(std::unique(AssetPaths.begin(), AssetPaths.end()), AssetPaths.end());
		const FAssetOperationResult AssetResult = IAssetTools::Get().PrepareDeletion({
			.AssetPaths = AssetPaths, .PhysicalRoots = PhysicalRoots},
			Plan->AssetOperation);
		if (!AssetResult && Plan->AssetOperation.GetBlockers().empty())
			AddBlocker(
				EContentDeletionBlocker::InspectionFailed,
				"Assets", {}, {}, AssetResult.Message);

		std::unordered_set<std::string> CompanionPaths;
		for (const FAssetDeletionEntry& Entry : Plan->AssetOperation.GetEntries())
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

			const FContentBrowserModel::FMountSnapshot* SelectedMount =
				MaximalRoots.empty() ? nullptr : MaximalRoots.front().Mount;
			std::filesystem::path SelectedReparsePoint;
			if (SelectedMount
				&& PathUtilities::IsLexicalDescendantPath(
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

			const FContentBrowserModel::FMountPath Resolved =
				Model.ResolveMountPath(CompanionPath);
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
			const FContentBrowserModel::FMountSnapshot* CompanionMount = Resolved.Mount;
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
			if (!IsSameVolume(CompanionPath, Plan->StagingVolumeRoot))
			{
				AddBlocker(
					EContentDeletionBlocker::CrossVolumeStaging,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"The asset companion cannot be renamed into the Undo staging volume.");
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

		for (const FAssetDeletionBlocker& Blocker : Plan->AssetOperation.GetBlockers())
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
		for (const FAssetOperationWarning& Warning : Plan->AssetOperation.GetWarnings())
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
				if (!PathUtilities::IsLexicalDescendantPath(
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

	auto FContentBrowserOperations::IsDeletionPlanCurrent(
		const FContentDeletionPlan& Plan) const -> bool
	{
		if (!Plan.CanExecute()
			|| Plan.RegistryRevision != Asset::GetAssetCatalogRevision())
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
		const FContentDeletionPlanPtr Current =
			BuildDeletionPlan(Items, Selection);
		if (!Current || !Current->CanExecute()
			|| Current->Entries.size() != Plan.Entries.size())
			return false;
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

	auto FContentBrowserOperations::ShowInExplorer(
		std::string_view PhysicalPath) const -> void
	{
#ifdef _WIN32
		const std::filesystem::path Path(PhysicalPath);
		std::filesystem::path PreferredPath = Path;
		const std::wstring WidePath = PreferredPath.make_preferred().wstring();
		if (ContentBrowserFilesystem::Probe(Path).IsDirectory())
			ShellExecuteW(
				nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
		else
		{
			const std::wstring Args = L"/select,\"" + WidePath + L"\"";
			ShellExecuteW(
				nullptr,
				L"open",
				L"explorer.exe",
				Args.c_str(),
				nullptr,
				SW_SHOW);
		}
#endif
	}

	auto FContentBrowserOperations::CopyToClipboard(std::string_view Text) const
		-> void
	{
		ImGui::SetClipboardText(std::string(Text).c_str());
	}
} // namespace Durin::Editor::ContentBrowser::Private
