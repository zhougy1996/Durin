#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/Catalog.h"
#include "Thumbnail/AssetThumbnailTypes.h"

namespace Durin
{
	struct FAssetThumbnailRequest
	{
		FAssetData Asset;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
	};

	// Holds only pending demand. Admitted work and its lifetime belong to the pool.
	class FAssetThumbnailRequestQueue
	{
	public:
		explicit FAssetThumbnailRequestQueue(uint32 InMaximumRequests = 512)
			: MaximumRequests(InMaximumRequests) {}

		// False means capacity is exhausted; a later access can retry.
		DURINED_API auto Request(const FAssetThumbnailRequest& Request) -> bool;
		DURINED_API auto Prioritize(const FTopLevelAssetPath& AssetPath) -> void;
		DURINED_API auto BeginFrame() -> void;
		DURINED_API auto TakeNext() -> std::optional<FAssetThumbnailRequest>;
		DURINED_API auto Remove(const FTopLevelAssetPath& AssetPath) -> void;
		auto Num() const -> size_t { return Requests.size(); }

	private:
		uint32 MaximumRequests;
		std::vector<FAssetThumbnailRequest> Requests;
	};
}
