#include "Data/ContentBrowserTransferDispatch.h"

namespace Durin::ContentBrowser::Private
{
	auto DispatchTransferItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination, bool Move,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> FContentBrowserOperationResult
	{
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		Items = Snapshot;
		FContentBrowserOperationResult Result;
		auto Reject = [&](std::string Error) {
			for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, Error);
			Result.bStopped = true;
			return Result;
		};
		struct FBatch
		{
			TStrongObjectPtr<DContentBrowserDataSource> Source;
			std::string Name;
			std::vector<FContentBrowserItemData> Items;
			std::unique_ptr<FContentBrowserItemDataTransferContext> Context;
		};
		std::vector<FBatch> Batches;
		for (const auto& Item : Items)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source || !Admitted(Source)) return Reject("Transfer rejected stale source membership.");
			auto Batch = std::ranges::find_if(Batches, [Source](const auto& Entry) { return Entry.Source.Get() == Source; });
			if (Batch == Batches.end()) Batches.push_back({TStrongObjectPtr<DContentBrowserDataSource>(Source), {}, {Item}, {}});
			else if (std::ranges::none_of(Batch->Items, [&](const auto& Entry) { return Entry.GetContributionKey() == Item.GetContributionKey(); })) Batch->Items.push_back(Item);
		}
		for (auto& Batch : Batches)
		{
			Batch.Name = Batch.Source->GetDataSourceName().ToString();
			std::ranges::sort(Batch.Items, {}, &FContentBrowserItemData::GetVirtualPath);
		}
		std::ranges::sort(Batches, {}, &FBatch::Name);
		auto Current = [&](const auto& Batch) {
			return Admitted(Batch.Source.Get()) && std::ranges::all_of(Batch.Items, [&](const auto& Item) { return Item.GetOwnerDataSource() == Batch.Source.Get(); });
		};
		std::unordered_map<std::string, bool> Claims;
		for (auto& Batch : Batches)
		{
			if (!Current(Batch)) return Reject("Transfer admission changed during preparation.");
			Batch.Context = Move ? Batch.Source->PrepareMoveItems(Batch.Items, Destination) : Batch.Source->PrepareCopyItems(Batch.Items, Destination);
			if (!Batch.Context) return Reject("A selected source does not support this transfer.");
			std::string Error;
			if (!Batch.Context->Validate(&Error) || !Current(Batch)) return Reject(Error.empty() ? "Transfer validation failed." : Error);
			for (const auto& Claim : Batch.Context->GetDestinations())
			{
				auto [Existing, Inserted] = Claims.try_emplace(Claim.Key, Claim.bDirectory);
				if (!Inserted && (!Existing->second || !Claim.bDirectory)) return Reject("Selected sources claim the same destination content.");
			}
		}
		// Repeat every admission after all virtual preparation callbacks, before any write.
		for (const auto& Batch : Batches)
		{
			std::string Error;
			if (!Current(Batch) || !Batch.Context->Validate(&Error)) return Reject(Error.empty() ? "Transfer admission changed before execution." : Error);
		}
		for (const auto& Batch : Batches)
		{
			if (!Current(Batch)) Result.bStopped = true;
			if (Result.bStopped || Result.bCancelled)
				for (const auto& Item : Batch.Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, "Transfer stopped before this source partition.");
			else Result.Append(Batch.Context->Execute());
		}
		return Result;
	}
}
