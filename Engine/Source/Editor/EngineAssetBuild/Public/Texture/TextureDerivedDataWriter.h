#pragma once

#include "EngineAssetBuildAPI.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/TextureCube.h"

namespace Durin::AssetBuild
{
	inline constexpr uint32 Texture2DBuilderVersion = 2;

	// Owns the canonical Texture2D recipe fields hashed into one DDC key.
	struct FTexture2DBuildKeyInput
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

		ENGINEASSETBUILD_API auto Serialize(FArchive& Ar) -> void;
	};
}

namespace Durin::AssetBuild::TextureDerivedDataWriter
{
	ENGINEASSETBUILD_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> std::vector<uint8>;
	ENGINEASSETBUILD_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> std::string;
	ENGINEASSETBUILD_API auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeDerivedDataKeyInput& Input,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	ENGINEASSETBUILD_API auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeDerivedDataKeyInput& Input,
		std::string& OutKey,
		std::string& OutError) -> bool;
	ENGINEASSETBUILD_API auto EncodeTextureCubePayload(
		const FTextureCubePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
}
