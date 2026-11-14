#include "Thumbnail/AssetThumbnailRequestQueue.h"

namespace Durin
{
	auto FAssetThumbnailRequestQueue::Request(const FAssetThumbnailRequest& Request) -> bool
	{
		const auto Existing = std::ranges::find(Requests, Request.Asset.AssetPath,
			[](const auto& Item) { return Item.Asset.AssetPath; });
		if (Existing != Requests.end())
		{
			Existing->Asset = Request.Asset;
			if (Request.Priority == EAssetThumbnailPriority::Visible)
				Existing->Priority = Request.Priority;
			return true;
		}
		if (Requests.size() >= MaximumRequests) return false;
		Requests.push_back(Request);
		return true;
	}

	auto FAssetThumbnailRequestQueue::Prioritize(const FTopLevelAssetPath& AssetPath) -> void
	{
		const auto Existing = std::ranges::find(Requests, AssetPath,
			[](const auto& Item) { return Item.Asset.AssetPath; });
		if (Existing != Requests.end()) Existing->Priority = EAssetThumbnailPriority::Visible;
	}

	auto FAssetThumbnailRequestQueue::BeginFrame() -> void
	{
		for (auto& Request : Requests) Request.Priority = EAssetThumbnailPriority::Prefetch;
	}

	auto FAssetThumbnailRequestQueue::TakeNext() -> std::optional<FAssetThumbnailRequest>
	{
		if (Requests.empty()) return std::nullopt;
		auto Selected = std::ranges::find(Requests, EAssetThumbnailPriority::Visible,
			&FAssetThumbnailRequest::Priority);
		if (Selected == Requests.end()) Selected = Requests.begin();
		auto Request = std::move(*Selected);
		Requests.erase(Selected);
		return Request;
	}

	auto FAssetThumbnailRequestQueue::Remove(const FTopLevelAssetPath& AssetPath) -> void
	{
		std::erase_if(Requests, [&](const auto& Request) { return Request.Asset.AssetPath == AssetPath; });
	}
}
