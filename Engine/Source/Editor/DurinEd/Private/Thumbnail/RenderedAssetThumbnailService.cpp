#include "Thumbnail/RenderedAssetThumbnailService.h"

namespace Durin::Editor
{
	FRenderedAssetThumbnailService::FRenderedAssetThumbnailService() = default;

	FRenderedAssetThumbnailService::~FRenderedAssetThumbnailService()
	{
		Shutdown();
	}

	auto FRenderedAssetThumbnailService::RegisterScoped(
		std::unique_ptr<IAssetThumbnailProvider> Provider,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle
	{
		return Registry.RegisterScoped(
			std::move(Provider), std::move(OwnerGate), OutError);
	}

	auto FRenderedAssetThumbnailService::Find(std::string_view AssetClassName) const
		-> FAssetThumbnailProviderHandle
	{
		return Registry.Find(AssetClassName);
	}

	auto FRenderedAssetThumbnailService::CaptureSourceImage(
		const Asset::FAssetData& Asset,
		FAssetThumbnailSourceImage& OutSource,
		std::string& OutError) const -> bool
	{
		return Registry.CaptureSourceImage(Asset, OutSource, OutError);
	}

	auto FRenderedAssetThumbnailService::UsesSourceImage(
		std::string_view AssetClassName) const -> bool
	{
		return Registry.UsesSourceImage(AssetClassName);
	}

	auto FRenderedAssetThumbnailService::Shutdown() -> void
	{
		Registry.Shutdown();
	}

	auto GetDefaultRenderedAssetThumbnailService()
		-> FRenderedAssetThumbnailService&
	{
		static FRenderedAssetThumbnailService Service;
		return Service;
	}

} // namespace Durin::Editor
