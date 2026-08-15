#pragma once

#include "AssetBuild/BuildFunction.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"

namespace Durin::Asset::Build::Private
{
	extern const FBuildFunctionIdentity Texture2DFunctionIdentity;
	extern const FBuildFunctionIdentity TextureCubeFunctionIdentity;
	inline constexpr std::string_view Texture2DInputName = "Texture2DInput";
	inline constexpr std::string_view Texture2DValueName = "Texture2DPayload";
	inline constexpr std::string_view TextureCubeInputName = "TextureCubeBuildInput";
	inline constexpr std::string_view TextureCubeValueName = "TextureCubePayload";

	auto EncodeTexture2DLocalInput(const FTexture2DBuildRequest& Request, bool bSRGB)
		-> std::vector<uint8>;
	auto DecodeTexture2DPlatformValue(const FBuildValue& Value,
		FTexturePlatformData& OutData, std::string& OutError) -> bool;
	auto EncodeTextureCubeLocalInput(const FTextureCubeSourceData& SourceData)
		-> std::vector<uint8>;
	auto DecodeTextureCubePlatformValue(const FBuildValue& Value,
		FTextureCubePlatformData& OutData, std::string& OutError) -> bool;

	auto CreateTexture2DBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateTextureCubeBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
