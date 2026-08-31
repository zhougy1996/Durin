#include "AssetThumbnail.h"

#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor
{
	FAssetThumbnail::FAssetThumbnail(FAssetThumbnailPackageFingerprint InAsset,
		uint32 InRequestedWidth, uint32 InRequestedHeight, FAssetThumbnailPool* InPool)
		: Asset(std::move(InAsset))
		, Pool(InPool ? InPool : &GetDefaultThumbnailManager().GetSharedPool())
		, RequestedWidth(std::max(1u, InRequestedWidth))
		, RequestedHeight(std::max(1u, InRequestedHeight))
	{
		Pool->AddReferencer(Asset.AssetPath);
	}

	FAssetThumbnail::~FAssetThumbnail() { Release(); }

	FAssetThumbnail::FAssetThumbnail(FAssetThumbnail&& Other) noexcept
		: Asset(std::move(Other.Asset)), Pool(std::exchange(Other.Pool, nullptr)),
		  RequestedWidth(Other.RequestedWidth), RequestedHeight(Other.RequestedHeight)
	{
	}

	auto FAssetThumbnail::operator=(FAssetThumbnail&& Other) noexcept -> FAssetThumbnail&
	{
		if (this == &Other) return *this;
		Release();
		Asset = std::move(Other.Asset);
		Pool = std::exchange(Other.Pool, nullptr);
		RequestedWidth = Other.RequestedWidth;
		RequestedHeight = Other.RequestedHeight;
		return *this;
	}

	auto FAssetThumbnail::Release() -> void
	{
		if (Pool) Pool->RemoveReferencer(Asset.AssetPath);
		Pool = nullptr;
	}

	auto FAssetThumbnail::Reassign(FAssetThumbnailPackageFingerprint InAsset) -> void
	{
		if (Asset == InAsset) return;
		if (Pool) Pool->RemoveReferencer(Asset.AssetPath);
		Asset = std::move(InAsset);
		if (Pool) Pool->AddReferencer(Asset.AssetPath);
	}

	auto FAssetThumbnail::Request(EAssetThumbnailPriority Priority) -> void
	{
		if (Pool) Pool->Request(Asset, Priority);
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
