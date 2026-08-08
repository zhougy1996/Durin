#pragma once

#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/AssetThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"

namespace Durin
{
	// Owns the one live rendered-thumbnail extension registry shared by every cache.
	class FRenderedAssetThumbnailService
	{
	public:
		DURINED_API FRenderedAssetThumbnailService();
		DURINED_API ~FRenderedAssetThumbnailService();

		FRenderedAssetThumbnailService(const FRenderedAssetThumbnailService&) = delete;
		FRenderedAssetThumbnailService& operator=(const FRenderedAssetThumbnailService&) = delete;

		DURINED_API auto RegisterScoped(
			std::unique_ptr<IAssetThumbnailProvider> Provider,
			std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle;
		DURINED_API auto Find(std::string_view AssetClassName) const
			-> FAssetThumbnailProviderHandle;
		DURINED_API auto UsesSourceImage(std::string_view AssetClassName) const -> bool;
		DURINED_API auto CaptureSourceImage(
			const Asset::FAssetData& Asset,
			FAssetThumbnailSourceImage& OutSource,
			std::string& OutError) const -> bool;
		DURINED_API auto Shutdown() -> void;

	private:
		friend class FRenderedAssetThumbnailCache;
		FAssetThumbnailProviderRegistry Registry;
	};

	// Returns the process-wide service used by compatibility cache construction and MainFrame composition.
	DURINED_API auto GetDefaultRenderedAssetThumbnailService()
		-> FRenderedAssetThumbnailService&;

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

	// Owns provider-neutral rendered generation, persistence, capture, upload, and UI texture lifetime.
	class FRenderedAssetThumbnailCache
	{
	public:
		DURINED_API explicit FRenderedAssetThumbnailCache(
			FAssetThumbnailBudgets Budgets = {},
			FAssetThumbnailObjectStoreSettings StoreSettings = {});
		DURINED_API explicit FRenderedAssetThumbnailCache(
			FRenderedAssetThumbnailService& Service,
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
