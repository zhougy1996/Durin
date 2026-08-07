#pragma once

#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/AssetThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"

namespace Durin
{
	class DMaterialInterface;

	// Captures deterministic material dependency keys for one exact material asset class.
	class FMaterialAssetThumbnailProvider final : public IAssetThumbnailProvider
	{
	public:
		DURINED_API explicit FMaterialAssetThumbnailProvider(std::string AssetClassName);

		DURINED_API auto GetRegistration() const -> FAssetThumbnailProviderRegistration override;
		DURINED_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;

	private:
		std::string AssetClassName;
	};

	// Provides stable lifecycle and budget observations without exposing preview or UI objects.
	struct FRenderedAssetThumbnailCacheStats
	{
		FRenderedAssetThumbnailPipelineStats Pipeline;
		uint64 PreviewSceneCreations = 0;
		uint64 PreviewSceneAssignments = 0;
		uint64 UploadsQueued = 0;
		uint64 UploadsCompleted = 0;
		uint64 UploadFailures = 0;
		uint64 GpuEvictions = 0;
		uint64 LiveGpuTextures = 0;
		bool bHasActiveJob = false;
		bool bHasPreviewScene = false;
	};

	// Owns all rendered-asset generation, persistence, capture, upload, and UI texture lifetime.
	class FRenderedAssetThumbnailCache
	{
	public:
		DURINED_API explicit FRenderedAssetThumbnailCache(
			FAssetThumbnailBudgets Budgets = {},
			FAssetThumbnailObjectStoreSettings StoreSettings = {});
		DURINED_API ~FRenderedAssetThumbnailCache();

		FRenderedAssetThumbnailCache(const FRenderedAssetThumbnailCache&) = delete;
		FRenderedAssetThumbnailCache& operator=(const FRenderedAssetThumbnailCache&) = delete;

		DURINED_API auto BeginFrame() -> void;
		DURINED_API auto Request(
			const FAssetThumbnailPackageFingerprint& Asset,
			EAssetThumbnailPriority Priority) -> void;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto EndFrame() -> void;
		DURINED_API auto CancelPendingRequests() -> void;
		DURINED_API auto Clear() -> void;
		DURINED_API auto GetStats() const -> FRenderedAssetThumbnailCacheStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
