#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTexture.h"

namespace Durin::Asset
{
	// Canonical inputs that determine one volume texture derived-data identity.
	struct FVolumeTextureBuildKeyInput
	{
		FXxHash128 SourceContentHash;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Depth = 0;
		FVolumeTextureBuildSettings Settings;
		uint32 BuilderVersion = VolumeTextureBuilderVersion;
		uint32 SourcePayloadSchemaVersion = VolumeTextureSourcePayloadSchemaVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;

		TEXTUREBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	TEXTUREBUILD_API auto BuildVolumeTextureDerivedDataKeyBytes(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError)
		-> FByteArray;
	TEXTUREBUILD_API auto BuildVolumeTextureDerivedDataKey(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError)
		-> std::string;
}
