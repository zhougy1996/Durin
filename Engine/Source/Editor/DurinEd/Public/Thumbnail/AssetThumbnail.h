#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/Catalog.h"

#include "DurinEdAPI.h"
#include "Thumbnail/AssetThumbnailPool.h"

namespace Durin
{
	// Lightweight UI reference to one canonical asset entry in a shared pool.
	class FAssetThumbnail
	{
	public:
		DURINED_API FAssetThumbnail(
			FAssetData Asset,
			FAssetThumbnailPool* Pool = nullptr);
		DURINED_API ~FAssetThumbnail();
		FAssetThumbnail(const FAssetThumbnail&) = delete;
		auto operator=(const FAssetThumbnail&) -> FAssetThumbnail& = delete;
		DURINED_API FAssetThumbnail(FAssetThumbnail&& Other) noexcept;
		DURINED_API auto operator=(FAssetThumbnail&& Other) noexcept -> FAssetThumbnail&;

		DURINED_API auto Reassign(FAssetData Asset) -> void;
		DURINED_API auto AccessTexture(EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Visible) -> FAssetThumbnailView;
		DURINED_API auto Refresh() -> void;
		DURINED_API auto GetView() const -> FAssetThumbnailView;
		auto GetAssetPath() const -> const FTopLevelAssetPath& { return Asset.AssetPath; }

	private:
		auto Release() -> void;

		FAssetData Asset;
		FAssetThumbnailPool* Pool = nullptr;
	};
}
