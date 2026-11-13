#pragma once
#include "CoreMinimal.h"
#include "Thumbnail/AssetThumbnailKey.h"
namespace Durin
{
	// Identifies one request independently from renderer-owned generation data.
	struct FAssetThumbnailRequest
	{
		FAssetThumbnailPackageFingerprint Asset;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
		uint64 RequestSerial = 0;
	};

	// Provides cooperative cancellation shared by scheduler and asynchronous completions.
	class FAssetThumbnailCancellation
	{
	public:
		DURINED_API FAssetThumbnailCancellation();

		DURINED_API auto Cancel() const -> void;
		DURINED_API auto IsCancelled() const -> bool;

	private:
		friend class DThumbnailManager;
		std::shared_ptr<std::atomic<bool>> State;
	};

	struct FAssetThumbnailGenerationRequest
	{
		FAssetThumbnailKeyInput KeyInput;
		FAssetThumbnailCancellation Cancellation;
		uint64 RendererGeneration = 0;
		uint64 RequestSerial = 0;
		bool bHasTransparency = true;
	};
}
