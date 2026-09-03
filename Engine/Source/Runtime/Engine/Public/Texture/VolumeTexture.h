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

		ENGINE_API auto CreateBuildInput() const -> FVolumeTextureSourceData;
		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		ENGINE_API auto SetAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
		ENGINE_API auto SetSourceData(
			const FVolumeTextureSourceData& Value, std::string& OutError) -> bool;
		ENGINE_API auto SetBuildSettings(
			FVolumeTextureBuildSettings Value, std::string& OutError) -> bool;
		auto GetBuildSettings() const -> const FVolumeTextureBuildSettings& { return BuildSettings; }
		// Returns installed CPU data only; never loads bulk data or updates resources.
		auto GetPlatformData() const -> const FVolumeTexturePlatformData*
		{
			return PlatformData.get();
		}
		// GameThread only. Loads and installs cooked data synchronously when absent,
		// then calls UpdateResource (GPU completion is asynchronous). Does not build
		// authored data. Already-installed data succeeds without another update.
		// On failure, logs the texture path and reason and returns false.
		ENGINE_API auto EnsurePlatformDataLoadedBlocking() -> bool;
		auto HasPlatformData() const -> bool
		{
			return PlatformData && PlatformData->IsValid();
		}
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FVolumeTexturePlatformData> Data,
			std::string& OutError) -> bool;
		auto GetCookedPlatformData() const -> const FBulkData& { return CookedPlatformData; }

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(FCookContext& Context,
			std::string_view VirtualPackagePath, std::string& OutError) -> bool;
	protected:
		auto CreateRenderResourceCandidate(FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;
		auto GetTextureSourceStorage() -> FTextureSource& override { return Source; }
		auto GetTextureSourceStorage() const -> const FTextureSource& override
		{
			return Source;
		}
		auto HasValidPlatformData() const -> bool override { return HasPlatformData(); }

	private:
		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTextureSource Source;

		DPROPERTY(EditorOnly)
		FVolumeTextureBuildSettings BuildSettings;

		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
		FBulkData CookedPlatformData;
		auto LoadCookedPlatformData(std::string& OutError) -> bool;
	};
}
