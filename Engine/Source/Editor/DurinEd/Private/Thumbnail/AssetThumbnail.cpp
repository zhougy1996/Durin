#include "AssetThumbnail.h"

#include "Image/ImageDecoder.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor
{
	auto FObjectThumbnail::Validate(uint64 MaximumBytes, std::string& OutError) const -> bool
	{
		const uint64 ExpectedBytes = static_cast<uint64>(Width) * Height * 4;
		if (Width == 0 || Height == 0 || ExpectedBytes > MaximumBytes
			|| (!Pixels.empty() && Pixels.size() != ExpectedBytes)
			|| EncodedBytes.size() > MaximumBytes || EncodingVersion == 0)
		{
			OutError = "Object thumbnail dimensions or bytes violate the configured bounds.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FObjectThumbnail::Decode(std::span<const std::byte> Bytes,
		uint64 MaximumBytes, std::string& OutError) -> bool
	{
		*this = {};
		if (Bytes.empty() || Bytes.size() > MaximumBytes)
		{
			OutError = "Encoded object thumbnail is empty or oversized.";
			return false;
		}
		Image::FDecodedImage Image;
		if (!Image::DecodeImageFromMemory(Bytes, Image, OutError,
			{.MaximumEncodedBytes = MaximumBytes,
			 .MaximumDecodedPixels = MaximumBytes / 4})) return false;
		Pixels = std::move(Image.Pixels);
		EncodedBytes.assign(Bytes.begin(), Bytes.end());
		Width = Image.Width;
		Height = Image.Height;
		bHasTransparency = Image.bHasTransparency;
		EncodingVersion = 1;
		return Validate(MaximumBytes, OutError);
	}

	FAssetThumbnail::FAssetThumbnail(FAssetThumbnailPackageFingerprint InAsset,
		uint32 InRequestedWidth, uint32 InRequestedHeight, FAssetThumbnailPool* InPool)
		: Asset(std::move(InAsset))
		, Pool(InPool ? InPool : &GetDefaultThumbnailManager().GetSharedPool())
		, RequestedWidth(std::max(1u, InRequestedWidth))
		, RequestedHeight(std::max(1u, InRequestedHeight))
	{
		Pool->AddReferencer(Asset.VirtualPath);
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
		if (Pool) Pool->RemoveReferencer(Asset.VirtualPath);
		Pool = nullptr;
	}

	auto FAssetThumbnail::Reassign(FAssetThumbnailPackageFingerprint InAsset) -> void
	{
		if (Asset == InAsset) return;
		if (Pool) Pool->RemoveReferencer(Asset.VirtualPath);
		Asset = std::move(InAsset);
		if (Pool) Pool->AddReferencer(Asset.VirtualPath);
	}

	auto FAssetThumbnail::Request(EAssetThumbnailPriority Priority) -> void
	{
		if (Pool) Pool->Request(Asset, Priority);
	}

	auto FAssetThumbnail::Refresh() -> void
	{
		if (Pool) Pool->Refresh(Asset.VirtualPath);
	}

	auto FAssetThumbnail::GetView() const -> FAssetThumbnailView
	{
		return Pool ? Pool->Find(Asset.VirtualPath) : FAssetThumbnailView{};
	}
}
