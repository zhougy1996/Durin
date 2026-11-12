#include "Data/ContentBrowserItemAttributeHelpers.h"
#include "Panels/ContentBrowserQuery.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Misc/StringHelper.h"
#include "Profiling/Profiling.h"

namespace Durin::ContentBrowser::Private
{
	using StringUtils::ContainsInsensitive;

	auto ContentBrowserQuery::TypeLabel(const FContentBrowserItem& Item)
		-> std::string
	{
		FContentBrowserItemDataAttributeValue DisplayName;
		if (Item.GetItemAttribute(ContentBrowserItemAttributes::ItemTypeDisplayName, DisplayName))
			if (const auto* Name=DisplayName.GetValue<std::string>(); Name && !Name->empty()) return *Name;
		if (Item.IsFolder()) return "Folder";
		if (Item.IsRedirector()) return "Redirector";
		return (Item.IsAsset() && !Item.IsRedirector()) ? "Asset" : "File";
	}

	static auto MatchesTypeFilter(
		const FContentBrowserItem& Item, EContentBrowserTypeFilter TypeFilter) -> bool
	{
		if (TypeFilter == EContentBrowserTypeFilter::All
			|| Item.IsFolder())
			return true;
		if (TypeFilter == EContentBrowserTypeFilter::Assets)
			return (Item.IsAsset() && !Item.IsRedirector());
		if (TypeFilter == EContentBrowserTypeFilter::Files)
			return (!Item.IsFolder() && !Item.IsAsset());
		if (TypeFilter == EContentBrowserTypeFilter::Redirectors)
			return Item.IsRedirector();
		if (!((Item.IsAsset() && !Item.IsRedirector()))) return false;
		const auto Type = FindAssetTypePresentation(ItemAttributes::String(Item, ContentBrowserItemAttributes::Class));
		const auto HasCategory = [&](std::string_view Category) {
			return Type && std::ranges::any_of(Type->CategoryPaths,
				[&](const auto& Path) { return !Path.empty() && Path.front() == Category; });
		};
		// Persisted built-in filter shortcuts resolve the same definition categories as the dynamic menu.
		switch (TypeFilter)
		{
		case EContentBrowserTypeFilter::Levels: return HasCategory("Level");
		case EContentBrowserTypeFilter::StaticMeshes: return HasCategory("Meshes");
		case EContentBrowserTypeFilter::Materials: return HasCategory("Material");
		case EContentBrowserTypeFilter::Textures: return HasCategory("Texture");
		default: return !HasCategory("Level") && !HasCategory("Meshes") && !HasCategory("Material") && !HasCategory("Texture");
		}
	}

	auto ContentBrowserQuery::Project(std::shared_ptr<const FContentBrowserItemsSnapshot> Snapshot, std::string_view PhysicalDirectory, const FContentBrowserQuerySettings& Settings, std::string_view VirtualDirectory)
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
			// Package bulk payloads are internal even when their owner is missing.
			// Filter the presentation, not the physical snapshot or operation scope.
			if ((!Item.IsFolder() && !Item.IsAsset())
				&& StringUtils::FoldAscii(std::filesystem::path(ItemAttributes::PhysicalPath(Item)).extension().string()) == ".dbulk")
				continue;
			if (Item.IsRedirector()
				&& !Settings.bShowRedirectors
				&& Settings.TypeFilter != EContentBrowserTypeFilter::Redirectors)
				continue;
			auto ItemPath = Item.GetVirtualPath();
			while (ItemPath.size() > 1 && ItemPath.back() == '/')
				ItemPath.pop_back();
			const std::filesystem::path Relative = !VirtualDirectory.empty() && !ItemPath.empty() ? std::filesystem::path(ItemPath).lexically_relative(VirtualDirectory) : std::filesystem::path(ItemAttributes::PhysicalPath(Item)).lexically_relative(PhysicalDirectory);
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
			const std::string_view SearchPath = Item.GetVirtualPath().empty()
				? std::string_view(ItemAttributes::PhysicalPath(Item))
				: std::string_view(Item.GetVirtualPath());
			if (bSearching && !ContainsInsensitive(Item.GetItemName(), Settings.Search)
				&& !ContainsInsensitive(SearchPath, Settings.Search)
				&& !ContainsInsensitive(Type, Settings.Search)
				&& !ContainsInsensitive(
					ItemAttributes::RedirectDestination(Item).ToString(), Settings.Search))
				continue;
			bool MatchesCategory = Settings.CategoryPath.empty() || Item.IsFolder();
			if (!MatchesCategory && (Item.IsAsset() && !Item.IsRedirector()))
				if (const auto Type = FindAssetTypePresentation(ItemAttributes::String(Item, ContentBrowserItemAttributes::Class)))
					MatchesCategory = std::ranges::any_of(Type->CategoryPaths, [&](const auto& Path) {
						return Path.size() >= Settings.CategoryPath.size() && std::equal(Settings.CategoryPath.begin(), Settings.CategoryPath.end(), Path.begin());
					});
			if (MatchesCategory && MatchesTypeFilter(Item, Settings.TypeFilter)) Indices.push_back(Index);
		}

		auto Compare = [&](size_t AIndex, size_t BIndex) {
			const auto& A = Snapshot->Items[AIndex];
			const auto& B = Snapshot->Items[BIndex];
			if (A.IsFolder()
				&& !(B.IsFolder()))
				return true;
			if (!(A.IsFolder())
				&& B.IsFolder())
				return false;
			int32 Result = 0;
			switch (Settings.SortColumn)
			{
			case EContentBrowserSortColumn::Type:
				Result = TypeLabels[AIndex].compare(TypeLabels[BIndex]);
				break;
			case EContentBrowserSortColumn::Size:
				Result = ItemAttributes::Unsigned(A, ContentBrowserItemAttributes::ItemDiskSize) < ItemAttributes::Unsigned(B, ContentBrowserItemAttributes::ItemDiskSize)
					? -1
					: ItemAttributes::Unsigned(A, ContentBrowserItemAttributes::ItemDiskSize) > ItemAttributes::Unsigned(B, ContentBrowserItemAttributes::ItemDiskSize) ? 1 : 0;
				break;
			case EContentBrowserSortColumn::Modified:
				Result = ItemAttributes::LastWriteTime(A) < ItemAttributes::LastWriteTime(B)
					? -1
					: ItemAttributes::LastWriteTime(A) > ItemAttributes::LastWriteTime(B) ? 1 : 0;
				break;
			default: Result = A.GetItemName().compare(B.GetItemName()); break;
			}
			if (Result == 0) Result = A.GetItemName().compare(B.GetItemName());
			return Settings.bSortAscending ? Result < 0 : Result > 0;
		};
		std::ranges::stable_sort(Indices, Compare);
		return {std::move(Snapshot), std::move(Indices)};
	}

	auto ContentBrowserQuery::CopySelection(const FContentBrowserItemRange& Items,
		const FContentBrowserItemKeySet& Selection) -> std::vector<FContentBrowserItem>
	{
		std::vector<FContentBrowserItem> Result;
		Result.reserve(Selection.size());
		for (const auto& Item : Items)
			if (Selection.contains(Item.GetItemKey())) Result.push_back(Item);
		return Result;
	}
}
