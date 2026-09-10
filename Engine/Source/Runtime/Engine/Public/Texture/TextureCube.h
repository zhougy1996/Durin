#pragma once

#include "EngineAPI.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

#include "TextureCube.gen.h"

namespace Durin
{
	// Selects display-ready LDR or preserved linear panorama radiance.
	DENUM(DisplayName = "Texture Cube Output")
	enum class ETextureCubeOutput : uint8
	{
		LDR = 0,
		HDR = 1,
	};

	DENUM(DisplayName = "Texture Cube Source Layout")
	enum class ETextureCubeSourceLayout : uint8
	{
		SixFaces,
		EquirectangularPanorama DMETA(DisplayName = "Equirectangular Panorama"),
	};

	// Immutable shared RGBA8 images used by decoding, projection, and build recipes.
	// Faces use Unknown gamma; the cube build settings supply color interpretation.
	struct FTextureCubeDecodedFaces
	{
		std::array<Image::FImage, TextureCubeFaceCount> Faces;
		std::array<uint8, TextureCubeFaceCount> SourceChannelCounts{};
		uint8 TransparencyMask = 0;

		ENGINE_API auto IsValid() const -> bool;
	};

	struct FTextureCubePlatformData
	{
		std::array<FTexturePlatformData, TextureCubeFaceCount> Faces;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		// Loads canonical six-slice TXPL in place; discard failures and check the owning byte boundary.
		ENGINE_API auto Serialize(
			FArchive& Ar) -> void;
	};

	// May synchronously load source pixels; returned images retain shared storage independently.
	ENGINE_API auto ReadTextureCubeFaces(const FTextureSource& Source) -> FTextureCubeDecodedFaces;

	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	// Packs the supplied faces into the canonical authored source.
	ENGINE_API auto PrepareTextureCubeSource(
		const FTextureCubeDecodedFaces& Value) -> std::optional<FTextureSource>;

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
		auto GetOutput() const -> ETextureCubeOutput { return Output; }
		auto GetOriginalSourceWidth() const -> uint32 { return OriginalSourceWidth; }
		auto GetOriginalSourceHeight() const -> uint32 { return OriginalSourceHeight; }
		auto IsSRGB() const -> bool { return bSRGB; }
		// GameThread only. Assigns validated settings and cancels pending authored builds.
		ENGINE_API auto SetBuildSettings(ETextureCubeSourceLayout InSourceLayout,
			uint32 InPanoramaFaceDimension, float InPanoramaExposureEV,
			uint32 InOriginalSourceWidth, uint32 InOriginalSourceHeight,
			bool bInSRGB, ETextureCubeOutput InOutput = ETextureCubeOutput::LDR) -> void;

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

		// Missing fields in old packages retain the original LDR recipe.
		DPROPERTY(EditorOnly, DisplayName = "Output Range")
		ETextureCubeOutput Output = ETextureCubeOutput::LDR;

		DPROPERTY(EditorOnly)
		uint32 OriginalSourceWidth = 0;

		DPROPERTY(EditorOnly)
		uint32 OriginalSourceHeight = 0;

		DPROPERTY()
		bool bSRGB = true;

		std::shared_ptr<FTextureCubePlatformData> PlatformData;
	};

}
