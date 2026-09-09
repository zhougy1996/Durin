#pragma once

#include "Texture/TextureDerivedData.h"

namespace Durin::TexturePayloadContainer
{
	struct FTargetContext
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
	};

	// Resolves the stable wire identity from Archive target facts.
	auto ResolveContext(FArchive& Ar, FTargetContext& Context) -> bool;

	struct FDescriptor
	{
		uint32 ProducerVersion = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
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

	struct FPayloadRecord
	{
		FRecord Record;
		FByteView Data;
	};

	// Transfers one declared container extent. Loaded records borrow the input
	// archive's storage; the caller retains its owner until family decoding ends.
	auto Serialize(FArchive& Ar, FDescriptor& Descriptor,
		std::vector<FPayloadRecord>& Records) -> void;

}
