#include "ContentBrowserDataSource.h"
#include "Data/ContentBrowserSaveDispatch.h"
#include "Data/ContentBrowserTransferDispatch.h"
#include "DObject/StrongObjectPtr.h"
namespace Durin::ContentBrowser
{
	namespace
	{
		template<typename Predicate> auto CanOperate(const FContentBrowserItem& Item, Predicate&& Can) -> bool
		{
			if (Item.GetInternalItems().empty()) return false;
			for (const auto& Data : Item.GetInternalItems())
			{
				auto* Source = Data.GetOwnerDataSource();
				if (!Source) return false;
				TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
				if (!Can(*Source, Data) || Data.GetOwnerDataSource() != Source) return false;
			}
			return true;
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
		if (!CanDuplicate()) return false;
		auto* Source = InternalItems.front().GetOwnerDataSource();
		if (!Source) return false;
		TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
		FContentBrowserItemData Data;
		const bool Result = Source->DuplicateItem(InternalItems.front(), Data);
		Out = {};
		if (Data.GetOwnerDataSource() == Source) Out.InternalItems.push_back(std::move(Data));
		return Result;
	}
	auto FContentBrowserItem::CanDelete() const -> bool
	{
		if (InternalItems.empty()) return false;
		for (const auto& Data : InternalItems)
		{
			auto* Source = Data.GetOwnerDataSource();
			if (!Source) return false;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
			if (!Source->CanDeleteItem(Data) || Data.GetOwnerDataSource() != Source) return false;
		}
		return true;
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
	auto Private::DispatchSaveItems(std::span<const FContentBrowserItemData> Items, EContentBrowserItemSaveFlags Flags,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> FContentBrowserOperationResult
	{
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		Items = Snapshot;
		FContentBrowserOperationResult Result;
		struct FBatch
		{
			TStrongObjectPtr<DContentBrowserDataSource> Source;
			std::string Name;
			std::vector<FContentBrowserItemData> Items;
		};
		std::vector<FBatch> Batches;
		for (const auto& Item : Items)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source || !Admitted(Source))
			{
				for (const auto& Entry : Items) Result.Add(Entry.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, "Save rejected stale source membership before dispatch.");
				Result.bStopped = true;
				return Result;
			}
			auto Batch = std::ranges::find_if(Batches, [Source](const auto& B) { return B.Source.Get() == Source; });
			if (Batch == Batches.end())
			{
				Batches.push_back({TStrongObjectPtr<DContentBrowserDataSource>(Source), {}, {Item}});
			}
			else if (std::ranges::none_of(Batch->Items, [&](const auto& Entry) { return Entry.GetContributionKey() == Item.GetContributionKey(); })) Batch->Items.push_back(Item);
		}
		for (auto& Batch : Batches)
		{
			Batch.Name = Batch.Source->GetDataSourceName().ToString();
			std::ranges::sort(Batch.Items, {}, &FContentBrowserItemData::GetVirtualPath);
		}
		std::ranges::sort(Batches, {}, &FBatch::Name);
		// Source naming is virtual; validate all captured partitions again before I/O.
		for (const auto& Batch : Batches)
			if (!Admitted(Batch.Source.Get()) || std::ranges::any_of(Batch.Items, [&](const auto& Item) { return Item.GetOwnerDataSource() != Batch.Source.Get(); })) Result.bStopped = true;
		for (const auto& Batch : Batches)
		{
			const bool Current = Admitted(Batch.Source.Get()) && std::ranges::all_of(Batch.Items, [&](const auto& Item) { return Item.GetOwnerDataSource() == Batch.Source.Get(); });
			if (!Current) Result.bStopped = true;
			if (Result.bStopped || Result.bCancelled)
				for (const auto& Item : Batch.Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, "Save stopped before this source partition.");
			else Result.Append(Batch.Source->BulkSaveItems(Batch.Items, Flags));
		}
		return Result;
	}
	auto FContentBrowserItem::Save(EContentBrowserItemSaveFlags Flags) const -> FContentBrowserOperationResult
	{
		return Private::DispatchSaveItems(InternalItems, Flags, [](auto* Source) { return Source && Source->IsActive(); });
	}
	auto FContentBrowserItem::CanRename(std::string_view Name, std::string* Error) const -> bool
	{
		return CanOperate(*this, [&](auto& Source, const auto& Data) { return Source.CanRenameItem(Data, Name, Error); });
	}
	auto FContentBrowserItem::Rename(std::string_view Name, FContentBrowserItem& Out) const -> FContentBrowserOperationResult
	{
		const auto Items = InternalItems;
		Out = {};
		FContentBrowserOperationResult Result;
		struct FPrepared { TStrongObjectPtr<DContentBrowserDataSource> Source; FContentBrowserItemData Item; std::string Name; std::unique_ptr<FContentBrowserItemDataTransferContext> Context; };
		std::vector<FPrepared> Prepared;
		auto Reject = [&](std::string Error) { for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, Error); Result.bStopped = true; return Result; };
		for (const auto& Item : Items)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source) return Reject("Rename source is no longer available.");
			Prepared.push_back({TStrongObjectPtr<DContentBrowserDataSource>(Source), Item, {}, {}});
		}
		for (auto& Entry : Prepared) Entry.Name = Entry.Source->GetDataSourceName().ToString();
		std::ranges::sort(Prepared, {}, &FPrepared::Name);
		std::unordered_map<std::string, bool> Claims;
		for (auto& Entry : Prepared)
		{
			if (Entry.Item.GetOwnerDataSource() != Entry.Source.Get()) return Reject("Rename source changed during preparation.");
			Entry.Context = Entry.Source->PrepareRenameItem(Entry.Item, Name);
			std::string Error;
			if (!Entry.Context || !Entry.Context->Validate(&Error)) return Reject(Error.empty() ? "Rename is unsupported." : Error);
			for (const auto& Claim : Entry.Context->GetDestinations())
			{
				auto [Existing, Inserted] = Claims.try_emplace(Claim.Key, Claim.bDirectory);
				if (!Inserted && (!Existing->second || !Claim.bDirectory)) return Reject("Rename sources claim the same destination.");
			}
		}
		for (const auto& Entry : Prepared)
		{
			std::string Error;
			if (Entry.Item.GetOwnerDataSource() != Entry.Source.Get() || !Entry.Context->Validate(&Error)) return Reject(Error.empty() ? "Rename admission changed." : Error);
		}
		for (const auto& Entry : Prepared)
		{
			if (Entry.Item.GetOwnerDataSource() != Entry.Source.Get()) Result.bStopped = true;
			if (Result.bStopped || Result.bCancelled) Result.Add(Entry.Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, "Rename stopped before this source.");
			else
			{
				Result.Append(Entry.Context->Execute());
				auto Data = Entry.Context->GetResultItemData();
				if (Data.GetOwnerDataSource() == Entry.Source.Get()) Out.InternalItems.push_back(std::move(Data));
			}
		}
		return Result;
	}
	auto FContentBrowserItem::CanCopy(std::string_view Dest, std::string* Error) const -> bool
	{
		if (InternalItems.empty()) return false;
		for (const auto& Item : InternalItems)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source) return false;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
			if (!Source->CanCopyItem(Item, Dest, Error) || Item.GetOwnerDataSource() != Source) return false;
		}
		return true;
	}
	auto FContentBrowserItem::Copy(std::string_view Dest) const -> FContentBrowserOperationResult
	{
		return Private::DispatchTransferItems(InternalItems, Dest, false, [](auto* Source) { return Source && Source->IsActive(); });
	}
	auto FContentBrowserItem::CanMove(std::string_view Dest, std::string* Error) const -> bool
	{
		if (InternalItems.empty()) return false;
		for (const auto& Item : InternalItems)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source) return false;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
			if (!Source->CanMoveItem(Item, Dest, Error) || Item.GetOwnerDataSource() != Source) return false;
		}
		return true;
	}
	auto FContentBrowserItem::Move(std::string_view Dest) const -> FContentBrowserOperationResult
	{
		return Private::DispatchTransferItems(InternalItems, Dest, true, [](auto* Source) { return Source && Source->IsActive(); });
	}
} // namespace Durin::ContentBrowser
