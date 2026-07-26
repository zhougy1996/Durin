#pragma once

#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	class DMaterialInterface;
	class DStaticMesh;
	class PrimitiveSceneProxy;

	// Builds the shared material-on-mesh proxy used by interactive and rendered previews.
	DURINED_API auto CreateMaterialPreviewPrimitive(
		DStaticMesh* Mesh,
		DMaterialInterface* Material,
		uint64 ComponentRevision,
		std::string& OutError) -> std::unique_ptr<PrimitiveSceneProxy>;

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

	// Owns Material and MaterialInstance generation, persistence, capture, upload, and UI texture lifetime.
	class FMaterialAssetThumbnailCache
	{
	public:
		DURINED_API explicit FMaterialAssetThumbnailCache(
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FMaterialAssetThumbnailCache();

		FMaterialAssetThumbnailCache(const FMaterialAssetThumbnailCache&) = delete;
		FMaterialAssetThumbnailCache& operator=(const FMaterialAssetThumbnailCache&) = delete;

		DURINED_API auto BeginFrame() -> void;
		DURINED_API auto Request(
			const FAssetThumbnailPackageFingerprint& Asset,
			EAssetThumbnailPriority Priority) -> void;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto EndFrame() -> void;
		DURINED_API auto CancelPendingRequests() -> void;
		DURINED_API auto Clear() -> void;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
