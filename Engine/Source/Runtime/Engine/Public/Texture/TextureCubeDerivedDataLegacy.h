#pragma once

#include "CookedAsset.h"
#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "PayloadDecodeResult.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	struct FTextureCubePlatformData;

	// Transitional Runtime-owned TextureCube recipe API. Stage 3 moves this
	// authoring contract into EngineAssetBuild and deletes this header.
	enum class ETextureCubeDerivedDataSourceLayout : uint32
	{
		SixFaces = 0,
		EquirectangularPanorama = 1
	};

	struct FTextureCubeDerivedDataKeyInput
	{
		ETextureCubeDerivedDataSourceLayout SourceLayout =
			ETextureCubeDerivedDataSourceLayout::SixFaces;
		std::array<FXxHash128, 6> FaceContentHashes{};
		FXxHash128 PanoramaContentHash;
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
		bool bSRGB = true;
		uint32 BuilderVersion = TextureCubeBuilderVersion;
		uint32 PayloadSchemaVersion = TexturePayloadSchemaVersion;
		uint32 ProjectionVersion = TextureCubeProjectionVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;
	};

	ENGINE_API auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeDerivedDataKeyInput& Input,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;

	ENGINE_API auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeDerivedDataKeyInput& Input,
		std::string& OutKey,
		std::string& OutError) -> bool;

	ENGINE_API auto EncodeTextureCubePayload(
		const FTextureCubePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
}
