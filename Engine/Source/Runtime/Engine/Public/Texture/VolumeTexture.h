#pragma once

#include "Asset/Cook.h"
#include "EngineAPI.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

#include "VolumeTexture.gen.h"

namespace Durin
{
	// Selects one portable uncompressed voxel format admitted by volume assets.
	DENUM()
	enum class EVolumeTextureFormat : uint8
	{
		R8_UNORM,
		RG8_UNORM,
		RGBA8_UNORM,
		R16_FLOAT,
		RGBA16_FLOAT
	};

	DENUM()
	enum class EVolumeTextureMipFilter : uint8
	{
		Box
	};

	// Owns a tightly packed normalized voxel source retained by an authored asset.
	DSTRUCT()
	struct FVolumeTextureSourceData
	{
		GENERATED_BODY()

		DPROPERTY()
		std::vector<uint8> Voxels;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint32 Depth = 0;

		DPROPERTY()
		EVolumeTextureFormat Format = EVolumeTextureFormat::R8_UNORM;

		ENGINE_API auto IsValid() const -> bool;
	};

	// Freezes deterministic mip filtering and output format policy.
	DSTRUCT()
	struct FVolumeTextureBuildSettings
	{
		GENERATED_BODY()

		DPROPERTY()
		EVolumeTextureFormat OutputFormat = EVolumeTextureFormat::R8_UNORM;

		DPROPERTY()
		EVolumeTextureMipFilter MipFilter = EVolumeTextureMipFilter::Box;

		auto operator==(const FVolumeTextureBuildSettings&) const -> bool = default;
	};

	// Owns one exact volume mip with explicit row and depth pitches.
	struct FVolumeTextureMipData
	{
		std::vector<uint8> Voxels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Depth = 0;
		uint32 RowPitch = 0;
		uint32 DepthPitch = 0;

		ENGINE_API auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	// Owns the portable format and complete three-axis mip chain used at runtime.
	struct FVolumeTexturePlatformData
	{
		std::vector<FVolumeTextureMipData> Mips;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const FTexturePlatformSerializationContext& Context) -> void;
	};

	// Package-backed volume asset with revisioned last-known-good GPU publication.
	DCLASS()
	class DVolumeTexture : public DTexture
	{
		GENERATED_BODY()

	public:
		ENGINE_API explicit DVolumeTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DVolumeTexture() override;

		auto GetSourceData() const -> const FVolumeTextureSourceData& { return SourceData; }
		auto GetBuildSettings() const -> const FVolumeTextureBuildSettings& { return BuildSettings; }
		auto GetPlatformData() const -> const FVolumeTexturePlatformData* { return PlatformData.get(); }
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto AddToCook(Asset::FCookContext& Context,
			std::string_view VirtualPackagePath, std::string& OutError,
			bool bRetainDiagnosticSourceData = false) -> bool;
		ENGINE_API auto PublishBuiltData(FVolumeTextureSourceData InSourceData,
			FVolumeTextureBuildSettings InBuildSettings,
			std::unique_ptr<FVolumeTexturePlatformData> InPlatformData,
			std::string InDerivedDataKey, std::string& OutError) -> bool;
		ENGINE_API auto PublishDerivedDataLoad(
			std::unique_ptr<FVolumeTexturePlatformData> InPlatformData,
			std::string InDerivedDataKey, std::string& OutError) -> bool;
		ENGINE_API auto ExchangeBuiltState(DVolumeTexture& Other) noexcept -> void;
		ENGINE_API auto RefreshBuildStatus() -> void;

	protected:
		auto CreateRenderResourceCandidate(FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;

	private:
		DPROPERTY()
		FVolumeTextureSourceData SourceData;

		DPROPERTY()
		FVolumeTextureBuildSettings BuildSettings;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;

		auto LoadCookedPlatformData(std::string& OutError) -> bool;
	};
}
