#pragma once

#include "AssetBuild/BuildFunction.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTexture.h"

namespace Durin::Asset::Build::Private
{
	extern const FBuildFunctionIdentity Texture2DFunctionIdentity;
	extern const FBuildFunctionIdentity TextureCubeFunctionIdentity;
	extern const FBuildFunctionIdentity VolumeTextureFunctionIdentity;
	inline constexpr std::string_view Texture2DInputName = "Texture2DInput";
	inline constexpr std::string_view Texture2DValueName = "Texture2DPayload";
	inline constexpr std::string_view TextureCubeInputName = "TextureCubeBuildInput";
	inline constexpr std::string_view TextureCubeValueName = "TextureCubePayload";
	inline constexpr std::string_view VolumeTextureInputName = "VolumeTextureInput";
	inline constexpr std::string_view VolumeTextureValueName = "VolumeTexturePayload";

	auto EncodeTexture2DLocalInput(const FTexture2DBuildRequest& Request, bool bSRGB)
		-> std::vector<uint8>;
	auto DecodeTexture2DPlatformValue(const FBuildValue& Value,
		FTexturePlatformData& OutData, std::string& OutError) -> bool;
	auto EncodeTextureCubeLocalInput(const FTextureCubeSourceData& SourceData)
		-> std::vector<uint8>;
	auto DecodeTextureCubePlatformValue(const FBuildValue& Value,
		FTextureCubePlatformData& OutData, std::string& OutError) -> bool;
	auto EncodeVolumeTextureLocalInput(const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings) -> std::vector<uint8>;
	auto DecodeVolumeTexturePlatformValue(const FBuildValue& Value,
		FVolumeTexturePlatformData& OutData, std::string& OutError) -> bool;

	auto CreateTexture2DBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateTextureCubeBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateVolumeTextureBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
