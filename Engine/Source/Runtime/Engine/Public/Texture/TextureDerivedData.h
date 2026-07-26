#pragma once

#include "CookedAsset.h"
#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	inline constexpr uint32 TexturePayloadMagic = 0x4C505854; // TXPL
	inline constexpr uint32 TexturePayloadSchemaVersion = 1;
	inline constexpr uint32 Texture2DBuilderVersion = 2;
	inline constexpr uint32 TextureDerivedDataKeySchemaVersion = 1;
	inline constexpr uint32 TexturePayloadHeaderSize = 80;
	inline constexpr uint32 TexturePayloadRecordSize = 40;
	inline constexpr uint32 TexturePayloadAlignment = 16;
	inline constexpr uint32 MaximumTexture2DDimension = 16'384;
	inline constexpr uint32 MaximumTextureMipCount = 32;
	inline constexpr uint64 MaximumTexturePayloadBytes = 2ull * 1024ull * 1024ull * 1024ull;

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

	struct FTexture2DDerivedDataKeyInput
	{
		FXxHash128 SourceContentHash;
		ETextureUsage Usage = ETextureUsage::Color;
		bool bSRGB = true;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		uint32 MaximumResolution = 0;
		float AlphaCoverageThreshold = 0.5f;
		uint32 BuilderVersion = Texture2DBuilderVersion;
		uint32 PayloadSchemaVersion = TexturePayloadSchemaVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;
	};

	ENGINE_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DDerivedDataKeyInput& Input) -> std::vector<uint8>;

	ENGINE_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DDerivedDataKeyInput& Input) -> std::string;

	ENGINE_API auto EncodeTexture2DPayload(
		const FTexturePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;

	ENGINE_API auto DecodeTexture2DPayload(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData,
		std::string& OutError) -> bool;
}
