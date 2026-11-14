#include "Thumbnail/AssetThumbnail.h"

#include "Thumbnail/ThumbnailManager.h"

namespace Durin
{
	FAssetThumbnail::FAssetThumbnail(FAssetData InAsset,
		FAssetThumbnailPool* InPool)
		: Asset(std::move(InAsset))
		, Pool(InPool ? InPool : &GetDefaultThumbnailManager().GetSharedPool())
	{
		Pool->AddReferencer(Asset.AssetPath);
	}

	FAssetThumbnail::~FAssetThumbnail() { Release(); }

	FAssetThumbnail::FAssetThumbnail(FAssetThumbnail&& Other) noexcept
		: Asset(std::move(Other.Asset)), Pool(std::exchange(Other.Pool, nullptr))
	{
	}

	auto FAssetThumbnail::operator=(FAssetThumbnail&& Other) noexcept -> FAssetThumbnail&
	{
		if (this == &Other) return *this;
		Release();
		Asset = std::move(Other.Asset);
		Pool = std::exchange(Other.Pool, nullptr);
		return *this;
	}

	auto FAssetThumbnail::Release() -> void
	{
		if (Pool) Pool->RemoveReferencer(Asset.AssetPath);
		Pool = nullptr;
	}

	auto FAssetThumbnail::Reassign(FAssetData InAsset) -> void
	{
		if (Asset == InAsset) return;
		if (Pool) Pool->RemoveReferencer(Asset.AssetPath);
		Asset = std::move(InAsset);
		if (Pool) Pool->AddReferencer(Asset.AssetPath);
	}

	auto FAssetThumbnail::AccessTexture(EAssetThumbnailPriority Priority) -> FAssetThumbnailView
	{
		return Pool ? Pool->AccessTexture(Asset, Priority) : FAssetThumbnailView{};
	}

	auto FAssetThumbnail::Refresh() -> void
	{
		if (Pool) Pool->Refresh(Asset.AssetPath);
	}

	auto FAssetThumbnail::GetView() const -> FAssetThumbnailView
	{
		return Pool ? Pool->Find(Asset.AssetPath) : FAssetThumbnailView{};
	}
}
