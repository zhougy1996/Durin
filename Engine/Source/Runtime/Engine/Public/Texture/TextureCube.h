#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "RHIDefinitions.h"
#include "Texture/Texture2D.h"

#include "TextureCube.gen.h"

namespace Durin
{
	class FTextureCubeRenderResource;

	DENUM(DisplayName = "Texture Cube Source Layout")
	enum class ETextureCubeSourceLayout : uint8
	{
		SixFaces,
		EquirectangularPanorama DMETA(DisplayName = "Equirectangular Panorama"),
	};

	struct FTextureCubeSourceData
	{
		std::array<FTextureSourceData, TextureCubeFaceCount> Faces;

		ENGINE_API auto IsValid() const -> bool;
	};

	struct FTextureCubePlatformData
	{
		std::array<FTexturePlatformData, TextureCubeFaceCount> Faces;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
	};

	struct FTextureCubeImportSettings
	{
		bool bSRGB = true;
	};

	struct FTextureCubePanoramaImportSettings
	{
		// Zero derives the face dimension from one quarter of the panorama width.
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
	};

	// Summarizes a source after decode, projection, and platform-build validation.
	struct FTextureCubeImportValidation
	{
		bool bValid = false;
		std::string Message;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		uint32 Dimension = 0;
		uint32 MipCount = 0;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		bool bHDR = false;

		explicit operator bool() const { return bValid; }
	};

	struct FTextureCubeImportResult;

	DCLASS()
	class DTextureCube : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTextureCube(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTextureCube() override;

		ENGINE_API auto GetSourceFile(ETextureCubeFace Face) const -> const std::string&;
		auto GetSourceLayout() const -> ETextureCubeSourceLayout { return SourceLayout; }
		auto GetPanoramaSourceFile() const -> const std::string& { return PanoramaSourceFile; }
		auto GetPanoramaFaceDimension() const -> uint32 { return PanoramaFaceDimension; }
		auto GetPanoramaExposureEV() const -> float { return PanoramaExposureEV; }
		auto GetOriginalSourceWidth() const -> uint32 { return OriginalSourceWidth; }
		auto GetOriginalSourceHeight() const -> uint32 { return OriginalSourceHeight; }
		ENGINE_API auto GetBuiltFaceDimension() const -> uint32;
		ENGINE_API auto GetBuiltMipCount() const -> uint32;
		ENGINE_API auto GetBuiltPixelFormat() const -> EPixelFormat;
		auto GetSourceData() const -> const FTextureCubeSourceData* { return SourceData.get(); }
		auto GetPlatformData() const -> const FTextureCubePlatformData* { return PlatformData.get(); }
		auto GetRenderResource() const -> const std::shared_ptr<FTextureCubeRenderResource>& { return RenderResource; }
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }

		ENGINE_API auto RebuildPlatformData(std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto RefreshBuildStatus() -> void;

		ENGINE_API static auto ImportAsset(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			std::string_view AssetPath,
			const FTextureCubeImportSettings& Settings = {}) -> FTextureCubeImportResult;
		ENGINE_API static auto ValidateImportSources(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const FTextureCubeImportSettings& Settings = {}) -> FTextureCubeImportValidation;
		ENGINE_API static auto ImportPanoramaAsset(
			std::string_view PanoramaFile,
			std::string_view AssetPath,
			const FTextureCubePanoramaImportSettings& Settings = {}) -> FTextureCubeImportResult;
		ENGINE_API static auto ValidatePanoramaImportSource(
			std::string_view PanoramaFile,
			const FTextureCubePanoramaImportSettings& Settings = {}) -> FTextureCubeImportValidation;
		ENGINE_API auto ReimportPanorama(
			std::string_view PanoramaFile,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool;

	private:
		auto GetMutableSourceFile(ETextureCubeFace Face) -> std::string&;
		auto ResolvePanoramaSource() const -> std::filesystem::path;
		auto InvalidatePlatformData() -> void;
		auto QueueRenderResourceBuild() -> void;

		DPROPERTY(DisplayName = "Source Layout")
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;

		DPROPERTY(DisplayName = "Positive X Source")
		std::string PositiveXSourceFile;

		DPROPERTY(DisplayName = "Negative X Source")
		std::string NegativeXSourceFile;

		DPROPERTY(DisplayName = "Positive Y Source")
		std::string PositiveYSourceFile;

		DPROPERTY(DisplayName = "Negative Y Source")
		std::string NegativeYSourceFile;

		DPROPERTY(DisplayName = "Positive Z Source")
		std::string PositiveZSourceFile;

		DPROPERTY(DisplayName = "Negative Z Source")
		std::string NegativeZSourceFile;

		DPROPERTY(DisplayName = "Panorama Source")
		std::string PanoramaSourceFile;

		DPROPERTY(DisplayName = "Panorama Face Dimension")
		uint32 PanoramaFaceDimension = 0;

		DPROPERTY(DisplayName = "Panorama Exposure EV")
		float PanoramaExposureEV = 0.0f;

		DPROPERTY()
		uint32 OriginalSourceWidth = 0;

		DPROPERTY()
		uint32 OriginalSourceHeight = 0;

		DPROPERTY()
		bool bSRGB = true;

		std::unique_ptr<FTextureCubeSourceData> SourceData;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		std::shared_ptr<FTextureCubeRenderResource> RenderResource;
		uint64 BuildRevision = 0;
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;
	};

	struct FTextureCubeImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTextureCube* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
