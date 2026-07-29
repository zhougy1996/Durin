#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

#include "TextureCube.gen.h"

namespace Durin
{
	DENUM(DisplayName = "Texture Cube Source Layout")
	enum class ETextureCubeSourceLayout : uint8
	{
		SixFaces,
		EquirectangularPanorama DMETA(DisplayName = "Equirectangular Panorama"),
	};

	DSTRUCT()
	struct FTextureCubeSourceImportData
	{
		GENERATED_BODY()

		DPROPERTY()
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;

		DPROPERTY()
		FTextureSourceFile PositiveX;

		DPROPERTY()
		FTextureSourceFile NegativeX;

		DPROPERTY()
		FTextureSourceFile PositiveY;

		DPROPERTY()
		FTextureSourceFile NegativeY;

		DPROPERTY()
		FTextureSourceFile PositiveZ;

		DPROPERTY()
		FTextureSourceFile NegativeZ;

		DPROPERTY()
		FTextureSourceFile Panorama;

		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;

		DPROPERTY()
		uint32 ProjectionVersion = 0;

		ENGINE_API auto GetFace(ETextureCubeFace Face) const -> const FTextureSourceFile&;
		ENGINE_API auto GetMutableFace(ETextureCubeFace Face) -> FTextureSourceFile&;
		ENGINE_API auto HasSource() const -> bool;
		auto operator==(const FTextureCubeSourceImportData&) const -> bool = default;
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
	class DTextureCube : public DTexture
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTextureCube(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTextureCube() override;

		ENGINE_API auto GetSourceFile(ETextureCubeFace Face) const -> const std::string&;
		auto GetSourceLayout() const -> ETextureCubeSourceLayout { return SourceLayout; }
		auto GetPanoramaSourceFile() const -> const std::string&
		{
			return SourceImportData.Panorama.SourcePath.Path;
		}
		auto GetSourceImportData() const -> const FTextureCubeSourceImportData& { return SourceImportData; }
		auto GetPanoramaFaceDimension() const -> uint32 { return PanoramaFaceDimension; }
		auto GetPanoramaExposureEV() const -> float { return PanoramaExposureEV; }
		auto GetOriginalSourceWidth() const -> uint32 { return OriginalSourceWidth; }
		auto GetOriginalSourceHeight() const -> uint32 { return OriginalSourceHeight; }
		ENGINE_API auto GetBuiltFaceDimension() const -> uint32;
		ENGINE_API auto GetBuiltMipCount() const -> uint32;
		ENGINE_API auto GetBuiltPixelFormat() const -> EPixelFormat;
		auto GetSourceData() const -> const FTextureCubeSourceData* { return SourceData.get(); }
		auto GetPlatformData() const -> const FTextureCubePlatformData* { return PlatformData.get(); }
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetDerivedDataDiagnostic() const -> const FTextureDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }

		ENGINE_API auto RebuildPlatformData(std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError,
			bool bRetainDiagnosticSourceMetadata = false) -> bool;
		ENGINE_API auto RefreshBuildStatus() -> void;

		ENGINE_API static auto ImportAsset(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			std::string_view AssetPath,
			const FTextureCubeImportSettings& Settings = {},
			const std::array<std::string, TextureCubeFaceCount>& SourceDestinations = {})
			-> FTextureCubeImportResult;
		ENGINE_API static auto ValidateImportSources(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const FTextureCubeImportSettings& Settings = {}) -> FTextureCubeImportValidation;
		ENGINE_API static auto ImportPanoramaAsset(
			std::string_view PanoramaFile,
			std::string_view AssetPath,
			const FTextureCubePanoramaImportSettings& Settings = {},
			std::string_view SourceDestination = {}) -> FTextureCubeImportResult;
		ENGINE_API static auto ValidatePanoramaImportSource(
			std::string_view PanoramaFile,
			const FTextureCubePanoramaImportSettings& Settings = {}) -> FTextureCubeImportValidation;
		ENGINE_API auto ReimportPanorama(
			std::string_view PanoramaFile,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool;
		ENGINE_API auto ReimportSources(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const FTextureCubeImportSettings& Settings,
			std::string& OutError) -> bool;
		ENGINE_API auto ChangePanoramaSourceReference(
			std::string_view SourceVirtualPath,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool;
		ENGINE_API auto ChangeSourceReferences(
			const std::array<std::string, TextureCubeFaceCount>& SourceVirtualPaths,
			const FTextureCubeImportSettings& Settings,
			std::string& OutError) -> bool;
		ENGINE_API auto IngestAndChangePanoramaSource(
			std::string_view FilePath,
			std::string_view TargetSourceVirtualPath,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool;
		ENGINE_API auto IngestAndChangeSources(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const std::array<std::string, TextureCubeFaceCount>& TargetSourceVirtualPaths,
			const FTextureCubeImportSettings& Settings,
			std::string& OutError) -> bool;

	protected:
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;

	private:
		auto ResolvePanoramaSource() const -> std::filesystem::path;
		auto InvalidatePlatformData() -> void;

		DPROPERTY(DisplayName = "Source Layout")
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;

		DPROPERTY()
		FTextureCubeSourceImportData SourceImportData;

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

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::unique_ptr<FTextureCubeSourceData> SourceData;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		std::string DerivedDataKey;
		FTextureDerivedDataDiagnostic DerivedDataDiagnostic;
		bool bLoadedFromDerivedDataCache = false;
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;

		auto LoadCookedPlatformData(std::string& OutError) -> bool;
	};

	struct FTextureCubeImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTextureCube* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
