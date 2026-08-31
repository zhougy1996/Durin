#pragma once

#include "Texture/TextureDerivedData.h"

namespace Durin::TexturePayloadContainer
{
	struct FDescriptor
	{
		uint32 ProducerVersion = 0;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;
		ETexturePayloadDimension Dimension = static_cast<ETexturePayloadDimension>(0);
		ETextureStablePixelFormat StableFormat = static_cast<ETextureStablePixelFormat>(0);
		uint32 SliceCount = 0;
		uint32 MipCount = 0;
	};

	struct FRecord
	{
		uint32 Coordinate = 0;
		uint32 MipIndex = 0;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;
		uint32 LayerPitch = 0;
		uint64 DataOffset = 0;
		uint64 ByteCount = 0;
	};

	struct FBuildRecord
	{
		FRecord Record;
		std::span<const std::byte> Data;
	};

	struct FDecodedContainer
	{
		FDescriptor Descriptor;
		std::vector<FRecord> Records;
	};

	auto Build(
		const FDescriptor& Descriptor,
		std::span<const FBuildRecord> Records,
		FByteArray& OutBytes,
		std::string& OutError) -> bool;

	auto Parse(
		std::span<const std::byte> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FDecodedContainer& OutContainer) -> FDecodeResult;

	inline auto GetData(std::span<const std::byte> Bytes, const FRecord& Record)
		-> std::span<const std::byte>
	{
		return Bytes.subspan(static_cast<size_t>(Record.DataOffset),
			static_cast<size_t>(Record.ByteCount));
	}
}
