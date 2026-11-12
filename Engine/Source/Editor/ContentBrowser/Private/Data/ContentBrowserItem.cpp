#include "ContentBrowserDataSource.h"
#include "DObject/StrongObjectPtr.h"
namespace Durin::ContentBrowser
{
	namespace
	{
		auto OperationItem(const FContentBrowserItem& Item) -> const FContentBrowserItemData*
		{
			const auto* Primary = Item.GetPrimaryInternalItem();
			auto* Source = Primary ? Primary->GetOwnerDataSource() : nullptr;
			if (!Source) return nullptr;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
			for (const auto& Data : Item.GetInternalItems())
				if (!Data.GetOwnerDataSource() || !Source->AreItemsEquivalentForOperations(*Primary, Data)) return nullptr;
			return Primary->GetOwnerDataSource() == Source ? Primary : nullptr;
		}
		template<typename Predicate> auto CanOperate(const FContentBrowserItem& Item, Predicate&& Can) -> bool
		{
			if (!OperationItem(Item)) return false;
			for (const auto& Data : Item.GetInternalItems())
			{
				auto* Source = Data.GetOwnerDataSource();
				if (!Source) return false;
				TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
				if (!Can(*Source, Data) || Data.GetOwnerDataSource() != Source) return false;
			}
			return OperationItem(Item) != nullptr;
		}
	}

	FContentBrowserItemDataKey::FContentBrowserItemDataKey(const FContentBrowserItemData& Data)
		: FContentBrowserItemDataKey(Data.GetItemType(), Data.GetVirtualPath()) {}
	FContentBrowserItemKey::FContentBrowserItemKey(const FContentBrowserItemData& Data)
		: FContentBrowserItemKey(Data.GetItemType(), Data.GetVirtualPath(), Data.GetOwnerKey()) {}
	FContentBrowserItemKey::FContentBrowserItemKey(const FContentBrowserItem& Item)
		: FContentBrowserItemKey(Item.GetItemKey()) {}
	auto FContentBrowserItem::GetItemAttribute(std::string_view Key, FContentBrowserItemDataAttributeValue& Out, bool IncludeMetaData) const
		-> bool
	{
		Out = {};
		for (const auto& Data : InternalItems)
			if (Data.GetItemAttribute(Key, Out, IncludeMetaData))
				return true;
		return false;
	}
	auto FContentBrowserItem::GetItemAttributes(FContentBrowserItemDataAttributeValues& Out, bool IncludeMetaData) const -> bool
	{
		Out.clear();
		for (const auto& Data : InternalItems)
		{
			FContentBrowserItemDataAttributeValues Values;
			if (Data.GetItemAttributes(Values, IncludeMetaData))
				for (auto& [Key, Value] : Values)
					if (Value.IsValid())
						Out.try_emplace(std::move(Key), std::move(Value));
		}
		return !Out.empty();
	}
	auto FContentBrowserItem::GetItemPhysicalPath(std::string& Out) const -> bool
	{
		Out.clear();
		for (const auto& Data : InternalItems)
			if (Data.GetItemPhysicalPath(Out))
				return true;
		return false;
	}
	auto FContentBrowserItem::CanEdit() const -> bool
	{
		return CanOperate(*this, [](auto& Source, const auto& Data) { return Source.CanEditItem(Data); });
	}
	auto FContentBrowserItem::Edit() const -> bool
	{
		return CanEdit() && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->EditItem(InternalItems.front());
	}
	auto FContentBrowserItem::CanDuplicate() const -> bool
	{
		return CanOperate(*this, [](auto& Source, const auto& Data) { return Source.CanDuplicateItem(Data); });
	}
	auto FContentBrowserItem::Duplicate(FContentBrowserItem& Out) const -> bool
	{
		return CanDuplicate() && InternalItems.front().GetOwnerDataSource() && InternalItems.front().GetOwnerDataSource()->DuplicateItem(InternalItems.front(), Out);
	}
	auto FContentBrowserItem::CanDelete() const -> bool
	{
		return CanOperate(*this, [](auto& Source, const auto& Data) { return Source.CanDeleteItem(Data); });
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
		return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanRenameItem(Data, Name, Error); });
	}
	auto FContentBrowserItem::Rename(std::string_view Name, FContentBrowserItem& Out) const -> bool
	{
		if (!CanRename(Name)) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->RenameItem(InternalItems.front(), Name, Out);
	}
	auto FContentBrowserItem::CanCopy(std::string_view Dest, std::string* Error) const -> bool
	{
		return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanCopyItem(Data, Dest, Error); });
	}
	auto FContentBrowserItem::Copy(std::string_view Dest) const -> bool
	{
		if (!CanCopy(Dest)) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->BulkCopyItems(InternalItems, Dest);
	}
	auto FContentBrowserItem::CanMove(std::string_view Dest, std::string* Error) const -> bool
	{
		return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanMoveItem(Data, Dest, Error); });
	}
	auto FContentBrowserItem::Move(std::string_view Dest) const -> bool
	{
		if (!CanMove(Dest)) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		return Source && Source->BulkMoveItems(InternalItems, Dest);
	}
} // namespace Durin::ContentBrowser
