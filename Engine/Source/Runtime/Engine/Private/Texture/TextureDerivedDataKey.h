#pragma once

#include "Hash/XxHash.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
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
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

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
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	struct FVolumeTextureBuildKeyInput
	{
		FXxHash128 CanonicalSourceIdentity;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Depth = 0;
		FVolumeTextureBuildSettings Settings;
		uint32 BuilderVersion = VolumeTextureBuilderVersion;
		uint32 SourcePayloadSchemaVersion = VolumeTextureSourcePayloadSchemaVersion;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINE_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> FByteArray;
	ENGINE_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> std::string;
	ENGINE_API auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FByteArray;
	ENGINE_API auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> std::string;
	ENGINE_API auto BuildVolumeTextureDerivedDataKeyBytes(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FByteArray;
	ENGINE_API auto BuildVolumeTextureDerivedDataKey(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> std::string;
}
