#pragma once

#include "Asset/EditorBulkData.h"
#include "Asset/CookedAsset.h"
#include "EngineAPI.h"
#include "DObject/StructOps.h"
#include "PixelFormat.h"

#include "VolumeTextureData.gen.h"

namespace Durin
{
	class FArchive;

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
		FEditorBulkData Voxels;

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

		FXxHash128 CanonicalSourceIdentity;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
		auto GetVoxelBytes() const -> FSharedByteBuffer
		{
			return Voxels.GetPayload().Wait().value_or(FSharedByteBuffer{});
		}
		ENGINE_API auto SetVoxelBytes(FByteView Bytes) -> bool;
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
		FByteBuffer Voxels;
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
		// Loads one declared payload in place; discard failed destinations. The
		// owning byte boundary checks completion before publishing a replacement.
		ENGINE_API auto Serialize(
			FArchive& Ar) -> void;
	};

}
