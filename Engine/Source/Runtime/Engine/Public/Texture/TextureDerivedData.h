#pragma once

#include "CookedAsset.h"
#include "EngineAPI.h"
#include "PayloadDecodeResult.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	struct FTextureCubePlatformData;

	inline constexpr uint32 TexturePayloadMagic = 0x4C505854; // TXPL
	inline constexpr uint32 TexturePayloadSchemaVersion = 1;
	inline constexpr uint32 Texture2DPayloadProducerVersion = 2;
	inline constexpr uint32 TextureCubeBuilderVersion = 1;
	inline constexpr uint32 TextureCubeProjectionVersion = 1;
	inline constexpr uint32 TextureDerivedDataKeySchemaVersion = 1;
	inline constexpr uint32 TexturePayloadHeaderSize = 80;
	inline constexpr uint32 TexturePayloadRecordSize = 40;
	inline constexpr uint32 TexturePayloadAlignment = 16;
	inline constexpr uint32 MaximumTexture2DDimension = 16'384;
	inline constexpr uint32 MaximumTextureCubeDimension = 4'096;
	inline constexpr uint32 MaximumTextureMipCount = 32;
	inline constexpr uint64 MaximumTexturePayloadBytes = 2ull * 1024ull * 1024ull * 1024ull;
	inline const FGuid Texture2DPrimaryCookedPayloadId{
		0x53aa6a89, 0xdc49401a, 0xb409adc4, 0x98ac4f8b};
	inline const FGuid TextureCubePrimaryCookedPayloadId{
		0xd52878ce, 0x8f5048c7, 0xa3c7ff84, 0x6e2c4c5a};

	enum class ETexturePayloadDimension : uint32
	{
		Texture2D = 1,
		TextureCube = 2
	};

	enum class ETextureStablePixelFormat : uint32
	{
		BC1_UNORM = 1,
		BC1_UNORM_SRGB = 2,
		BC3_UNORM = 3,
		BC3_UNORM_SRGB = 4,
		BC5_UNORM = 5,
		BC7_UNORM = 6,
		BC7_UNORM_SRGB = 7
	};

	ENGINE_API auto DecodeTextureCubePayload(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::unique_ptr<FTextureCubePlatformData>& OutPlatformData) -> FPayloadDecodeResult;
}
