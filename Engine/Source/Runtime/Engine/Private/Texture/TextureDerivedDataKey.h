#pragma once

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Hash/XxHash.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace DerivedData { class FBuildDefinition; struct FBuildDefinitionError; }
	struct FArchiveFailure;

	inline constexpr std::string_view Texture2DCacheBucket = "Textures/Objects";
	inline constexpr std::string_view TextureCubeCacheBucket = "TextureCube/Objects";
	inline constexpr std::string_view VolumeTextureCacheBucket = "VolumeTexture/Objects";

	struct FTexture2DBuildKeyInput
	{
		FXxHash128 SourceIdentity;
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
		// Optionally reports the first invalid field; success clears the supplied failure.
		ENGINE_API auto IsValid(FArchiveFailure* OutFailure = nullptr) const -> bool;
	};

	enum class ETextureCubeBuildSourceLayout : uint32
	{
		SixFaces = 0,
		EquirectangularPanorama = 1
	};

	struct FTextureCubeBuildKeyInput
	{
		ETextureCubeBuildSourceLayout SourceLayout = ETextureCubeBuildSourceLayout::SixFaces;
		// Identity of the complete ordered canonical source, including every face.
		FXxHash128 CanonicalSourceIdentity;
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
		bool bSRGB = true;
		uint32 BuilderVersion = TextureCubeBuilderVersion;
		uint32 PayloadSchemaVersion = TexturePayloadSchemaVersion;
		uint32 ProjectionVersion = TextureCubeProjectionVersion;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		// Optionally reports the first invalid field; success clears the supplied failure.
		ENGINE_API auto IsValid(FArchiveFailure* OutFailure = nullptr) const -> bool;
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
		// Optionally reports the first invalid field; success clears the supplied failure.
		ENGINE_API auto IsValid(FArchiveFailure* OutFailure = nullptr) const -> bool;
	};

#if DURIN_WITH_EDITOR
	ENGINE_API auto MakeTexture2DBuildDefinition(const FTexture2DBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto MakeTextureCubeBuildDefinition(const FTextureCubeBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto MakeVolumeTextureBuildDefinition(const FVolumeTextureBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;

#endif
	ENGINE_API auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> FByteBuffer;
	ENGINE_API auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> FCacheKeyProxy;
	ENGINE_API auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FByteBuffer;
	ENGINE_API auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FCacheKeyProxy;
	ENGINE_API auto BuildVolumeTextureDerivedDataKeyBytes(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FByteBuffer;
	ENGINE_API auto BuildVolumeTextureDerivedDataKey(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FCacheKeyProxy;
}
