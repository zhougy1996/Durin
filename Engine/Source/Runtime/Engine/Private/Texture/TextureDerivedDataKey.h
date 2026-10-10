#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/ITextureCompressorModule.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace DerivedData { class FBuildAction; struct FBuildDefinitionError; struct FBuildFunctionDescriptor; }
	struct FArchiveFailure;

	inline constexpr uint32 Texture2DBuildFunctionVersion = 1;
	inline constexpr uint32 TextureCubeBuildFunctionVersion = 1;
	inline constexpr uint32 VolumeTextureBuildFunctionVersion = 1;
	inline constexpr uint32 TextureBuildOutputSchemaVersion = 2;

	inline constexpr std::string_view Texture2DCacheBucket = "Texture2D";
	inline constexpr std::string_view TextureCubeCacheBucket = "TextureCube";
	inline constexpr std::string_view VolumeTextureCacheBucket = "VolumeTexture";

	struct FTexture2DBuildKeyInput
	{
		FXxHash128 SourceIdentity;
		ETextureUsage Usage = ETextureUsage::Color;
		bool bSRGB = true;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		uint32 MaximumResolution = 0;
		float AlphaCoverageThreshold = 0.5f;
		uint64 BuilderVersion = 0;
		uint32 FunctionVersion = Texture2DBuildFunctionVersion;
		uint32 OutputSchemaVersion = TextureBuildOutputSchemaVersion;
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
		uint64 BuilderVersion = 0;
		uint32 FunctionVersion = TextureCubeBuildFunctionVersion;
		uint32 OutputSchemaVersion = TextureBuildOutputSchemaVersion;
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
		uint64 BuilderVersion = 0;
		uint32 FunctionVersion = VolumeTextureBuildFunctionVersion;
		uint32 OutputSchemaVersion = TextureBuildOutputSchemaVersion;
		uint32 SourcePayloadSchemaVersion = VolumeTextureSourcePayloadSchemaVersion;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		// Optionally reports the first invalid field; success clears the supplied failure.
		ENGINE_API auto IsValid(FArchiveFailure* OutFailure = nullptr) const -> bool;
	};

#if DURIN_WITH_EDITOR
	ENGINE_API auto GetTextureBuildDescriptor(ETextureBuildKind TextureKind)
		-> DerivedData::FBuildFunctionDescriptor;
	ENGINE_API auto MakeTexture2DBuildAction(const FTexture2DBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto MakeTextureCubeBuildAction(const FTextureCubeBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto MakeVolumeTextureBuildAction(const FVolumeTextureBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, DerivedData::FBuildDefinitionError>;

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

#endif
