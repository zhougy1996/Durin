#include "Panels/ContentBrowserQuery.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Misc/StringHelper.h"
#include "Profiling/Profiling.h"

namespace Durin::Editor::ContentBrowser::Private
{
	using StringUtils::ContainsInsensitive;

	static auto ClassLeaf(std::string_view QualifiedName) -> std::string
	{
		const size_t Separator = QualifiedName.rfind("::");
		std::string Name = Separator == std::string_view::npos
			? std::string(QualifiedName)
			: std::string(QualifiedName.substr(Separator + 2));
		if (Name.starts_with('D') && Name.size() > 1) Name.erase(Name.begin());
		return Name;
	}

	auto ContentBrowserQuery::TypeLabel(const FContentBrowserItem& Item)
		-> std::string
	{
		if (Item.Kind == EContentBrowserItemKind::Folder) return "Folder";
		if (Item.Kind == EContentBrowserItemKind::Redirector) return "Redirector";
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			if (const auto Type = FindAssetTypePresentation(Item.AssetClassName)) return Type->DisplayName;
			return ClassLeaf(Item.AssetClassName);
		}
		return Item.Extension.empty()
			? "File"
			: Item.Extension.substr(1) + " file";
	}

	static auto MatchesTypeFilter(
		const FContentBrowserItem& Item, EContentBrowserTypeFilter TypeFilter) -> bool
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
		const auto Type = FindAssetTypePresentation(Item.AssetClassName);
		const EAssetCategory Category = Type ? Type->Category : EAssetCategory::Other;
		switch (TypeFilter)
		{
		case EContentBrowserTypeFilter::Levels: return Category == EAssetCategory::Level;
		case EContentBrowserTypeFilter::StaticMeshes: return Category == EAssetCategory::StaticMesh;
		case EContentBrowserTypeFilter::Materials: return Category == EAssetCategory::Material;
		case EContentBrowserTypeFilter::Textures: return Category == EAssetCategory::Texture;
		default: return Category == EAssetCategory::Other;
		}
	}

	auto ContentBrowserQuery::Project(std::shared_ptr<const FContentBrowserItemsSnapshot> Snapshot,
		std::string_view PhysicalDirectory, const FContentBrowserQuerySettings& Settings)
		-> FContentBrowserItemRange
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.Project");
		if (!Snapshot) return {};
		std::vector<size_t> Indices;
		Indices.reserve(Snapshot->Items.size());
		std::vector<std::string> TypeLabels;
		if (!Settings.Search.empty() || Settings.SortColumn == EContentBrowserSortColumn::Type)
		{
			TypeLabels.reserve(Snapshot->Items.size());
			for (const auto& Item : Snapshot->Items) TypeLabels.push_back(TypeLabel(Item));
		}
		const bool bSearching = !Settings.Search.empty();
		for (size_t Index = 0; Index < Snapshot->Items.size(); ++Index)
		{
			const auto& Item = Snapshot->Items[Index];
			if (Item.Kind == EContentBrowserItemKind::Redirector
				&& !Settings.bShowRedirectors
				&& Settings.TypeFilter != EContentBrowserTypeFilter::Redirectors)
				continue;
			const std::filesystem::path Relative =
				std::filesystem::path(Item.PhysicalPath)
					.lexically_relative(PhysicalDirectory);
			if (Relative.empty() || Relative == "."
				|| (!Relative.empty() && *Relative.begin() == ".."))
				continue;
			if (!bSearching && !Relative.parent_path().empty()) continue;
			if (!Settings.bShowHiddenFiles
				&& std::ranges::any_of(
					Relative,
					[](const std::filesystem::path& Component) {
						return Component.generic_string().starts_with('.');
					}))
				continue;

			const std::string_view Type = TypeLabels.empty() ? std::string_view{} : TypeLabels[Index];
			const std::string_view SearchPath = Item.VirtualPath.empty()
				? std::string_view(Item.PhysicalPath)
				: std::string_view(Item.VirtualPath);
			if (bSearching && !ContainsInsensitive(Item.Name, Settings.Search)
				&& !ContainsInsensitive(SearchPath, Settings.Search)
				&& !ContainsInsensitive(Type, Settings.Search)
				&& !ContainsInsensitive(
					Item.RedirectDestination.ToString(), Settings.Search))
				continue;
			if (MatchesTypeFilter(Item, Settings.TypeFilter)) Indices.push_back(Index);
		}

		auto Compare = [&](size_t AIndex, size_t BIndex) {
			const auto& A = Snapshot->Items[AIndex];
			const auto& B = Snapshot->Items[BIndex];
			if (A.Kind == EContentBrowserItemKind::Folder
				&& B.Kind != EContentBrowserItemKind::Folder)
				return true;
			if (A.Kind != EContentBrowserItemKind::Folder
				&& B.Kind == EContentBrowserItemKind::Folder)
				return false;
			int32 Result = 0;
			switch (Settings.SortColumn)
			{
			case EContentBrowserSortColumn::Type:
				Result = TypeLabels[AIndex].compare(TypeLabels[BIndex]);
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
			return Settings.bSortAscending ? Result < 0 : Result > 0;
		};
		std::ranges::stable_sort(Indices, Compare);
		return {std::move(Snapshot), std::move(Indices)};
	}

	auto ContentBrowserQuery::CopySelection(const FContentBrowserItemRange& Items,
		const std::unordered_set<std::string>& Selection) -> std::vector<FContentBrowserItem>
	{
		std::vector<FContentBrowserItem> Result;
		Result.reserve(Selection.size());
		for (const auto& Item : Items)
			if (Selection.contains(Item.StableId())) Result.push_back(Item);
		return Result;
	}
}
