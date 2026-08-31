#pragma once

#include "DurinEdAPI.h"
#include "Thumbnail/AssetThumbnailPool.h"

namespace Durin::Editor
{
	// Lightweight UI reference to one canonical asset entry in a shared pool.
	class FAssetThumbnail
	{
	public:
		DURINED_API FAssetThumbnail(
			FAssetThumbnailPackageFingerprint Asset,
			uint32 RequestedWidth = 256,
			uint32 RequestedHeight = 256,
			FAssetThumbnailPool* Pool = nullptr);
		DURINED_API ~FAssetThumbnail();
		FAssetThumbnail(const FAssetThumbnail&) = delete;
		auto operator=(const FAssetThumbnail&) -> FAssetThumbnail& = delete;
		DURINED_API FAssetThumbnail(FAssetThumbnail&& Other) noexcept;
		DURINED_API auto operator=(FAssetThumbnail&& Other) noexcept -> FAssetThumbnail&;

		DURINED_API auto Reassign(FAssetThumbnailPackageFingerprint Asset) -> void;
		DURINED_API auto Request(EAssetThumbnailPriority Priority) -> void;
		DURINED_API auto Refresh() -> void;
		DURINED_API auto GetView() const -> FAssetThumbnailView;
		auto GetRequestedWidth() const -> uint32 { return RequestedWidth; }
		auto GetRequestedHeight() const -> uint32 { return RequestedHeight; }
		auto GetAssetPath() const -> const FPackagePath& { return Asset.VirtualPath; }

	private:
		auto Release() -> void;

		FAssetThumbnailPackageFingerprint Asset;
		FAssetThumbnailPool* Pool = nullptr;
		uint32 RequestedWidth = 256;
		uint32 RequestedHeight = 256;
	};
}
