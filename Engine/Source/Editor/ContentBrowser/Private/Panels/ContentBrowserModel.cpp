#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Panels/ContentBrowserItemView.h"

#include "Asset.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"
#include "Thumbnail/AssetThumbnailProvider.h"

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		using StringUtils::ContainsInsensitive;
		using ContentBrowserFilesystem::NormalizePath;
		using ContentBrowserItemView::ClassLeaf;

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
		std::vector<FMountSnapshot> NextMountSnapshot;
		NextMountSnapshot.reserve(ContentMountCount);
		for (const PathUtilities::FMountPoint& Mount : RegisteredMounts)
		{
			if (!Mount.bAutoScan) continue;
			const std::string ContentRoot = Mount.GetContentDir().generic_string();
			NextMountSnapshot.push_back({
				Mount.VirtualRoot,
				ContentRoot,
				NormalizePath(ContentRoot),
				Mount.bContentWritable});
		}
		const auto GameMount = std::ranges::find(
			NextMountSnapshot, std::string_view{"/Game/"}, &FMountSnapshot::VirtualRoot);
		const auto EngineMount = std::ranges::find(
			NextMountSnapshot, std::string_view{"/Engine/"}, &FMountSnapshot::VirtualRoot);
		if (GameMount != NextMountSnapshot.end()
			&& EngineMount != NextMountSnapshot.end()
			&& EngineMount < GameMount)
			std::rotate(EngineMount, GameMount, std::next(GameMount));

		const bool bUnchanged = std::ranges::equal(
			NextMountSnapshot,
			MountSnapshot,
			[](const FMountSnapshot& A, const FMountSnapshot& B) {
				return A.VirtualRoot == B.VirtualRoot
					&& A.SourcePhysicalRoot == B.SourcePhysicalRoot
					&& A.PhysicalRoot == B.PhysicalRoot
					&& A.bContentWritable == B.bContentWritable;
			});
		if (bUnchanged) return;

		MountSnapshot = std::move(NextMountSnapshot);
		DirectoryChildrenCache.clear();
		RequestedDirectoryChildrenSnapshots.clear();
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
		const Asset::FAssetCatalogRefreshResult Refresh =
			Asset::RefreshAssetCatalog(
				Asset::EAssetRegistryScanMode::Incremental);
		if (Refresh) return {};
		return Refresh.Errors.empty()
			? Asset::FAssetResult{
				Asset::EAssetError::IoError,
				"Asset catalog refresh was incomplete."}
			: Refresh.Errors.front();
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
		if (!Resolved || !IsDirectoryAvailable(Resolved.NormalizedPhysicalPath))
			return false;
		if (Resolved.NormalizedPhysicalPath == CurrentPhysicalPath)
			return true;
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
		const Asset::FAssetCatalogEntry Entry = Asset::FindAssetExact(Path);
		const Asset::FAssetData* Data = Entry.Data ? &*Entry.Data : nullptr;
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
		bSnapshotInjectedForTesting = false;
		DirectoryChildrenCache.clear();
		RequestedDirectoryChildrenSnapshots.clear();
		ItemsSnapshot.clear();
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		if (CurrentPhysicalPath.empty())
		{
			Items.clear();
			return;
		}

		auto AppendFilesystemEntry =
			[&](const std::filesystem::directory_entry& Entry) -> bool
		{
			const std::filesystem::path EntryPath = Entry.path();
			const std::string Name = EntryPath.filename().generic_string();
			std::error_code EntryError;
			const std::filesystem::file_status Status =
				QueryEntryStatus(Entry, EntryError);
			if (EntryError)
			{
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Entry,
					EntryPath,
					std::format("Skipped entry because its status could not be read: {}", EntryError.message()));
				return true;
			}
			else if (std::filesystem::is_symlink(Status))
				return true;
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
			return false;
		};

		std::error_code IteratorError;
		if (Search.empty())
		{
			std::filesystem::directory_iterator It(
				CurrentPhysicalPath,
				std::filesystem::directory_options::skip_permission_denied,
				IteratorError);
			const std::filesystem::directory_iterator End;
			if (IteratorError)
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					CurrentPhysicalPath,
					std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End)
			{
				const std::filesystem::path EntryPath = It->path();
				AppendFilesystemEntry(*It);
				IteratorError.clear();
				It.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Traversal,
						EntryPath,
						std::format("Directory traversal stopped: {}", IteratorError.message()));
			}
		}
		else
		{
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
				const std::filesystem::path EntryPath = It->path();
				if (AppendFilesystemEntry(*It)) It.disable_recursion_pending();
				IteratorError.clear();
				It.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Traversal,
						EntryPath,
						std::format("Directory traversal stopped: {}", IteratorError.message()));
			}
		}

		RefreshAssetDirectoryIndex();
		if (Search.empty())
		{
			if (const auto It = AssetDirectoryIndex.find(CurrentPhysicalPath);
				It != AssetDirectoryIndex.end())
			{
				for (const FIndexedAsset& Asset : It->second)
					AppendAssetItem(*Asset.Path, *Asset.Data);
			}
		}
		else
		{
			for (const auto& [Directory, Assets] : AssetDirectoryIndex)
			{
				if (Directory != CurrentPhysicalPath
					&& !PathUtilities::IsLexicalDescendantPath(
						Directory, CurrentPhysicalPath, true))
					continue;
				for (const FIndexedAsset& Asset : Assets)
					AppendAssetItem(*Asset.Path, *Asset.Data);
			}
		}
		RebuildItems();
	}

	auto FContentBrowserModel::RefreshAssetDirectoryIndex() -> void
	{
		const uint64 Revision = Asset::GetAssetCatalogRevision();
		if (AssetCatalogSnapshot.Revision == Revision) return;

		AssetCatalogSnapshot = Asset::CaptureAssetCatalogSnapshot();
		AssetDirectoryIndex.clear();
		for (const auto& [Path, Data] : AssetCatalogSnapshot.Assets)
		{
			const std::string Directory = NormalizePath(
				std::filesystem::path(Data.PhysicalPath)
					.parent_path()
					.generic_string());
			AssetDirectoryIndex[Directory].push_back({&Path, &Data});
		}
	}

	auto FContentBrowserModel::AppendAssetItem(
		const FAssetPath& Path,
		const Asset::FAssetData& Data) -> void
	{
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
		::Durin::Editor::FAssetThumbnailSourceImage SourceImage;
		std::string ThumbnailError;
		::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService =
			::Durin::Editor::GetDefaultAssetThumbnailProviderRegistry();
		if (ThumbnailService.UsesSourceImage(Data.AssetClassName))
		{
			if (ThumbnailService.CaptureSourceImage(Data, SourceImage, ThumbnailError))
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
				|| (!Relative.empty() && *Relative.begin() == ".."))
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
		const bool bScopeChanged = Search.empty() != InSearch.empty();
		Search = InSearch;
		if (bScopeChanged && !bSnapshotInjectedForTesting)
		{
			RefreshItemsSnapshot();
			return;
		}
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
		std::string_view PhysicalDirectory) const
		-> std::span<const std::filesystem::path>
	{
		const std::string Physical = NormalizePath(PhysicalDirectory);
		const auto It = DirectoryChildrenCache.find(Physical);
		if (It != DirectoryChildrenCache.end()) return It->second;
		static const std::vector<std::filesystem::path> Empty;
		return Empty;
	}

	auto FContentBrowserModel::HasDirectoryChildrenSnapshot(
		std::string_view PhysicalDirectory) const -> bool
	{
		return DirectoryChildrenCache.contains(NormalizePath(PhysicalDirectory));
	}

	auto FContentBrowserModel::RequestDirectoryChildrenSnapshot(
		std::string_view PhysicalDirectory) -> void
	{
		const std::string Physical = NormalizePath(PhysicalDirectory);
		if (!DirectoryChildrenCache.contains(Physical))
			RequestedDirectoryChildrenSnapshots.insert(Physical);
	}

	auto FContentBrowserModel::RefreshRequestedDirectoryChildrenSnapshots() -> void
	{
		std::vector<std::string> Requests(
			RequestedDirectoryChildrenSnapshots.begin(),
			RequestedDirectoryChildrenSnapshots.end());
		RequestedDirectoryChildrenSnapshots.clear();
		for (const std::string& Physical : Requests)
		{
			auto [Cache, bInserted] = DirectoryChildrenCache.try_emplace(Physical);
			if (!bInserted) continue;
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
					Cache->second.push_back(EntryPath);
				IteratorError.clear();
				EntryIt.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Traversal,
						EntryPath,
						std::format("Directory tree traversal stopped: {}", IteratorError.message()));
			}
			std::ranges::sort(Cache->second);
		}
	}

	auto FContentBrowserModel::FindNearestAvailableDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		std::filesystem::path Directory = NormalizePath(PhysicalPath);
		while (!Directory.empty() && !IsDirectoryAvailable(Directory))
		{
			const std::filesystem::path Parent = Directory.parent_path();
			if (Parent == Directory) return {};
			Directory = Parent;
		}
		return Directory.generic_string();
	}

	auto FContentBrowserModel::QueryEntryStatus(
		const std::filesystem::directory_entry& Entry,
		std::error_code& Error) const -> std::filesystem::file_status
	{
		return EntryStatusQuery
			? EntryStatusQuery(Entry, Error)
			: Entry.symlink_status(Error);
	}

	auto FContentBrowserModel::QueryPathStatus(
		const std::filesystem::path& Path,
		std::error_code& Error) const -> std::filesystem::file_status
	{
		return PathStatusQuery
			? PathStatusQuery(Path, Error)
			: std::filesystem::status(Path, Error);
	}

	auto FContentBrowserModel::IsDirectoryAvailable(
		const std::filesystem::path& Path) const -> bool
	{
		std::error_code Error;
		const std::filesystem::file_status Status = QueryPathStatus(Path, Error);
		return !Error && std::filesystem::is_directory(Status);
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
		bSnapshotInjectedForTesting = true;
		DirectoryChildrenCache.clear();
		RequestedDirectoryChildrenSnapshots.clear();
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		RebuildItems();
	}
} // namespace Durin::Editor::ContentBrowser::Private
