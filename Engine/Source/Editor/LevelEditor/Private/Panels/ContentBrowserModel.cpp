#include "Panels/ContentBrowserModel.h"

#include "AssetSystem.h"
#include "Assets/SourceImageThumbnailDecoder.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

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

		auto FindImageWithStem(const std::filesystem::path& PathWithoutExtension)
			-> std::filesystem::path
		{
			std::error_code Error;
			for (std::filesystem::directory_iterator It(
					 PathWithoutExtension.parent_path(),
					 std::filesystem::directory_options::skip_permission_denied, Error),
				 End;
				 !Error && It != End;
				 It.increment(Error))
			{
				if (!It->is_regular_file(Error)
					|| It->path().stem() != PathWithoutExtension.filename())
					continue;
				if (IsSupportedSourceImageExtension(
						It->path().extension().generic_string()))
					return It->path();
			}
			return {};
		}

		auto FindTextureSourceFile(const Asset::FAssetData& Data)
			-> std::filesystem::path
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Data.PackagePath.GetView());
			if (!Lookup || !Lookup.Mount->bAssetPackages)
				return {};
			const PathUtilities::FMountPoint& Mount = *Lookup.Mount;

			Asset::FAssetPackageInspection Inspection;
			if (Asset::InspectAssetPackage(Data.PhysicalPath, Inspection))
			{
				const Asset::FAssetPackageField* SourceField =
					Inspection.FindField("SourceImportData");
				FTexture2DSourceImportData SourceImportData;
				if (SourceField
					&& SourceField->TryReadStruct(
						FTexture2DSourceImportData::StaticStruct(), &SourceImportData)
					&& SourceImportData.HasSource())
				{
					const PathUtilities::FSourcePathResult Resolved =
						PathUtilities::ResolveSourcePath(
							SourceImportData.Source.SourcePath.Path);
					if (Resolved
						&& IsSupportedSourceImageExtension(
							Resolved.PhysicalPath.extension().generic_string()))
						return Resolved.PhysicalPath;
				}
			}

			const std::filesystem::path SourceRoot = Mount.Root / "Textures";
			if (const std::filesystem::path Direct =
					FindImageWithStem(
						SourceRoot / std::string(Data.PackagePath.GetAssetName()));
				!Direct.empty())
				return Direct;

			std::filesystem::path RelativePackage =
				std::filesystem::path(Data.PhysicalPath).lexically_relative(Mount.Root);
			RelativePackage.replace_extension();
			return FindImageWithStem(SourceRoot / RelativePackage);
		}
	} // namespace

	auto ContentBrowserModel::TypeLabel(const FContentBrowserItem& Item)
		-> std::string
	{
		if (Item.Kind == EContentBrowserItemKind::Folder) return "Folder";
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const std::string ClassName = ClassLeaf(Item.AssetClassName);
			return ClassName == "TextureCube" ? "Texture Cube" : ClassName;
		}
		return Item.Extension.empty()
			? "Source File"
			: Item.Extension.substr(1) + " file";
	}

	auto FContentBrowserModel::RefreshMountSnapshot() -> void
	{
		const auto& RegisteredMounts = PathUtilities::GetRegisteredMountPoints();
		const size_t ContentMountCount = std::ranges::count_if(
			RegisteredMounts,
			[](const PathUtilities::FMountPoint& Mount) {
				return Mount.bAssetPackages;
			});
		const bool bUnchanged = ContentMountCount == MountSnapshot.size()
			&& std::ranges::equal(
				RegisteredMounts
					| std::views::filter([](const PathUtilities::FMountPoint& Mount) {
						  return Mount.bAssetPackages;
					  }),
				MountSnapshot,
				[](const PathUtilities::FMountPoint& Registered,
					const FMountSnapshot& Cached) {
					return Registered.VirtualRoot == Cached.VirtualRoot
						&& Registered.Root.generic_string()
							== Cached.SourcePhysicalRoot;
				});
		if (bUnchanged) return;

		MountSnapshot.clear();
		MountSnapshot.reserve(ContentMountCount);
		for (const PathUtilities::FMountPoint& Mount : RegisteredMounts)
		{
			if (!Mount.bAssetPackages) continue;
			const std::string ContentRoot = Mount.Root.generic_string();
			MountSnapshot.push_back(
				{Mount.VirtualRoot, ContentRoot, NormalizePath(ContentRoot)});
		}
		DirectoryChildrenCache.clear();
	}

	auto FContentBrowserModel::RescanRegistry() -> Asset::FAssetResult
	{
		return Asset::GetAssetRegistry().ScanMountedContent(
			Asset::EAssetRegistryScanMode::Incremental);
	}

	auto FContentBrowserModel::PhysicalToVirtualDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		const PathUtilities::FAssetPathResult Classified =
			PathUtilities::ClassifyAssetPath(PhysicalPath);
		if (!Classified) return {};
		std::string Result = Classified.NormalizedVirtualPath;
		if (!Result.ends_with('/')) Result += '/';
		return Result;
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
		const std::string Normalized = NormalizePath(PhysicalPath);
		const std::string Virtual = PhysicalToVirtualDirectory(Normalized);
		if (Virtual.empty() || !std::filesystem::is_directory(Normalized))
			return false;

		CurrentPhysicalPath = Normalized;
		CurrentVirtualPath = Virtual;
		if (bAddHistory)
		{
			if (HistoryIndex >= 0
				&& static_cast<size_t>(HistoryIndex + 1) < NavigationHistory.size())
				NavigationHistory.resize(static_cast<size_t>(HistoryIndex + 1));
			if (NavigationHistory.empty() || NavigationHistory.back() != Normalized)
			{
				NavigationHistory.push_back(Normalized);
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
			Asset::GetAssetRegistry().FindAsset(Path);
		if (!Data) return {};
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
		if (CurrentPhysicalPath.empty())
		{
			Items.clear();
			return;
		}

		std::error_code Ec;
		for (std::filesystem::recursive_directory_iterator It(
				 CurrentPhysicalPath,
				 std::filesystem::directory_options::skip_permission_denied, Ec),
			 End;
			 !Ec && It != End;
			 It.increment(Ec))
		{
			const std::filesystem::directory_entry& Entry = *It;
			const std::string Name = Entry.path().filename().generic_string();
			if (Entry.is_directory(Ec))
			{
				ItemsSnapshot.push_back(
					{EContentBrowserItemKind::Folder,
						Name,
						PhysicalToVirtualDirectory(Entry.path().generic_string()),
						NormalizePath(Entry.path().generic_string())});
			}
		}

		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			if (!IsInsideCurrentDirectory(Data.PhysicalPath, true)) continue;
			FContentBrowserItem Item{
				EContentBrowserItemKind::Asset,
				std::string(Path.GetAssetName()),
				Path.ToString(),
				NormalizePath(Data.PhysicalPath),
				Data.AssetClassName,
				".dasset"};
			std::error_code FileEc;
			Item.FileSize = std::filesystem::file_size(Data.PhysicalPath, FileEc);
			Item.LastWriteTime = Data.LastWriteTime;
			if (Data.AssetClassName
				== DTexture2D::StaticClass()->GetQualifiedName().ToString())
			{
				const std::filesystem::path ThumbnailPath =
					FindTextureSourceFile(Data);
				if (!ThumbnailPath.empty())
				{
					Item.ThumbnailIdentity = Item.VirtualPath;
					Item.ThumbnailSourcePath =
						NormalizePath(ThumbnailPath.generic_string());
					FileEc.clear();
					Item.ThumbnailFileSize =
						std::filesystem::file_size(ThumbnailPath, FileEc);
					FileEc.clear();
					Item.ThumbnailLastWriteTime =
						std::filesystem::last_write_time(ThumbnailPath, FileEc);
				}
			}
			else if (
				Data.AssetClassName
					== DMaterial::StaticClass()->GetQualifiedName().ToString()
				|| Data.AssetClassName
					== DMaterialInstance::StaticClass()->GetQualifiedName().ToString()
				|| Data.AssetClassName
					== DTextureCube::StaticClass()->GetQualifiedName().ToString())
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
		if (TypeFilter == 0 || Item.Kind == EContentBrowserItemKind::Folder)
			return true;
		const std::string Type = ContentBrowserModel::TypeLabel(Item);
		if (TypeFilter == 1) return Type == "Level";
		if (TypeFilter == 2) return Type == "StaticMesh";
		if (TypeFilter == 3) return Type.find("Material") != std::string::npos;
		if (TypeFilter == 4)
			return Type == "Texture2D" || Type == "Texture Cube";
		return Item.Kind != EContentBrowserItemKind::Asset
			|| (Type != "Level" && Type != "StaticMesh"
				&& Type.find("Material") == std::string::npos
				&& Type != "Texture2D" && Type != "Texture Cube");
	}

	auto FContentBrowserModel::RebuildItems() -> void
	{
		Items.clear();
		const bool bSearching = !Search.empty();
		for (const FContentBrowserItem& Item : ItemsSnapshot)
		{
			const std::filesystem::path Relative =
				std::filesystem::path(Item.PhysicalPath)
					.lexically_relative(CurrentPhysicalPath);
			if (Relative.empty() || Relative == "."
				|| Relative.native().starts_with(L".."))
				continue;
			if (!bSearching && !Relative.parent_path().empty()) continue;
			if (!bShowSourceFiles
				&& Item.Kind == EContentBrowserItemKind::SourceFile)
				continue;
			if (!bShowSourceFiles && Item.Kind == EContentBrowserItemKind::Folder
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
				&& !ContainsInsensitive(Type, Search))
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

	auto FContentBrowserModel::SetTypeFilter(int32 Filter) -> void
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

	auto FContentBrowserModel::SetShowSourceFiles(bool bShow) -> void
	{
		bShowSourceFiles = bShow;
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
			std::error_code Ec;
			for (std::filesystem::directory_iterator EntryIt(
					 Physical,
					 std::filesystem::directory_options::skip_permission_denied, Ec),
				 End;
				 !Ec && EntryIt != End;
				 EntryIt.increment(Ec))
				if (EntryIt->is_directory(Ec))
					It->second.push_back(EntryIt->path());
			std::ranges::sort(It->second);
		}
		return It->second;
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
		RebuildItems();
	}
} // namespace Durin
