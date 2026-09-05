#pragma once

#include "EngineAPI.h"
#include "Serialization/SerializationDefinitions.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	struct FTextureCubePlatformData;
	struct FVolumeTexturePlatformData;

	inline constexpr uint32 TexturePayloadSchemaVersion = 2;
	inline constexpr uint32 Texture2DBuilderVersion = 3;
	inline constexpr uint32 Texture2DPayloadProducerVersion = 4;
	inline constexpr uint32 TextureCubeBuilderVersion = 3;
	inline constexpr uint32 TextureCubeProjectionVersion = 2;
	inline constexpr uint32 VolumeTextureBuilderVersion = 3;
	inline constexpr uint32 TextureDerivedDataKeySchemaVersion = 3;
	inline constexpr uint32 TexturePayloadHeaderSize = 80;
	inline constexpr uint32 TexturePayloadRecordSize = 40;
	inline constexpr uint32 TexturePayloadAlignment = 16;
	inline constexpr uint32 MaximumTexture2DDimension = 16'384;
	inline constexpr uint32 MaximumTextureCubeDimension = 4'096;
	inline constexpr uint32 MaximumVolumeTextureDimension = 2'048;
	inline constexpr uint32 MaximumTextureMipCount = 32;
	inline constexpr uint64 MaximumTexturePayloadBytes = 2ull * 1024ull * 1024ull * 1024ull;
	inline const FGuid Texture2DPrimaryCookedPayloadId{
		0x53aa6a89, 0xdc49401a, 0xb409adc4, 0x98ac4f8b};
	inline const FGuid TextureCubePrimaryCookedPayloadId{
		0xd52878ce, 0x8f5048c7, 0xa3c7ff84, 0x6e2c4c5a};
	inline const FGuid VolumeTexturePrimaryCookedPayloadId{
		0x672b164e, 0x4e194871, 0xa7b841df, 0xe3208b15};

	enum class ETexturePayloadDimension : uint32
	{
		Texture2D = 1,
		TextureCube = 2,
		Texture3D = 3
	};

	enum class ETextureStablePixelFormat : uint32
	{
		BC1_UNORM = 1,
		BC1_UNORM_SRGB = 2,
		BC3_UNORM = 3,
		BC3_UNORM_SRGB = 4,
		BC5_UNORM = 5,
		BC7_UNORM = 6,
		BC7_UNORM_SRGB = 7,
		R8_UNORM = 8,
		RG8_UNORM = 9,
		RGBA8_UNORM = 10,
		R16_FLOAT = 11,
		RGBA16_FLOAT = 12
	};

	ENGINE_API auto BuildVolumeTextureSerializedValue(
		const FVolumeTexturePlatformData& PlatformData,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteArray& OutBytes,
		std::string& OutError) -> bool;
	// Decodes into detached storage and preserves the prior output on every failure.
	ENGINE_API auto ParseVolumeTextureSerializedValue(
		std::span<const std::byte> Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FVolumeTexturePlatformData& OutPlatformData) -> FDecodeResult;

}
