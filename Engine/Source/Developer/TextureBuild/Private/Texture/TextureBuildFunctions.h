#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTexture.h"

namespace Durin::AssetPrivate
{
	using namespace ::Durin::DerivedData;

	extern const FBuildFunctionName Texture2DFunctionName;
	extern const FBuildFunctionName TextureCubeFunctionName;
	extern const FBuildFunctionName VolumeTextureFunctionName;
	inline constexpr std::string_view Texture2DInputName = "Texture2DInput";
	inline constexpr std::string_view Texture2DValueName = "Texture2DPayload";
	inline constexpr std::string_view TextureCubeInputName = "TextureCubeBuildInput";
	inline constexpr std::string_view TextureCubeValueName = "TextureCubePayload";
	inline constexpr std::string_view VolumeTextureInputName = "VolumeTextureInput";
	inline constexpr std::string_view VolumeTextureValueName = "VolumeTexturePayload";

	auto EncodeTexture2DLocalInput(const FTexture2DBuildRequest& Request, bool bSRGB)
		-> FByteArray;
	auto DecodeTexture2DPlatformValue(const FBuildValue& Value,
		FTexturePlatformData& OutData, std::string& OutError) -> bool;
	auto EncodeTextureCubeLocalInput(const FTextureCubeSourceData& SourceData)
		-> FByteArray;
	auto DecodeTextureCubePlatformValue(const FBuildValue& Value,
		FTextureCubePlatformData& OutData, std::string& OutError) -> bool;
	auto EncodeVolumeTextureLocalInput(const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings) -> FByteArray;
	auto DecodeVolumeTexturePlatformValue(const FBuildValue& Value,
		FVolumeTexturePlatformData& OutData, std::string& OutError) -> bool;

	auto CreateTexture2DBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateTextureCubeBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateVolumeTextureBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
