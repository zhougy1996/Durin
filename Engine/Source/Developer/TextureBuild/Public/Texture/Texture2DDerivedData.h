#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::Asset
{
	inline constexpr uint32 Texture2DBuilderVersion = 2;

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

		TEXTUREBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	TEXTUREBUILD_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> FByteArray;
	TEXTUREBUILD_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> std::string;
}
