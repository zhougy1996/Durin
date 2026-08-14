#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"

namespace Durin::Editor
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
			FModuleOwnedCallbackGate OwnerGate,
			std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle;
		// Process-owned/test providers only; unloadable modules must pass an owner gate.
		auto RegisterScoped(std::unique_ptr<IAssetThumbnailProvider> Provider,
			std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle
		{
			return RegisterScoped(std::move(Provider), {}, OutError);
		}
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

} // namespace Durin::Editor
