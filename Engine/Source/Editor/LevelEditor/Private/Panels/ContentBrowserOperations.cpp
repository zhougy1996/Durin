#include "Panels/ContentBrowserOperations.h"

#include "AssetSystem.h"
#include "Engine/Level.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "MonaImGui.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin
{
	namespace
	{
		constexpr uint64 FnvOffset = 14695981039346656037ull;
		constexpr uint64 FnvPrime = 1099511628211ull;

		auto NormalizePath(std::string_view Path) -> std::string
		{
			if (Path.empty()) return {};
			return std::filesystem::absolute(std::filesystem::path(Path))
				.lexically_normal()
				.generic_string();
		}

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
			return HashAppend(
				Hash, std::to_string(Fingerprint.LastWriteTimeTicks));
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
		FMoveAssets InMoveAssets)
		: Model(InModel)
		, MoveAssets(std::move(InMoveAssets))
	{
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
			const Asset::FAssetResult Result = RenameFolder(Item, NewName);
			if (!Result) return {Result};
			return {};
		}

		if (IsManagedCompanion(Item))
			return Failure(
				Asset::EAssetError::InvalidPath,
				"This file is managed by an asset. Rename or move the asset instead.");

		std::filesystem::path Destination =
			std::filesystem::path(Item.PhysicalPath).parent_path()
			/ std::filesystem::path(NewName);
		if (Destination.extension().empty()) Destination += Item.Extension;
		if (std::filesystem::exists(Destination))
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

	auto FContentBrowserOperations::RenameFolder(
		const FContentBrowserItem& Item,
		std::string_view NewName) -> Asset::FAssetResult
	{
		const std::filesystem::path OldFolder(Item.PhysicalPath);
		const std::filesystem::path NewFolder =
			OldFolder.parent_path() / std::filesystem::path(NewName);
		if (std::filesystem::exists(NewFolder))
			return {
				Asset::EAssetError::InvalidPath,
				"A folder with that name already exists."};

		const std::string OldVirtual =
			Model.PhysicalToVirtualDirectory(OldFolder.generic_string());
		const std::string NewVirtual =
			Model.PhysicalToVirtualDirectory(NewFolder.generic_string());
		if (OldVirtual.empty() || NewVirtual.empty())
			return {
				Asset::EAssetError::InvalidPath,
				"Folder moves must stay inside the same mounted content root."};

		std::vector<FEditorAssetMove> Moves;
		std::unordered_set<std::string> ManagedFiles;
		std::vector<std::filesystem::path> RelativeDirectories;
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
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
			if (const Asset::FAssetData* Existing =
					Asset::GetAssetRegistry().FindAssetExact(NewPath))
				return {
					Asset::EAssetError::AlreadyExists,
					Existing->EntryKind == Asset::EAssetRegistryEntryKind::Redirector
						? std::format(
							"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another folder name.",
							NewPath.ToString(), Existing->RedirectDestination.ToString())
						: std::format(
							"Asset {} already exists. Choose another folder name or remove the existing asset.",
							NewPath.ToString())};
			if (Asset::FindLoadedPackage(NewPath))
				return {
					Asset::EAssetError::AlreadyExists,
					std::format(
						"A loaded package already uses {}. Close it or choose another folder name.",
						NewPath.ToString())};

			Moves.push_back({Path, NewPath});
			const std::filesystem::path AssetFile(Data.PhysicalPath);
			ManagedFiles.insert(NormalizePath(AssetFile.generic_string()));
			std::error_code Ec;
			for (std::filesystem::directory_iterator It(
					 AssetFile.parent_path(),
					 std::filesystem::directory_options::skip_permission_denied,
					 Ec),
				 End;
				 !Ec && It != End;
				 It.increment(Ec))
				if (It->is_regular_file(Ec)
					&& It->path().stem() == AssetFile.stem())
					ManagedFiles.insert(
						NormalizePath(It->path().generic_string()));
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
			if (It->is_regular_file(Ec)
				&& !ManagedFiles.contains(
					NormalizePath(It->path().generic_string())))
				return {
					Asset::EAssetError::IoError,
					std::format(
						"Folder contains an unmanaged file: {}. Move it separately before renaming the folder.",
						It->path().filename().generic_string())};
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
			const bool bExisted = std::filesystem::exists(DestinationDirectory);
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
		if (std::filesystem::exists(OldFolder))
		{
			for (std::filesystem::recursive_directory_iterator It(
					 OldFolder,
					 std::filesystem::directory_options::skip_permission_denied,
					 Ec),
				 End;
				 !Ec && It != End;
				 It.increment(Ec))
				if (It->is_directory(Ec)) OldDirectories.push_back(It->path());
			std::ranges::sort(
				OldDirectories,
				[](const auto& A, const auto& B) {
					return A.native().size() > B.native().size();
				});
			for (const auto& Directory : OldDirectories)
			{
				Ec.clear();
				std::filesystem::remove(Directory, Ec);
			}
			Ec.clear();
			std::filesystem::remove(OldFolder, Ec);
		}
		return {};
	}

	auto FContentBrowserOperations::IsManagedCompanion(
		const FContentBrowserItem& Item) const -> bool
	{
		if (Item.Kind != EContentBrowserItemKind::File) return false;
		const std::filesystem::path Source(Item.PhysicalPath);
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			const std::filesystem::path AssetFile(Data.PhysicalPath);
			if (NormalizePath(AssetFile.parent_path().generic_string())
					== NormalizePath(Source.parent_path().generic_string())
				&& AssetFile.stem() == Source.stem())
				return true;
		}
		return false;
	}

	auto FContentBrowserOperations::CreateFolder(
		std::string_view PhysicalDirectory) -> FContentBrowserOperationResult
	{
		const std::string NormalizedDirectory = NormalizePath(PhysicalDirectory);
		if (Model.PhysicalToVirtualDirectory(NormalizedDirectory).empty())
			return Failure(
				Asset::EAssetError::InvalidPath,
				"Folders can only be created inside a mounted content directory.");

		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0
				? "New Folder"
				: std::format("New Folder ({})", Suffix + 1);
			const std::filesystem::path Path =
				std::filesystem::path(NormalizedDirectory) / Name;
			if (std::filesystem::exists(Path)) continue;
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

	auto FContentBrowserOperations::CreateLevelAsset(
		std::string_view VirtualDirectory) -> FContentBrowserOperationResult
	{
		std::string Directory(VirtualDirectory);
		if (!Directory.ends_with('/')) Directory += '/';
		FAssetPath AssetPath;
		bool bFoundPath = false;
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0
				? "NewLevel"
				: std::format("NewLevel{}", Suffix + 1);
			if (!FAssetPath::TryCreate(Directory + Name, AssetPath)) continue;
			if (!Asset::GetAssetRegistry().FindAsset(AssetPath)
				&& !Asset::FindLoadedPackage(AssetPath))
			{
				bFoundPath = true;
				break;
			}
		}
		if (!bFoundPath)
			return Failure(
				Asset::EAssetError::AlreadyExists,
				"Could not find a unique level asset name in this folder.");

		DLevel* Level = nullptr;
		Asset::FAssetResult Result = Asset::CreateAsset(AssetPath, Level);
		if (!Result || !Level)
			return Result
				? Failure(
					  Asset::EAssetError::IoError,
					  "Could not create the level asset.")
				: FContentBrowserOperationResult{Result};
		Result = Asset::SavePackage(Level->GetPackage());
		if (!Result)
		{
			Asset::UnloadPackage(AssetPath);
			return {Result};
		}
		FContentBrowserOperationResult Outcome;
		Outcome.RevealAssetPath = AssetPath.ToString();
		return Outcome;
	}

	auto FContentBrowserOperations::CreateMaterialAsset(
		std::string_view VirtualDirectory,
		bool bInstance) -> FContentBrowserOperationResult
	{
		std::string Directory(VirtualDirectory);
		if (!Directory.ends_with('/')) Directory += '/';
		const std::string BaseName =
			bInstance ? "NewMaterialInstance" : "NewMaterial";
		FAssetPath AssetPath;
		bool bFoundPath = false;
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0
				? BaseName
				: std::format("{}{}", BaseName, Suffix + 1);
			if (!FAssetPath::TryCreate(Directory + Name, AssetPath)) continue;
			if (!Asset::GetAssetRegistry().FindAsset(AssetPath)
				&& !Asset::FindLoadedPackage(AssetPath))
			{
				bFoundPath = true;
				break;
			}
		}
		if (!bFoundPath)
			return Failure(
				Asset::EAssetError::AlreadyExists,
				"Could not find a unique material asset name in this folder.");

		DMaterialInterface* CreatedMaterial = nullptr;
		Asset::FAssetResult Result;
		if (bInstance)
		{
			DMaterialInstance* Instance = nullptr;
			Result = Asset::CreateAsset(AssetPath, Instance);
			CreatedMaterial = Instance;
		}
		else
		{
			DMaterial* Material = nullptr;
			Result = Asset::CreateAsset(AssetPath, Material);
			CreatedMaterial = Material;
		}
		if (!Result || !CreatedMaterial)
			return Result
				? Failure(
					  Asset::EAssetError::IoError,
					  "Could not create the material asset.")
				: FContentBrowserOperationResult{Result};
		Result = Asset::SavePackage(CreatedMaterial->GetPackage());
		if (!Result)
		{
			Asset::UnloadPackage(AssetPath);
			return {Result};
		}
		FContentBrowserOperationResult Outcome;
		Outcome.RevealAssetPath = AssetPath.ToString();
		Outcome.OpenAssetClassName =
			CreatedMaterial->GetClass()->GetQualifiedName().ToString();
		return Outcome;
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
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
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
		return Asset::FixUpRedirectors(Redirectors);
	}

	auto FContentBrowserOperations::FixUpRedirectors(
		std::span<const FAssetPath> Redirectors) -> Asset::FAssetResult
	{
		return Asset::FixUpRedirectors(Redirectors);
	}

	auto FContentBrowserOperations::FixUpAllRedirectors()
		-> Asset::FAssetResult
	{
		return Asset::FixUpAllRedirectors();
	}

	auto FContentBrowserOperations::AnalyzeDeletion(
		std::span<const FContentBrowserItem> Items,
		const std::unordered_set<std::string>& Selection,
		std::vector<std::pair<std::string, Asset::FAssetDeleteAnalysis>>& Analyses,
		std::vector<std::pair<std::string, Asset::FAssetResult>>& Errors) const
		-> void
	{
		Analyses.clear();
		Errors.clear();
		for (const FContentBrowserItem& Item : Items)
		{
			if (!Selection.contains(Item.StableId())
				|| (Item.Kind != EContentBrowserItemKind::Asset
					&& Item.Kind != EContentBrowserItemKind::Redirector))
				continue;
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
			{
				Errors.emplace_back(
					Item.StableId(),
					Asset::FAssetResult{
						Asset::EAssetError::InvalidPath,
						"Asset path is invalid."});
				continue;
			}
			Asset::FAssetDeleteAnalysis Analysis;
			Asset::FAssetResult Result =
				Asset::AnalyzeAssetDeletion(Path, Analysis);
			if (Result)
				Analyses.emplace_back(Item.StableId(), std::move(Analysis));
			else
				Errors.emplace_back(Item.StableId(), std::move(Result));
		}
	}

	auto FContentBrowserOperations::BuildDeletionPlan(
		std::span<const FContentBrowserItem> Items,
		const std::unordered_set<std::string>& Selection) const
		-> FContentDeletionPlanPtr
	{
		auto Plan = std::make_shared<FContentDeletionPlan>();
		Plan->RegistryRevision = Asset::GetAssetRegistry().GetRevision();
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
			const auto Mount = std::ranges::find_if(
				Model.GetMounts(),
				[&](const FContentBrowserModel::FMountSnapshot& Candidate) {
					return AreSamePath(PhysicalPath, Candidate.PhysicalRoot)
						|| PathUtilities::IsLexicalDescendantPath(
							PhysicalPath, Candidate.PhysicalRoot, true);
				});
			if (Mount == Model.GetMounts().end())
			{
				AddBlocker(
					EContentDeletionBlocker::OutsideMount,
					Item.Name,
					PhysicalPath,
					{},
					"Selected path is outside every mounted content root.");
				continue;
			}
			if (AreSamePath(PhysicalPath, Mount->PhysicalRoot))
				AddBlocker(
					EContentDeletionBlocker::MountRoot,
					Item.Name,
					PhysicalPath,
					{},
					"A mounted content root cannot be deleted.");
			if (!Mount->bAuthoringWritable)
				AddBlocker(
					EContentDeletionBlocker::ReadOnlyMount,
					Item.Name,
					PhysicalPath,
					{},
					"The selected mount is read-only for authoring.");
			if (!IsSameVolume(PhysicalPath, Plan->StagingVolumeRoot))
				AddBlocker(
					EContentDeletionBlocker::CrossVolumeStaging,
					Item.Name,
					PhysicalPath,
					{},
					"The selected root cannot be renamed into the Undo staging volume.");
			SelectedRoots.push_back({&Item, PhysicalPath, &*Mount});
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

		std::unordered_map<std::string, const Asset::FAssetData*> AssetsByPhysicalPath;
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
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
		std::vector<Asset::FAssetDeletionBatchBlocker> AssetBlockers;
		const Asset::FAssetResult AssetResult = Asset::AnalyzeAssetDeletionBatch(
			AssetPaths, PhysicalRoots, Plan->AssetBatch, AssetBlockers);
		if (!AssetResult)
			AddBlocker(
				EContentDeletionBlocker::InspectionFailed,
				"Assets", {}, {}, AssetResult.Message);

		std::unordered_set<std::string> CompanionPaths;
		for (const Asset::FAssetDeletionBatchEntry& Entry :
			Plan->AssetBatch.GetEntries())
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
			if (!std::filesystem::is_regular_file(CompanionPath, Ec)) continue;
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

		for (const Asset::FAssetDeletionBatchBlocker& Blocker : AssetBlockers)
		{
			EContentDeletionBlocker Kind = EContentDeletionBlocker::InspectionFailed;
			switch (Blocker.Kind)
			{
			case Asset::EAssetDeletionBatchBlocker::ExternalPersistentReference:
			case Asset::EAssetDeletionBatchBlocker::ExternalLoadedReference:
				Kind = EContentDeletionBlocker::ExternalReference;
				break;
			case Asset::EAssetDeletionBatchBlocker::RedirectorTargetNotSelected:
				Kind = EContentDeletionBlocker::RedirectorTargetNotSelected;
				break;
			case Asset::EAssetDeletionBatchBlocker::TargetRedirectorsNotSelected:
				Kind = EContentDeletionBlocker::TargetRedirectorsNotSelected;
				break;
			case Asset::EAssetDeletionBatchBlocker::LoadingPackage:
				Kind = EContentDeletionBlocker::LoadingPackage;
				break;
			case Asset::EAssetDeletionBatchBlocker::DirtyPackage:
				Kind = EContentDeletionBlocker::DirtyPackage;
				break;
			case Asset::EAssetDeletionBatchBlocker::ReferenceStoreInspectionFailed:
				Kind = EContentDeletionBlocker::ReferenceStoreInspectionFailed;
				break;
			case Asset::EAssetDeletionBatchBlocker::CompanionInspectionFailed:
				Kind = EContentDeletionBlocker::CompanionInspectionFailed;
				break;
			case Asset::EAssetDeletionBatchBlocker::CompanionOwnershipConflict:
				Kind = EContentDeletionBlocker::CompanionOwnershipConflict;
				break;
			case Asset::EAssetDeletionBatchBlocker::ExternalCompanionOwner:
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
		for (const Asset::FAssetDeletionBatchWarning& Warning :
			 Plan->AssetBatch.GetWarnings())
			Plan->Warnings.push_back({
				.DisplayName = Warning.TargetPath.ToString(),
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
			}
			Directory.Digest = Digest;
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
			|| Plan.RegistryRevision != Asset::GetAssetRegistry().GetRevision())
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
				|| Before.Digest != After.Digest)
				return false;
		}
		return true;
	}

	auto FContentBrowserOperations::DeleteEmptyFolder(
		const FContentBrowserItem& Item) const -> Asset::FAssetResult
	{
		std::error_code Ec;
		if (!std::filesystem::is_empty(Item.PhysicalPath, Ec))
			return {
				Asset::EAssetError::IoError,
				"Folders must be empty before they can be deleted. Delete or move their assets first."};
		if (!std::filesystem::remove(Item.PhysicalPath, Ec) || Ec)
			return {
				Asset::EAssetError::IoError,
				std::format("Could not delete folder: {}", Ec.message())};
		return {};
	}

	auto FContentBrowserOperations::Delete(
		std::span<const FContentBrowserItem> Items,
		const std::unordered_set<std::string>& Selection)
		-> Asset::FAssetResult
	{
		std::vector<FContentBrowserItem> Targets;
		for (const FContentBrowserItem& Item : Items)
			if (Selection.contains(Item.StableId())) Targets.push_back(Item);
		std::ranges::sort(
			Targets,
			[](const FContentBrowserItem& A, const FContentBrowserItem& B) {
				return A.Kind != EContentBrowserItemKind::Folder
					&& B.Kind == EContentBrowserItemKind::Folder;
			});
		for (const FContentBrowserItem& Item : Targets)
		{
			if (Item.Kind == EContentBrowserItemKind::Asset
				|| Item.Kind == EContentBrowserItemKind::Redirector)
			{
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
					return {
						Asset::EAssetError::InvalidPath,
						"Selected asset has an invalid path."};
				const Asset::FAssetResult Result = Asset::DeleteAsset(Path);
				if (!Result) return Result;
			}
			else if (Item.Kind == EContentBrowserItemKind::Folder)
			{
				const Asset::FAssetResult Result = DeleteEmptyFolder(Item);
				if (!Result) return Result;
			}
			else
			{
				if (IsManagedCompanion(Item))
					return {
						Asset::EAssetError::InvalidPath,
						"This file is managed by an asset. Delete the asset instead."};
				std::error_code Ec;
				if (!std::filesystem::remove(Item.PhysicalPath, Ec) || Ec)
					return {
						Asset::EAssetError::IoError,
						std::format(
							"Could not delete file: {}", Ec.message())};
			}
		}
		return {};
	}

	auto FContentBrowserOperations::ShowInExplorer(
		std::string_view PhysicalPath) const -> void
	{
#ifdef _WIN32
		const std::filesystem::path Path(PhysicalPath);
		std::filesystem::path PreferredPath = Path;
		const std::wstring WidePath = PreferredPath.make_preferred().wstring();
		if (std::filesystem::is_directory(Path))
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
} // namespace Durin
