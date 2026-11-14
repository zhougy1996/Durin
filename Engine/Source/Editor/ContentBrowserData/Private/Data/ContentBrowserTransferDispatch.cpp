#include "Data/ContentBrowserTransferDispatch.h"
#include "ContentBrowserDataSource.h"
namespace Durin::ContentBrowser::Private
{
	auto DispatchTransferItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination, bool Move,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> bool
	{
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		const std::string Target(Destination);
		struct FBatch { TStrongObjectPtr<DContentBrowserDataSource> Source; std::vector<FContentBrowserItemData> Items; };
		std::vector<FBatch> Batches;
		for (const auto& Item : Snapshot)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!Source || !Admitted(Source)) continue;
			auto Batch = std::ranges::find_if(Batches, [Source](const auto& Value) { return Value.Source.Get() == Source; });
			if (Batch == Batches.end()) Batches.push_back({TStrongObjectPtr<DContentBrowserDataSource>(Source), {Item}});
			else if (std::ranges::none_of(Batch->Items, [&](const auto& Existing) { return Existing.GetContributionKey() == Item.GetContributionKey(); })) Batch->Items.push_back(Item);
		}
		bool Success = false;
		for (const auto& Batch : Batches)
		{
			if (!Admitted(Batch.Source.Get())) continue;
			std::vector<FContentBrowserItemData> Current;
			for (const auto& Item : Batch.Items) if (Item.GetOwnerDataSource() == Batch.Source.Get()) Current.push_back(Item);
			if (!Current.empty()) Success |= Move ? Batch.Source->BulkMoveItems(Current, Target) : Batch.Source->BulkCopyItems(Current, Target);
		}
		return Success;
	}
}
