#pragma once
#include "CoreMinimal.h"
#include "Thumbnail/AssetThumbnailKey.h"
namespace Durin
{
	// Lightweight pending interest; priority is consumed by queue admission.
	struct FAssetThumbnailRequest
	{
		FAssetThumbnailPackageFingerprint Asset;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
	};

	// Provides cooperative cancellation shared by scheduler and asynchronous completions.
	class FAssetThumbnailCancellation
	{
	public:
		DURINED_API FAssetThumbnailCancellation();

		DURINED_API auto Cancel() const -> void;
		DURINED_API auto IsCancelled() const -> bool;

	private:
		std::shared_ptr<std::atomic<bool>> State;
	};

	// Immutable cache/draw inputs and cooperative cancellation for admitted work.
	struct FAssetThumbnailScheduledRequest
	{
		std::string CacheKey;
		FAssetThumbnailKeyInput KeyInput;
		FAssetThumbnailCancellation Cancellation;
		bool bHasTransparency = true;
	};
}
