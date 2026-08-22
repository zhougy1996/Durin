#pragma once

#include "Asset/Cook.h"
#include "Asset/AuthoredBulkData.h"
#include "EngineAPI.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

#include "VolumeTexture.gen.h"

namespace Durin
{
	struct FVolumeTextureSourceVersion
	{
		inline static constexpr FGuid Guid{
			0x769fba31, 0x9d4f42d7, 0xb311e1a4, 0x5a0d82c6};
		enum Type : int32
		{
			BeforeCustomVersionWasAdded = -1,
			ByteBlob = 1,
			AuthoredBulkData = 2,
			LatestVersion = AuthoredBulkData,
		};
	};

	inline constexpr FGuid VolumeTextureSourcePayloadId{
		0x6fe21a38, 0x494340a7, 0xa304c2d5, 0x26f22931};
	inline constexpr FGuid VolumeTextureSourceFormatId{
		0x2854a7c1, 0x94cb4ab8, 0x8cd8be32, 0xc2f680b7};
	inline constexpr uint32 VolumeTextureSourceFormatVersion = 1;

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
		Asset::FAuthoredBulkData Voxels;

		DPROPERTY(Deprecated, CustomVersion = Durin::FVolumeTextureSourceVersion,
			DeprecatedBefore = Durin::FVolumeTextureSourceVersion::AuthoredBulkData,
			HistoricalName = "Voxels", MigratesTo = "Voxels")
		std::vector<std::byte> VoxelsBlob_DEPRECATED;

		DPROPERTY(Deprecated, CustomVersion = Durin::FVolumeTextureSourceVersion,
			DeprecatedBefore = Durin::FVolumeTextureSourceVersion::ByteBlob,
			MigratesTo = "Voxels")
		std::vector<uint8> Voxels_DEPRECATED;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint32 Depth = 0;

		DPROPERTY()
		EVolumeTextureFormat Format = EVolumeTextureFormat::R8_UNORM;

		ENGINE_API auto IsValid() const -> bool;
		auto GetVoxelBytes() const -> std::span<const std::byte>
		{
			return Voxels.GetResidentBytes();
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

	// Stores the mounted source image and its atlas interpretation for editor
	// rebuild and reimport. Runtime and cooked sampling remain source-format agnostic.
	DSTRUCT()
	struct FVolumeTextureSourceImportData
	{
		GENERATED_BODY()

		DPROPERTY()
		FTextureSourceFile Source;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Source File")
		std::string SourceFile;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Import Format")
		EVolumeTextureImportFormat ImportFormat = EVolumeTextureImportFormat::PngRowMajorAtlas;

		DPROPERTY(Edit, ReadOnly)
		EVolumeTextureSourceChannels Channels = EVolumeTextureSourceChannels::Red;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Slice Width")
		uint32 SliceWidth = 0;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Slice Height")
		uint32 SliceHeight = 0;

		DPROPERTY(Edit, ReadOnly)
		uint32 Depth = 0;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Tile Columns")
		uint32 TilesX = 0;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Tile Rows")
		uint32 TilesY = 0;

		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;

		auto HasSource() const -> bool { return Source.HasSource(); }
		auto operator==(const FVolumeTextureSourceImportData&) const -> bool = default;
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
		auto GetSourceImportData() const -> const FVolumeTextureSourceImportData& { return SourceImportData; }
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
		ENGINE_API auto PublishSourceImportData(
			FVolumeTextureSourceImportData InSourceImportData,
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
		DPROPERTY()
		FVolumeTextureSourceData SourceData;

		DPROPERTY(Edit, ReadOnly, DisplayName = "Import")
		FVolumeTextureSourceImportData SourceImportData;

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

namespace Durin
{
	template<>
	struct TDStructOpsTraits<FVolumeTextureSourceData>
		: TDStructOpsTraitsBase<FVolumeTextureSourceData>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(
			FVolumeTextureSourceData& Value,
			FDStructPostDeserializeContext& Context) -> bool
		{
			if (!Value.Voxels_DEPRECATED.empty())
			{
				if (!Value.VoxelsBlob_DEPRECATED.empty())
					return Context.Fail(
						"Volume source contains both deprecated Blob and byte Array data.");
				Value.VoxelsBlob_DEPRECATED.resize(Value.Voxels_DEPRECATED.size());
				std::ranges::transform(Value.Voxels_DEPRECATED, Value.VoxelsBlob_DEPRECATED.begin(),
					[](uint8 Byte) { return static_cast<std::byte>(Byte); });
				Value.Voxels_DEPRECATED.clear();
			}
			if (!Value.VoxelsBlob_DEPRECATED.empty())
			{
				if (Value.Voxels.GetDescriptor().LogicalByteCount != 0)
					return Context.Fail(
						"Volume source contains both current bulk and deprecated Blob data.");
				if (!Value.SetVoxelBytes(Value.VoxelsBlob_DEPRECATED))
					return Context.Fail("Volume source Blob-to-bulk migration failed.");
				Value.VoxelsBlob_DEPRECATED.clear();
			}
			return Value.IsValid()
				|| Context.Fail("Volume source byte migration produced invalid dimensions or payload size.");
		}
	};
}
