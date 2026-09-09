#pragma once

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
			return Voxels.GetPayload().Wait().Buffer;
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
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const FTexturePlatformSerializationContext& Context) -> void;
	};

	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	// Payload-backed input may require a synchronous read.
	ENGINE_API auto PrepareVolumeTextureSource(
		const FVolumeTextureSourceData& Value) -> std::optional<FTextureSource>;

	// Package-backed volume asset with owned updates and last-successful GPU publication.
	DCLASS()
	class DVolumeTexture : public DTexture
	{
		GENERATED_BODY()

	public:
		ENGINE_API explicit DVolumeTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DVolumeTexture() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetBuildSettings() const -> const FVolumeTextureBuildSettings& { return BuildSettings; }
		// GameThread only. Assigns validated settings and cancels pending authored builds.
		ENGINE_API auto SetBuildSettings(
			FVolumeTextureBuildSettings Value) -> void;

		ENGINE_API auto CreateBuildInput() const -> FVolumeTextureSourceData;

		// Returns installed CPU data only; never loads bulk data or updates resources.
		auto GetPlatformData() const -> const FVolumeTexturePlatformData*
		{
			return PlatformData.get();
		}
		// Immutable input identity for uploads and CPU previews; replacement leaves existing readers valid.
		auto GetPlatformDataShared() const -> std::shared_ptr<const FVolumeTexturePlatformData> { return PlatformData; }
		auto HasPlatformData() const -> bool override
		{
			return PlatformData && PlatformData->IsValid();
		}
		// Adopts data already validated by the producer on GameThread; does not update resources.
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FVolumeTexturePlatformData> Data) -> void;

	protected:
		auto CreateRenderResourceCandidate(FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> override;

	private:
		auto ResetPlatformData() -> void override { PlatformData.reset(); }
		auto BuildPlatformDataForLoad() -> void override;
		auto LoadCookedPlatformData() -> bool override;

		DPROPERTY(EditorOnly)
		FVolumeTextureBuildSettings BuildSettings;

		std::shared_ptr<FVolumeTexturePlatformData> PlatformData;
	};
}
