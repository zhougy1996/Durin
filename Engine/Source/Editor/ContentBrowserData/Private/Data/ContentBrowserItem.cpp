#include "ContentBrowserItem.h"
#include "ContentBrowserDataSource.h"
#include "Data/ContentBrowserSaveDispatch.h"
#include "Data/ContentBrowserDeleteDispatch.h"
#include "Data/ContentBrowserTransferDispatch.h"
#include "DObject/StrongObjectPtr.h"
namespace Durin::ContentBrowser
{
	namespace
	{
		template<typename Predicate> auto CanOperate(const FContentBrowserItem& Item, Predicate&& Can) -> bool
		{
			const auto Items = Item.InternalItems;
			bool Success = false;
			for (const auto& Data : Items)
			{
				auto* Source = Data.GetOwnerDataSource();
				if (!Source || !Source->IsActive()) continue;
				TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
				Success |= Can(*Source, Data) && Data.GetOwnerDataSource() == Source;
			}
			return Success;
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
		const auto Items = InternalItems;
		bool Success = false;
		for (const auto& Item : Items) if (auto* Source = Item.GetOwnerDataSource())
		{
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
			Success |= Source->EditItem(Item);
		}
		return Success;
	}
	auto FContentBrowserItem::CanDuplicate() const -> bool
	{
		return CanOperate(*this, [](auto& Source, const auto& Data) { return Source.CanDuplicateItem(Data); });
	}
	auto FContentBrowserItem::Duplicate(FContentBrowserItem& Out) const -> bool
	{
		const auto Items = InternalItems;
		Out = {};
		for (const auto& Item : Items) if (auto* Source = Item.GetOwnerDataSource())
		{
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
			FContentBrowserItemData Duplicated;
			if (Source->DuplicateItem(Item, Duplicated) && Duplicated.GetOwnerDataSource() == Source) Out.InternalItems.push_back(std::move(Duplicated));
		}
		return !Out.InternalItems.empty();
	}
	auto FContentBrowserItem::CanDelete() const -> bool
	{ return CanOperate(*this, [](auto& Source, const auto& Data) { return Source.CanDeleteItem(Data); }); }
	auto FContentBrowserItem::Delete() const -> bool
	{ return Private::DispatchDeleteItems(InternalItems, [](auto* Source) { return Source && Source->IsActive(); }); }
	auto Private::DispatchDeleteItems(std::span<const FContentBrowserItemData> Items,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> bool
	{
		struct FPartition { TStrongObjectPtr<DContentBrowserDataSource> Source; std::vector<FContentBrowserItemData> Items; };
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		std::vector<FPartition> Partitions;
		for (const auto& Item : Snapshot)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source || !Admitted(Source)) continue;
			auto Found = std::ranges::find_if(Partitions, [Source](const auto& P) { return P.Source.Get() == Source; });
			if (Found == Partitions.end()) Partitions.push_back({TStrongObjectPtr<DContentBrowserDataSource>(Source), {Item}});
			else if (std::ranges::none_of(Found->Items, [&](const auto& Value) { return Value.GetContributionKey() == Item.GetContributionKey(); })) Found->Items.push_back(Item);
		}
		bool Success = false;
		for (auto& Part : Partitions)
			if (Admitted(Part.Source.Get()) && std::ranges::all_of(Part.Items, [&](const auto& Item) { return Item.GetOwnerDataSource() == Part.Source.Get(); }))
				Success |= Part.Source->BulkDeleteItems(Part.Items);
		return Success;
	}

	auto FContentBrowserItem::CanSave(EContentBrowserItemSaveFlags Flags) const -> bool
	{ return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanSaveItem(Data, Flags); }); }

	auto Private::DispatchSaveItems(std::span<const FContentBrowserItemData> Items, EContentBrowserItemSaveFlags Flags,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> bool
	{
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Sources;
		for (const auto& Item : Snapshot) if (auto* Source = Item.GetOwnerDataSource(); Source && Admitted(Source)
			&& std::ranges::none_of(Sources, [Source](const auto& Existing) { return Existing.Get() == Source; })) Sources.emplace_back(Source);
		bool Success = false;
		for (const auto& Source : Sources)
		{
			if (!Admitted(Source.Get())) continue;
			std::vector<FContentBrowserItemData> Batch;
			for (const auto& Item : Snapshot) if (Item.GetOwnerDataSource() == Source.Get()) Batch.push_back(Item);
			if (!Batch.empty()) Success |= Source->BulkSaveItems(Batch, Flags);
		}
		return Success;
	}

	auto FContentBrowserItem::Save(EContentBrowserItemSaveFlags Flags) const -> bool
	{
		return Private::DispatchSaveItems(InternalItems, Flags, [](auto* Source) { return Source && Source->IsActive(); });
	}
	auto FContentBrowserItem::CanRename(std::optional<std::string_view> Name, std::string* Error) const -> bool
	{
		const std::optional<std::string> CapturedName = Name ? std::optional(std::string(*Name)) : std::nullopt;
		return CanOperate(*this, [&](auto& Source, const auto& Data) {
			return Source.CanRenameItem(Data, CapturedName ? std::optional<std::string_view>(*CapturedName) : std::nullopt, Error);
		});
	}
	auto FContentBrowserItem::Rename(std::string_view Name, FContentBrowserItem& Out) const -> bool
	{
		const auto Items = InternalItems;
		const std::string Target(Name);
		Out = {};
		for (const auto& Item : Items)
		{
			auto* Source = Item.GetOwnerDataSource(); if (!Source || !Source->IsActive()) continue;
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
			FContentBrowserItemData Renamed;
			if (Source->RenameItem(Item, Target, Renamed) && Renamed.GetOwnerDataSource() == Source) Out.InternalItems.push_back(std::move(Renamed));
		}
		return !Out.InternalItems.empty();
	}

	auto FContentBrowserItem::CanCopy(std::string_view Dest, std::string* Error) const -> bool
	{
		const std::string CapturedDestination(Dest);
		return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanCopyItem(Data, CapturedDestination, Error); });
	}
	auto FContentBrowserItem::Copy(std::string_view Dest) const -> bool
	{
		const auto Items = InternalItems;
		const std::string Target(Dest);
		bool Success = false;
		for (const auto& Item : Items) if (auto* Source = Item.GetOwnerDataSource())
		{
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
			Success |= Source->CopyItem(Item, Target);
		}
		return Success;
	}
	auto FContentBrowserItem::CanMove(std::string_view Dest, std::string* Error) const -> bool
	{
		const std::string CapturedDestination(Dest);
		return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanMoveItem(Data, CapturedDestination, Error); });
	}
	auto FContentBrowserItem::Move(std::string_view Dest) const -> bool
	{
		const auto Items = InternalItems;
		const std::string Target(Dest);
		bool Success = false;
		for (const auto& Item : Items) if (auto* Source = Item.GetOwnerDataSource())
		{
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
			Success |= Source->MoveItem(Item, Target);
		}
		return Success;
	}
} // namespace Durin::ContentBrowser
