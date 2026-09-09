#pragma once

#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

#include "TextureCube.gen.h"

namespace Durin
{
	inline constexpr FGuid TextureCubeImportedFacesPayloadId{
		0x8b2cd073, 0x19654a69, 0x8730d84e, 0x54fd72e1};
	inline constexpr uint32 TextureCubeImportedDataSchemaVersion = 1;
	inline constexpr uint64 MaximumTextureCubeImportedPixelBytes =
		512ull * 1024ull * 1024ull;

	DENUM(DisplayName = "Texture Cube Source Layout")
	enum class ETextureCubeSourceLayout : uint8
	{
		SixFaces,
		EquirectangularPanorama DMETA(DisplayName = "Equirectangular Panorama"),
	};

	// Shares six RGBA8 faces and retains the original per-face import metadata.
	// Faces use Unknown gamma; the cube build settings supply color interpretation.
	struct FTextureCubeSourceData
	{
		std::array<Image::FImage, TextureCubeFaceCount> Faces;
		std::array<uint8, TextureCubeFaceCount> SourceChannelCounts{};
		uint8 TransparencyMask = 0;

		ENGINE_API auto IsValid() const -> bool;
	};

	// Owns the six decoder-free RGBA8 faces used by every cube build.
	DSTRUCT()
	struct FTextureCubeImportedData
	{
		GENERATED_BODY()

		DPROPERTY()
		FEditorBulkData Pixels;

		DPROPERTY()
		uint32 FaceDimension = 0;

		DPROPERTY()
		uint8 SourceChannelCount = 0;

		DPROPERTY()
		uint8 TransparencyMask = 0;

		DPROPERTY()
		uint32 SchemaVersion = TextureCubeImportedDataSchemaVersion;

		FXxHash128 CanonicalSourceIdentity;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto SetSourceData(const FTextureCubeSourceData& Source) -> bool;
		ENGINE_API auto ToSourceData() const -> FTextureCubeSourceData;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
	};

	struct FTextureCubePlatformData
	{
		std::array<FTexturePlatformData, TextureCubeFaceCount> Faces;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		// Serializes the canonical six-slice TXPL value for DDC and Cook.
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const FTexturePlatformSerializationContext& Context) -> void;
	};

	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	// Payload-backed input may require a synchronous read.
	ENGINE_API auto PrepareTextureCubeSource(
		const FTextureCubeImportedData& Value) -> std::optional<FTextureSource>;

	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	// Converts the supplied in-memory panorama.
	ENGINE_API auto PrepareTextureCubePanoramaSource(Image::FImageView Value,
		uint8 SourceChannelCount, uint8 TransparencyMask) -> std::optional<FTextureSource>;

	DCLASS()
	class DTextureCube : public DTexture
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTextureCube(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTextureCube() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetSourceLayout() const -> ETextureCubeSourceLayout { return SourceLayout; }
		auto GetPanoramaFaceDimension() const -> uint32 { return PanoramaFaceDimension; }
		auto GetPanoramaExposureEV() const -> float { return PanoramaExposureEV; }
		auto GetOriginalSourceWidth() const -> uint32 { return OriginalSourceWidth; }
		auto GetOriginalSourceHeight() const -> uint32 { return OriginalSourceHeight; }
		auto IsSRGB() const -> bool { return bSRGB; }
		// GameThread only. Assigns validated settings and cancels pending authored builds.
		ENGINE_API auto SetBuildSettings(ETextureCubeSourceLayout InSourceLayout,
			uint32 InPanoramaFaceDimension, float InPanoramaExposureEV,
			uint32 InOriginalSourceWidth, uint32 InOriginalSourceHeight,
			bool bInSRGB) -> void;

		ENGINE_API auto RebuildPlatformData() -> bool;

		ENGINE_API auto GetBuiltFaceDimension() const -> uint32;
		ENGINE_API auto GetBuiltMipCount() const -> uint32;
		ENGINE_API auto GetBuiltPixelFormat() const -> EPixelFormat;
		// Returns installed CPU data only; never loads bulk data or updates resources.
		auto GetPlatformData() const -> const FTextureCubePlatformData*
		{
			return PlatformData.get();
		}
		// Immutable input identity for uploads and CPU previews; replacement leaves existing readers valid.
		auto GetPlatformDataShared() const -> std::shared_ptr<const FTextureCubePlatformData> { return PlatformData; }
		auto HasPlatformData() const -> bool override
		{
			return PlatformData && PlatformData->IsValid();
		}
		// Adopts data already validated by the producer on GameThread; does not update resources.
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FTextureCubePlatformData> Data) -> void;

	protected:
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> override;

	private:
		auto ResetPlatformData() -> void override { PlatformData.reset(); }
		auto BuildPlatformDataForLoad() -> void override;
		auto LoadCookedPlatformData() -> bool override;

		DPROPERTY(EditorOnly, DisplayName = "Source Layout")
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;

		DPROPERTY(EditorOnly, DisplayName = "Panorama Face Dimension")
		uint32 PanoramaFaceDimension = 0;

		DPROPERTY(EditorOnly, DisplayName = "Panorama Exposure EV")
		float PanoramaExposureEV = 0.0f;

		DPROPERTY(EditorOnly)
		uint32 OriginalSourceWidth = 0;

		DPROPERTY(EditorOnly)
		uint32 OriginalSourceHeight = 0;

		DPROPERTY()
		bool bSRGB = true;

		std::shared_ptr<FTextureCubePlatformData> PlatformData;
	};

}
