#include "Panels/ContentBrowserModel.h"

#include "AssetSystem.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"

namespace Durin
{
	namespace
	{
		using StringUtils::ContainsInsensitive;

		auto NormalizePath(std::string_view Path) -> std::string
		{
			if (Path.empty()) return {};
			return std::filesystem::absolute(std::filesystem::path(Path))
				.lexically_normal()
				.generic_string();
		}

		auto ClassLeaf(std::string_view QualifiedName) -> std::string
		{
			const size_t Separator = QualifiedName.rfind("::");
			std::string Name = Separator == std::string_view::npos
				? std::string(QualifiedName)
				: std::string(QualifiedName.substr(Separator + 2));
			if (Name.starts_with('D') && Name.size() > 1) Name.erase(Name.begin());
			return Name;
		}

	} // namespace

	auto ContentBrowserModel::TypeLabel(const FContentBrowserItem& Item)
		-> std::string
	{
		if (Item.Kind == EContentBrowserItemKind::Folder) return "Folder";
		if (Item.Kind == EContentBrowserItemKind::Redirector) return "Redirector";
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const std::string ClassName = ClassLeaf(Item.AssetClassName);
			if (ClassName == "TextureCube") return "Texture Cube";
			if (ClassName == "SkeletalMesh") return "Skeletal Mesh";
			if (ClassName == "AnimationClip") return "Animation Clip";
			return ClassName;
		}
		return Item.Extension.empty()
			? "File"
			: Item.Extension.substr(1) + " file";
	}

	auto FContentBrowserModel::RefreshMountSnapshot() -> void
	{
		const auto& RegisteredMounts = PathUtilities::GetRegisteredMountPoints();
		const size_t ContentMountCount = std::ranges::count_if(
			RegisteredMounts,
			[](const PathUtilities::FMountPoint& Mount) {
				return Mount.bAutoScan;
			});
		const bool bUnchanged = ContentMountCount == MountSnapshot.size()
			&& std::ranges::equal(
				RegisteredMounts
					| std::views::filter([](const PathUtilities::FMountPoint& Mount) {
						  return Mount.bAutoScan;
					  }),
				MountSnapshot,
				[](const PathUtilities::FMountPoint& Registered,
					const FMountSnapshot& Cached) {
					return Registered.VirtualRoot == Cached.VirtualRoot
						&& Registered.GetContentDir().generic_string()
							== Cached.SourcePhysicalRoot
						&& Registered.bAuthoringWritable
							== Cached.bAuthoringWritable;
				});
		if (bUnchanged) return;

		MountSnapshot.clear();
		MountSnapshot.reserve(ContentMountCount);
		for (const PathUtilities::FMountPoint& Mount : RegisteredMounts)
		{
			if (!Mount.bAutoScan) continue;
			const std::string ContentRoot = Mount.GetContentDir().generic_string();
			MountSnapshot.push_back({
				Mount.VirtualRoot,
				ContentRoot,
				NormalizePath(ContentRoot),
				Mount.bAuthoringWritable});
		}
		DirectoryChildrenCache.clear();
		if (!CurrentPhysicalPath.empty() && !ResolveMountPath(CurrentPhysicalPath))
		{
			CurrentPhysicalPath.clear();
			CurrentVirtualPath.clear();
			ItemsSnapshot.clear();
			Items.clear();
		}
	}

	auto FContentBrowserModel::RescanRegistry() -> Asset::FAssetResult
	{
		return Asset::GetAssetRegistry().ScanMountedContent(
			Asset::EAssetRegistryScanMode::Incremental);
	}

	auto FContentBrowserModel::PhysicalToVirtualDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		const FMountPath Resolved = ResolveMountPath(PhysicalPath);
		if (!Resolved) return {};
		std::string Result = Resolved.VirtualPath;
		if (!Result.ends_with('/')) Result += '/';
		return Result;
	}

	auto FContentBrowserModel::ResolveMountPath(
		std::string_view PhysicalPath) const -> FMountPath
	{
		const PathUtilities::FAssetPathResult Classified =
			PathUtilities::ClassifyAssetPath(PhysicalPath);
		if (!Classified) return {};
		const std::string ClassifiedRoot =
			NormalizePath(Classified.Mount->GetContentDir().generic_string());
		const auto Mount = std::ranges::find_if(
			MountSnapshot,
			[&](const FMountSnapshot& Candidate) {
				return Candidate.VirtualRoot == Classified.Mount->VirtualRoot
					&& Candidate.PhysicalRoot == ClassifiedRoot;
			});
		if (Mount == MountSnapshot.end()) return {};
		return {
			.Mount = &*Mount,
			.NormalizedPhysicalPath = NormalizePath(PhysicalPath),
			.VirtualPath = Classified.NormalizedVirtualPath};
	}

	auto FContentBrowserModel::VirtualToPhysical(std::string_view VirtualPath) const
		-> std::string
	{
		std::string EntryPath(VirtualPath);
		if (!EntryPath.ends_with('/')) EntryPath += '/';
		EntryPath += "_directory_";
		const PathUtilities::FAssetPathResult Resolved =
			PathUtilities::ResolveAssetPath(EntryPath);
		return Resolved
			? NormalizePath(Resolved.PhysicalPath.parent_path().generic_string())
			: std::string{};
	}

	auto FContentBrowserModel::NavigateToPhysical(
		std::string_view PhysicalPath,
		bool bAddHistory) -> bool
	{
		RefreshMountSnapshot();
		const FMountPath Resolved = ResolveMountPath(PhysicalPath);
		if (!Resolved
			|| !std::filesystem::is_directory(Resolved.NormalizedPhysicalPath))
			return false;
		std::string Virtual = Resolved.VirtualPath;
		if (!Virtual.ends_with('/')) Virtual += '/';

		CurrentPhysicalPath = Resolved.NormalizedPhysicalPath;
		CurrentVirtualPath = Virtual;
		if (bAddHistory)
		{
			if (HistoryIndex >= 0
				&& static_cast<size_t>(HistoryIndex + 1) < NavigationHistory.size())
				NavigationHistory.resize(static_cast<size_t>(HistoryIndex + 1));
			if (NavigationHistory.empty()
				|| NavigationHistory.back() != Resolved.NormalizedPhysicalPath)
			{
				NavigationHistory.push_back(Resolved.NormalizedPhysicalPath);
				HistoryIndex = static_cast<int32>(NavigationHistory.size() - 1);
			}
		}
		RefreshItemsSnapshot();
		return true;
	}

	auto FContentBrowserModel::NavigateHistory(int32 Delta) -> bool
	{
		const int32 Target = HistoryIndex + Delta;
		if (Target < 0 || static_cast<size_t>(Target) >= NavigationHistory.size())
			return false;
		HistoryIndex = Target;
		if (NavigateToPhysical(
				NavigationHistory[static_cast<size_t>(HistoryIndex)], false))
			return true;

		NavigationHistory.erase(NavigationHistory.begin() + HistoryIndex);
		HistoryIndex =
			std::min(HistoryIndex, static_cast<int32>(NavigationHistory.size()) - 1);
		return false;
	}

	auto FContentBrowserModel::IsInsideCurrentDirectory(
		std::string_view PhysicalPath,
		bool bRecursive) const -> bool
	{
		return PathUtilities::IsLexicalDescendantPath(
			NormalizePath(PhysicalPath), CurrentPhysicalPath, bRecursive);
	}

	auto FContentBrowserModel::RevealAsset(std::string_view AssetPath)
		-> std::string
	{
		FAssetPath Path;
		if (!FAssetPath::TryCreate(AssetPath, Path)) return {};
		const Asset::FAssetData* Data =
			Asset::GetAssetRegistry().FindAssetExact(Path);
		if (!Data) return {};
		if (Data->EntryKind == Asset::EAssetRegistryEntryKind::Redirector)
			bShowRedirectors = true;
		if (!NavigateToPhysical(
				std::filesystem::path(Data->PhysicalPath)
					.parent_path()
					.generic_string()))
			return {};
		return NormalizePath(Data->PhysicalPath);
	}

	auto FContentBrowserModel::RefreshItemsSnapshot() -> void
	{
		DirectoryChildrenCache.clear();
		ItemsSnapshot.clear();
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		if (CurrentPhysicalPath.empty())
		{
			Items.clear();
			return;
		}

		std::error_code IteratorError;
		std::filesystem::recursive_directory_iterator It(
				 CurrentPhysicalPath,
				 std::filesystem::directory_options::skip_permission_denied,
				 IteratorError);
		const std::filesystem::recursive_directory_iterator End;
		if (IteratorError)
			AddEnumerationDiagnostic(
				EEnumerationDiagnosticKind::Traversal,
				CurrentPhysicalPath,
				std::format("Could not enumerate directory: {}", IteratorError.message()));
		while (!IteratorError && It != End)
		{
			const std::filesystem::directory_entry& Entry = *It;
			const std::filesystem::path EntryPath = Entry.path();
			const std::string Name = EntryPath.filename().generic_string();
			std::error_code EntryError;
			const std::filesystem::file_status Status =
				QueryEntryStatus(Entry, EntryError);
			if (EntryError)
			{
				It.disable_recursion_pending();
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Entry,
					EntryPath,
					std::format("Skipped entry because its status could not be read: {}", EntryError.message()));
			}
			else if (std::filesystem::is_symlink(Status))
			{
				It.disable_recursion_pending();
			}
			else if (std::filesystem::is_directory(Status))
			{
				ItemsSnapshot.push_back(
					{EContentBrowserItemKind::Folder,
						Name,
						PhysicalToVirtualDirectory(EntryPath.generic_string()),
						NormalizePath(EntryPath.generic_string())});
			}
			else if (std::filesystem::is_regular_file(Status)
				&& EntryPath.extension() != ".dasset")
			{
				FContentBrowserItem Item{
					EContentBrowserItemKind::File,
					Name,
					{},
					NormalizePath(EntryPath.generic_string()),
					{},
					EntryPath.extension().generic_string()};
				Item.FileSize = Entry.file_size(EntryError);
				if (!EntryError)
					Item.LastWriteTime = Entry.last_write_time(EntryError);
				if (EntryError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Entry,
						EntryPath,
						std::format("Skipped file because its metadata could not be read: {}", EntryError.message()));
				else
					ItemsSnapshot.push_back(std::move(Item));
			}

			IteratorError.clear();
			It.increment(IteratorError);
			if (IteratorError)
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					EntryPath,
					std::format("Directory traversal stopped: {}", IteratorError.message()));
		}

		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			if (!IsInsideCurrentDirectory(Data.PhysicalPath, true)) continue;
			FContentBrowserItem Item{
				Data.EntryKind == Asset::EAssetRegistryEntryKind::Redirector
					? EContentBrowserItemKind::Redirector
					: EContentBrowserItemKind::Asset,
				std::string(Path.GetAssetName()),
				Path.ToString(),
				NormalizePath(Data.PhysicalPath),
				Data.AssetClassName,
				".dasset"};
			Item.RedirectDestination = Data.RedirectDestination;
			std::error_code FileEc;
			Item.FileSize = std::filesystem::file_size(Data.PhysicalPath, FileEc);
			Item.LastWriteTime = Data.LastWriteTime;
			FAssetThumbnailSourceImage SourceImage;
			std::string ThumbnailError;
			FRenderedAssetThumbnailService& ThumbnailService =
				GetDefaultRenderedAssetThumbnailService();
			if (ThumbnailService.UsesSourceImage(Data.AssetClassName))
			{
				if (ThumbnailService.CaptureSourceImage(
						Data, SourceImage, ThumbnailError))
				{
					Item.ThumbnailIdentity = Item.VirtualPath;
					Item.ThumbnailSourcePath = SourceImage.PhysicalPath;
					Item.ThumbnailFileSize = SourceImage.FileSize;
					Item.ThumbnailLastWriteTime = SourceImage.LastWriteTime;
				}
			}
			else if (ThumbnailService.Find(Data.AssetClassName))
			{
				Item.ThumbnailIdentity = Item.VirtualPath;
				Item.ThumbnailFileSize = Data.FileSize;
				Item.ThumbnailPackageFormatVersion = Data.FormatVersion;
				Item.ThumbnailLastWriteTimeTicks = Data.LastWriteTimeTicks;
			}
			ItemsSnapshot.push_back(std::move(Item));
		}
		RebuildItems();
	}

	auto FContentBrowserModel::MatchesTypeFilter(
		const FContentBrowserItem& Item) const -> bool
	{
		if (TypeFilter == EContentBrowserTypeFilter::All
			|| Item.Kind == EContentBrowserItemKind::Folder)
			return true;
		if (TypeFilter == EContentBrowserTypeFilter::Assets)
			return Item.Kind == EContentBrowserItemKind::Asset;
		if (TypeFilter == EContentBrowserTypeFilter::Files)
			return Item.Kind == EContentBrowserItemKind::File;
		if (TypeFilter == EContentBrowserTypeFilter::Redirectors)
			return Item.Kind == EContentBrowserItemKind::Redirector;
		if (Item.Kind != EContentBrowserItemKind::Asset) return false;
		const std::string Type = ContentBrowserModel::TypeLabel(Item);
		if (TypeFilter == EContentBrowserTypeFilter::Levels)
			return Type == "Level";
		if (TypeFilter == EContentBrowserTypeFilter::StaticMeshes)
			return Type == "StaticMesh";
		if (TypeFilter == EContentBrowserTypeFilter::SkeletalAssets)
			return Type == "Skeleton" || Type == "Skeletal Mesh"
				|| Type == "Animation Clip";
		if (TypeFilter == EContentBrowserTypeFilter::Materials)
			return Type.find("Material") != std::string::npos;
		if (TypeFilter == EContentBrowserTypeFilter::Textures)
			return Type == "Texture2D" || Type == "Texture Cube";
		return Type != "Level" && Type != "StaticMesh" && Type != "Skeleton"
			&& Type != "Skeletal Mesh" && Type != "Animation Clip"
			&& Type.find("Material") == std::string::npos
			&& Type != "Texture2D" && Type != "Texture Cube";
	}

	auto FContentBrowserModel::RebuildItems() -> void
	{
		Items.clear();
		const bool bSearching = !Search.empty();
		for (const FContentBrowserItem& Item : ItemsSnapshot)
		{
			if (Item.Kind == EContentBrowserItemKind::Redirector
				&& !bShowRedirectors
				&& TypeFilter != EContentBrowserTypeFilter::Redirectors)
				continue;
			const std::filesystem::path Relative =
				std::filesystem::path(Item.PhysicalPath)
					.lexically_relative(CurrentPhysicalPath);
			if (Relative.empty() || Relative == "."
				|| Relative.native().starts_with(L".."))
				continue;
			if (!bSearching && !Relative.parent_path().empty()) continue;
			if (!bShowHiddenFiles
				&& std::ranges::any_of(
					Relative,
					[](const std::filesystem::path& Component) {
						return Component.generic_string().starts_with('.');
					}))
				continue;

			const std::string Type = ContentBrowserModel::TypeLabel(Item);
			const std::string_view SearchPath = Item.VirtualPath.empty()
				? std::string_view(Item.PhysicalPath)
				: std::string_view(Item.VirtualPath);
			if (bSearching && !ContainsInsensitive(Item.Name, Search)
				&& !ContainsInsensitive(SearchPath, Search)
				&& !ContainsInsensitive(Type, Search)
				&& !ContainsInsensitive(
					Item.RedirectDestination.ToString(), Search))
				continue;
			if (MatchesTypeFilter(Item)) Items.push_back(Item);
		}

		auto Compare = [&](const FContentBrowserItem& A,
						   const FContentBrowserItem& B) {
			if (A.Kind == EContentBrowserItemKind::Folder
				&& B.Kind != EContentBrowserItemKind::Folder)
				return true;
			if (A.Kind != EContentBrowserItemKind::Folder
				&& B.Kind == EContentBrowserItemKind::Folder)
				return false;
			int32 Result = 0;
			switch (SortColumn)
			{
			case EContentBrowserSortColumn::Type:
				Result = ContentBrowserModel::TypeLabel(A).compare(
					ContentBrowserModel::TypeLabel(B));
				break;
			case EContentBrowserSortColumn::Size:
				Result = A.FileSize < B.FileSize
					? -1
					: A.FileSize > B.FileSize ? 1 : 0;
				break;
			case EContentBrowserSortColumn::Modified:
				Result = A.LastWriteTime < B.LastWriteTime
					? -1
					: A.LastWriteTime > B.LastWriteTime ? 1 : 0;
				break;
			default: Result = A.Name.compare(B.Name); break;
			}
			if (Result == 0) Result = A.Name.compare(B.Name);
			return bSortAscending ? Result < 0 : Result > 0;
		};
		std::ranges::stable_sort(Items, Compare);
	}

	auto FContentBrowserModel::SetSearch(std::string_view InSearch) -> void
	{
		Search = InSearch;
		RebuildItems();
	}

	auto FContentBrowserModel::SetTypeFilter(EContentBrowserTypeFilter Filter) -> void
	{
		TypeFilter = Filter;
		RebuildItems();
	}

	auto FContentBrowserModel::SetSort(
		EContentBrowserSortColumn Column,
		bool bAscending) -> void
	{
		SortColumn = Column;
		bSortAscending = bAscending;
		RebuildItems();
	}

	auto FContentBrowserModel::SetShowHiddenFiles(bool bShow) -> void
	{
		bShowHiddenFiles = bShow;
		RebuildItems();
	}

	auto FContentBrowserModel::SetShowRedirectors(bool bShow) -> void
	{
		bShowRedirectors = bShow;
		RebuildItems();
	}

	auto FContentBrowserModel::GetDirectoryChildren(
		std::string_view PhysicalDirectory)
		-> std::span<const std::filesystem::path>
	{
		const std::string Physical = NormalizePath(PhysicalDirectory);
		auto [It, bInserted] = DirectoryChildrenCache.try_emplace(Physical);
		if (bInserted)
		{
			std::error_code IteratorError;
			std::filesystem::directory_iterator EntryIt(
					 Physical,
					 std::filesystem::directory_options::skip_permission_denied,
					 IteratorError);
			const std::filesystem::directory_iterator End;
			if (IteratorError)
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					Physical,
					std::format("Could not enumerate directory tree node: {}", IteratorError.message()));
			while (!IteratorError && EntryIt != End)
			{
				const std::filesystem::directory_entry& Entry = *EntryIt;
				const std::filesystem::path EntryPath = Entry.path();
				std::error_code EntryError;
				const std::filesystem::file_status Status =
					QueryEntryStatus(Entry, EntryError);
				if (EntryError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Entry,
						EntryPath,
						std::format("Skipped tree entry because its status could not be read: {}", EntryError.message()));
				else if (std::filesystem::is_directory(Status))
					It->second.push_back(EntryIt->path());
				IteratorError.clear();
				EntryIt.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Traversal,
						EntryPath,
						std::format("Directory tree traversal stopped: {}", IteratorError.message()));
			}
			std::ranges::sort(It->second);
		}
		return It->second;
	}

	auto FContentBrowserModel::QueryEntryStatus(
		const std::filesystem::directory_entry& Entry,
		std::error_code& Error) const -> std::filesystem::file_status
	{
		return EntryStatusQuery
			? EntryStatusQuery(Entry, Error)
			: Entry.symlink_status(Error);
	}

	auto FContentBrowserModel::AddEnumerationDiagnostic(
		EEnumerationDiagnosticKind Kind,
		const std::filesystem::path& Path,
		std::string Message) -> void
	{
		constexpr size_t MaximumDiagnostics = 8;
		if (EnumerationDiagnostics.size() >= MaximumDiagnostics)
		{
			++SuppressedEnumerationDiagnosticCount;
			return;
		}
		EnumerationDiagnostics.push_back({
			.Kind = Kind,
			.PhysicalPath = NormalizePath(Path.generic_string()),
			.Message = std::move(Message)});
	}

	auto FContentBrowserModel::SetSnapshotForTesting(
		std::string CurrentDirectory,
		std::vector<FContentBrowserItem> Snapshot) -> void
	{
		CurrentPhysicalPath = std::filesystem::path(CurrentDirectory)
								  .lexically_normal()
								  .generic_string();
		ItemsSnapshot = std::move(Snapshot);
		DirectoryChildrenCache.clear();
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		RebuildItems();
	}
} // namespace Durin
