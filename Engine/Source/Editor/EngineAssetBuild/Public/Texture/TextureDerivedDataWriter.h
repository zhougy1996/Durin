#pragma once

#include "EngineAssetBuildAPI.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/TextureCube.h"

namespace Durin::AssetBuild::TextureDerivedDataWriter
{
	ENGINEASSETBUILD_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DDerivedDataKeyInput& Input) -> std::vector<uint8>;
	ENGINEASSETBUILD_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DDerivedDataKeyInput& Input) -> std::string;
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
