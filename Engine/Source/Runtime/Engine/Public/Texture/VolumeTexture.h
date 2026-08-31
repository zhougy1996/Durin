#pragma once

#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/Cook.h"
#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

#include "VolumeTexture.gen.h"

namespace Durin
{
	inline constexpr FGuid VolumeTextureSourcePayloadId{
		0x6fe21a38, 0x494340a7, 0xa304c2d5, 0x26f22931};
	inline constexpr uint32 VolumeTextureSourcePayloadSchemaVersion = 1;

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

	DENUM(DisplayName = "Volume Texture Import Format")
	enum class EVolumeTextureImportFormat : uint8
	{
		PngRowMajorAtlas DMETA(DisplayName = "PNG Row-Major Atlas")
	};

	DENUM(DisplayName = "Volume Texture Source Channels")
	enum class EVolumeTextureSourceChannels : uint8
	{
		Red,
		Green,
		Blue,
		Alpha,
		Luminance,
		RGBA
	};

	// Owns a tightly packed normalized voxel source retained by an authored asset.
	DSTRUCT()
	struct FVolumeTextureSourceData
	{
		GENERATED_BODY()

		DPROPERTY()
		Asset::FEditorBulkData Voxels;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint32 Depth = 0;

		DPROPERTY()
		EVolumeTextureFormat Format = EVolumeTextureFormat::R8_UNORM;

		DPROPERTY()
		uint32 PayloadSchemaVersion = VolumeTextureSourcePayloadSchemaVersion;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
		auto GetVoxelBytes() const -> FSharedByteBuffer
		{
			return Voxels.GetPayload().Wait().Buffer;
		}
		ENGINE_API auto SetVoxelBytes(std::span<const std::byte> Bytes) -> bool;
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
		FByteArray Voxels;
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
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetSourceData() const -> const FVolumeTextureSourceData& { return SourceData; }
		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		ENGINE_API auto PublishAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
		auto GetBuildSettings() const -> const FVolumeTextureBuildSettings& { return BuildSettings; }
		ENGINE_API auto GetPlatformData() const -> const FVolumeTexturePlatformData*;
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetCookedPlatformData() const -> const Asset::FBulkData& { return CookedPlatformData; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }
		auto GetDerivedDataDiagnostic() const -> const FTextureDerivedDataDiagnostic&
		{
			return DerivedDataDiagnostic;
		}

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
	private:
		friend auto Asset::ContributeEngineCookAsset(
			DObject&, std::string_view, Asset::FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(Asset::FCookContext& Context,
			std::string_view VirtualPackagePath, std::string& OutError) -> bool;
	public:
		ENGINE_API auto PublishBuiltData(FVolumeTextureSourceData InSourceData,
			FVolumeTextureBuildSettings InBuildSettings,
			std::unique_ptr<FVolumeTexturePlatformData> InPlatformData,
			std::string InDerivedDataKey, std::string InPersistenceDiagnostic,
			std::string& OutError) -> bool;
		ENGINE_API auto PublishDerivedDataLoad(
			std::unique_ptr<FVolumeTexturePlatformData> InPlatformData,
			std::string InDerivedDataKey, std::string& OutError) -> bool;
		ENGINE_API auto ExchangeBuiltState(DVolumeTexture& Other) noexcept -> void;
		auto ExchangeImportedState(DVolumeTexture& Other) noexcept -> void
		{
			ExchangeBuiltState(Other);
		}
		ENGINE_API auto RefreshBuildStatus() -> void;

	protected:
		auto CreateRenderResourceCandidate(FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;

	private:
		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FVolumeTextureSourceData SourceData;

		DPROPERTY()
		FVolumeTextureBuildSettings BuildSettings;

		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
		Asset::FBulkData CookedPlatformData;
		std::string DerivedDataKey;
		FTextureDerivedDataDiagnostic DerivedDataDiagnostic;
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;

		auto LoadCookedPlatformData(std::string& OutError) -> bool;
	};
}
