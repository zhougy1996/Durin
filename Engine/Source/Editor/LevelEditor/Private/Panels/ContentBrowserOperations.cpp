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
			if (Asset::GetAssetRegistry().FindAsset(NewPath)
				|| Asset::FindLoadedPackage(NewPath))
				return {
					Asset::EAssetError::AlreadyExists,
					std::format("Asset {} already exists.", NewPath.ToString())};

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

		const Asset::FAssetResult MoveResult = MoveAssets(Moves);
		if (!MoveResult) return MoveResult;
		for (const std::filesystem::path& RelativeDirectory : RelativeDirectories)
		{
			Ec.clear();
			std::filesystem::create_directories(
				NewFolder / RelativeDirectory, Ec);
			if (!Ec) continue;

			std::vector<FEditorAssetMove> RollbackMoves;
			RollbackMoves.reserve(Moves.size());
			for (auto It = Moves.rbegin(); It != Moves.rend(); ++It)
				RollbackMoves.push_back({It->NewPath, It->OldPath});
			const Asset::FAssetResult RollbackResult =
				MoveAssets(RollbackMoves);
			return {
				Asset::EAssetError::IoError,
				std::format(
					"Could not recreate an empty directory: {}{}",
					Ec.message(),
					RollbackResult
						? ""
						: std::format(
							  " Asset rollback also failed: {}",
							  RollbackResult.Message))};
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
				|| Item.Kind != EContentBrowserItemKind::Asset)
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
			if (Item.Kind == EContentBrowserItemKind::Asset)
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
