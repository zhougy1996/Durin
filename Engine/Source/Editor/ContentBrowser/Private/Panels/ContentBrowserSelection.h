#pragma once

#include "CoreMinimal.h"

#include "Panels/ContentBrowserChanges.h"
#include "Panels/ContentBrowserQuery.h"

namespace Durin::ContentBrowser::Private
{
	// Selection identities survive captures; only a completed projection repairs visibility.
	struct FContentBrowserSelection
	{
		FContentBrowserItemKeySet Selected;
		FContentBrowserItemKey Anchor;
		auto Repair(const FContentBrowserItemRange& Items) -> void
		{
			std::erase_if(Selected, [&](const auto& Id) {
				return std::ranges::none_of(Items, [&](const auto& Item) { return Item.GetItemKey() == Id; });
			});
			if (!Selected.contains(Anchor)) Anchor = {};
		}
		auto Select(const FContentBrowserItemRange& Items, size_t Index, bool Extend, bool Toggle) -> void
		{
			if (Index >= Items.size()) return;
			const auto& Id = Items[Index].GetItemKey();
			if (Extend && Anchor.IsValid())
			{
				const auto It = std::ranges::find_if(Items, [&](const auto& Item) { return Item.GetItemKey() == Anchor; });
				if (It != Items.end())
				{
					const size_t Start = static_cast<size_t>(std::distance(Items.begin(), It));
					if (!Toggle) Selected.clear();
					for (size_t I = std::min(Start, Index); I <= std::max(Start, Index); ++I)
						Selected.insert(Items[I].GetItemKey());
					return;
				}
			}
			if (Toggle) { if (!Selected.erase(Id)) Selected.insert(Id); }
			else { Selected.clear(); Selected.insert(Id); }
			Anchor = Id;
		}
		// Update ownership is mandatory even when the logical key omits folder owners.
		static auto Migrate(FContentBrowserItemKey& Key, const FContentBrowserItemDataUpdate& Update) -> bool
		{
			const auto& Data = Update.GetItemData();
			const bool Moved = Update.GetUpdateType() == EContentBrowserItemUpdateType::Moved;
			if (!Key.IsValid() || !Data.GetOwnerDataSource()
				|| (!Moved && Update.GetUpdateType() != EContentBrowserItemUpdateType::Removed)) return false;
			if (Key.GetItemType() == EContentBrowserItemFlags::Type_File && Key.GetOwnerKey() != Data.GetOwnerKey()) return false;
			const FContentBrowserItemDataKey Previous(Data.GetItemType(), Moved ? Update.GetPreviousVirtualPath() : Data.GetVirtualPath());
			const auto& OldPath = Previous.GetVirtualPath();
			const auto& Path = Key.GetVirtualPath();
			const bool Folder = Data.GetItemType() == EContentBrowserItemFlags::Type_Folder;
			if (!(Folder ? ContentBrowserChanges::WithinAssetDirectory(Path, OldPath)
				: Key.GetItemType() == Previous.GetItemType() && Path == OldPath)) return false;
			Key = Moved ? FContentBrowserItemKey(Key.GetItemType(), Data.GetVirtualPath() + Path.substr(OldPath.size()), Key.GetOwnerKey())
				: FContentBrowserItemKey{};
			return true;
		}
		static auto HasOtherFolderContribution(const FContentBrowserItemKey& Key, const FContentBrowserItemDataUpdate& Update,
			std::span<const FContentBrowserItem> Items) -> bool
		{
			if (Key.GetItemType() != EContentBrowserItemFlags::Type_Folder) return false;
			const auto Row = std::ranges::find_if(Items, [&](const auto& Item) { return Item.GetItemKey() == Key; });
			return Row != Items.end() && std::ranges::any_of(Row->GetInternalItems(), [&](const auto& Data) {
				return Data.GetOwnerDataSource() && Data.GetOwnerKey() != Update.GetItemData().GetOwnerKey();
			});
		}
		auto Apply(const FContentBrowserItemDataUpdate& Update, std::span<const FContentBrowserItem> Items = {}) -> void
		{
			FContentBrowserItemKeySet Next;
			for (auto Key : Selected)
			{
				if (HasOtherFolderContribution(Key, Update, Items)) Next.insert(Key);
				Migrate(Key, Update);
				if (Key.IsValid()) Next.insert(std::move(Key));
			}
			Selected = std::move(Next);
			if (!HasOtherFolderContribution(Anchor, Update, Items)) Migrate(Anchor, Update);
		}
	};
}
