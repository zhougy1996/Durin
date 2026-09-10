#pragma once

#include "DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "Texture/TextureBuildOutcome.h"
#include "Modules/ModularFeature.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	inline constexpr uint64 MaximumTextureCubePanoramaPixels = 32ull * 1024ull * 1024ull;
	inline constexpr uint32 MaximumTextureCubePanoramaDimension = 16384;
	inline constexpr uint32 MaximumProjectedTextureCubeFaceDimension = 4096;
	inline constexpr float MinimumTextureCubePanoramaExposureEV = -16.0f;
	inline constexpr float MaximumTextureCubePanoramaExposureEV = 16.0f;

	struct FTextureCubeFacesBuildSettings
	{
		bool bSRGB = true;
	};

	struct FTextureCubePanoramaBuildSettings
	{
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
		ETextureCubeOutput Output = ETextureCubeOutput::LDR;
	};

	struct FTextureCubePanoramaImage
	{
		FByteBuffer Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		bool bHasTransparency = false;
	};

	struct FTextureCubePanoramaFloatImage
	{
		std::vector<float> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
	};

	// Canonical faces may retain panorama authoring metadata when used by PostLoad.
	struct FTextureCubeFacesBuildInput
	{
		FTextureCubeDecodedFaces DecodedFaces;
		// Installed source identity for rebuilds; imports let Engine prepare the canonical identity.
		FXxHash128 SourceIdentity;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		uint32 OriginalSourceWidth = 0;
		uint32 OriginalSourceHeight = 0;
		uint32 PanoramaFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		FTextureCubeFacesBuildSettings Settings;
	};

	struct FTextureCubePanoramaBuildInput
	{
		std::variant<FTextureCubePanoramaImage, FTextureCubePanoramaFloatImage> Image;
		FTextureCubePanoramaBuildSettings Settings;
	};

	// Immutable object-free input borrowed by a synchronous provider invocation.
	struct FTextureCubeBuildRequest
	{
		std::variant<FTextureCubeFacesBuildInput, FTextureCubePanoramaBuildInput> Input;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	struct FTextureCubeBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 BuilderVersion = 0;
		uint32 ProjectionVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && BuilderVersion != 0
				&& ProjectionVersion != 0;
		}
	};

	// Engine-owned canonical authoring state produced while normalizing panorama input.
	struct FTextureCubeCanonicalBuildInput
	{
		FTextureCubeDecodedFaces DecodedFaces;
		// Installed source identity for rebuilds; imports let Engine prepare the canonical identity.
		FXxHash128 SourceIdentity;
		Image::FImage AuthoredPanorama;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		uint32 OriginalSourceWidth = 0;
		uint32 OriginalSourceHeight = 0;
		uint32 PanoramaFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		bool bSRGB = true;
		ETextureCubeOutput Output = ETextureCubeOutput::LDR;
	};

	enum class ETextureCubeBuildProductOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// Detached derived-only product. Authored and normalized source stay separate.
	struct FTextureCubeBuildProduct
	{
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		FCacheKeyProxy DerivedDataKey;
		std::string PersistenceDiagnostic;
		FTextureCubeBuildProviderDescriptor Provider;
		ETextureCubeBuildProductOrigin Origin = ETextureCubeBuildProductOrigin::Rebuilt;
	};

	struct FTextureCubeRecipeBuildRequest
	{
		std::reference_wrapper<const FTextureCubeDecodedFaces> DecodedFaces;
		bool bSRGB = true;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		// Borrowed only for this synchronous invocation; HDR bypasses RGBA8 scratch faces.
		const Image::FImage* HDRPanorama = nullptr;
		FTextureCubePanoramaBuildSettings PanoramaSettings;
	};

	struct FTextureCubeRecipeBuildProduct
	{
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
	};

	struct FTextureCubeResultApplicationContext
	{
		bool bMarkPackageDirty = true;
		bool bSourceDecoderInvoked = true;
		// PostLoad/rebuild consumes the installed source without replacing its storage.
		bool bPreserveSource = false;
	};

	class ITextureCubeBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.TextureCubeBuildProvider";
		static constexpr uint32 FeatureVersion = 3;

		virtual auto GetDescriptor() const -> FTextureCubeBuildProviderDescriptor = 0;
		virtual auto Normalize(
			const FTextureCubeBuildRequest& Request
		)
			-> TTextureBuildResult<FTextureCubeCanonicalBuildInput> = 0;
		virtual auto Build(
			const FTextureCubeRecipeBuildRequest& Request
		)
			-> TTextureBuildResult<FTextureCubeRecipeBuildProduct> = 0;
	};

	struct FTextureCubeBuildValue
	{
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
	};

	ENGINE_API auto InvokeTextureCubeBuildProvider(const FTextureCubeBuildRequest& Request)
		-> TTextureBuildResult<FTextureCubeBuildValue>;
	ENGINE_API auto BuildTextureCubeSynchronously(DTextureCube& Texture, const FTextureCubeBuildRequest& Request, const FTextureCubeResultApplicationContext& Context)
		-> FTextureBuildOutcome;
}
