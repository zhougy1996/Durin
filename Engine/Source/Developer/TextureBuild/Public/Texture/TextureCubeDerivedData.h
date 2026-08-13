#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::Asset::Build
{
	enum class ETextureCubeBuildSourceLayout : uint32
	{
		SixFaces = 0,
		EquirectangularPanorama = 1
	};

	struct FTextureCubeBuildKeyInput
	{
		ETextureCubeBuildSourceLayout SourceLayout = ETextureCubeBuildSourceLayout::SixFaces;
		std::array<FXxHash128, TextureCubeFaceCount> FaceContentHashes{};
		FXxHash128 PanoramaContentHash;
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
		bool bSRGB = true;
		uint32 BuilderVersion = TextureCubeBuilderVersion;
		uint32 PayloadSchemaVersion = TexturePayloadSchemaVersion;
		uint32 ProjectionVersion = TextureCubeProjectionVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;

		TEXTUREBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	TEXTUREBUILD_API auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>;
	TEXTUREBUILD_API auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}
