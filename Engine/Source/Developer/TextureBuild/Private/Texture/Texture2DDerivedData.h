#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	inline constexpr uint32 Texture2DBuilderVersion = 2;

	struct FTexture2DBuildKeyInput
	{
		FXxHash128 ImportedDataIdentity;
		ETextureUsage Usage = ETextureUsage::Color;
		bool bSRGB = true;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		uint32 MaximumResolution = 0;
		float AlphaCoverageThreshold = 0.5f;
		uint32 BuilderVersion = Texture2DBuilderVersion;
		uint32 PayloadSchemaVersion = TexturePayloadSchemaVersion;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;

		TEXTUREBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	TEXTUREBUILD_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> FByteArray;
	TEXTUREBUILD_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> std::string;
}
