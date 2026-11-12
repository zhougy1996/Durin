#include "ContentBrowserDataSource.h"
namespace Durin::ContentBrowser
{
	auto FContentBrowserItem::CanEdit() const -> bool
	{
		return InternalItems.size() == 1 && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->CanEditItem(InternalItems.front());
	}
	auto FContentBrowserItem::Edit() const -> bool
	{
		return CanEdit() && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->EditItem(InternalItems.front());
	}
	auto FContentBrowserItem::CanDuplicate() const -> bool
	{
		return InternalItems.size() == 1 && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->CanDuplicateItem(InternalItems.front());
	}
	auto FContentBrowserItem::Duplicate(FContentBrowserItem& Out) const -> bool
	{
		return CanDuplicate() && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->DuplicateItem(InternalItems.front(), Out);
	}
	auto FContentBrowserItem::CanDelete() const -> bool
	{
		return InternalItems.size() == 1 && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->CanDeleteItem(InternalItems.front());
	}

	auto FContentBrowserItem::CanSave(EContentBrowserItemSaveFlags Flags) const -> bool
	{
		if (InternalItems.empty()) return false;
		for (const auto& Data : InternalItems)
		{
			auto* Source = Data.GetOwnerDataSource();
			if (!Source || !Source->CanSaveItem(Data, Flags)) return false;
		}
		return true;
	}
	auto FContentBrowserItem::Save(EContentBrowserItemSaveFlags Flags) const -> bool
	{
		if (!CanSave(Flags)) return false;
		std::map<DContentBrowserDataSource*, std::vector<FContentBrowserItemData>> Batches;
		for (const auto& Data : InternalItems)
		{
			auto* Source = Data.GetOwnerDataSource();
			if (!Source) return false;
			Batches[Source].push_back(Data);
		}
		bool Result = true;
		for (const auto& [Source, Batch] : Batches)
		{
			const bool Current = std::ranges::all_of(Batch, [Source](const auto& Data) { return Data.GetOwnerDataSource() == Source; });
			Result = (Current && Source->BulkSaveItems(Batch, Flags)) && Result;
		}
		return Result;
	}
	auto FContentBrowserItem::CanRename(std::string_view Name, std::string* Error) const -> bool
	{
		if (InternalItems.size() != 1) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->CanRenameItem(InternalItems.front(), Name, Error);
	}
	auto FContentBrowserItem::Rename(std::string_view Name, FContentBrowserItem& Out) const -> bool
	{
		if (!CanRename(Name)) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->RenameItem(InternalItems.front(), Name, Out);
	}
	auto FContentBrowserItem::CanCopy(std::string_view Dest, std::string* Error) const -> bool
	{
		if (InternalItems.size() != 1) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->CanCopyItem(InternalItems.front(), Dest, Error);
	}
	auto FContentBrowserItem::Copy(std::string_view Dest) const -> bool
	{
		if (!CanCopy(Dest)) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->BulkCopyItems(InternalItems, Dest);
	}
	auto FContentBrowserItem::CanMove(std::string_view Dest, std::string* Error) const -> bool
	{
		if (InternalItems.size() != 1) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->CanMoveItem(InternalItems.front(), Dest, Error);
	}
	auto FContentBrowserItem::Move(std::string_view Dest) const -> bool
	{
		if (!CanMove(Dest)) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->BulkMoveItems(InternalItems, Dest);
	}
} // namespace Durin::ContentBrowser
