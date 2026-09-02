#pragma once

#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
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

	struct FTextureCubeSourceData
	{
		std::array<FTextureSourceData, TextureCubeFaceCount> Faces;

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

	DCLASS()
	class DTextureCube : public DTexture
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTextureCube(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTextureCube() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetSourceLayout() const -> ETextureCubeSourceLayout { return SourceLayout; }
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
		auto GetPanoramaFaceDimension() const -> uint32 { return PanoramaFaceDimension; }
		auto GetPanoramaExposureEV() const -> float { return PanoramaExposureEV; }
		auto GetOriginalSourceWidth() const -> uint32 { return OriginalSourceWidth; }
		auto GetOriginalSourceHeight() const -> uint32 { return OriginalSourceHeight; }
		ENGINE_API auto GetBuiltFaceDimension() const -> uint32;
		ENGINE_API auto GetBuiltMipCount() const -> uint32;
		ENGINE_API auto GetBuiltPixelFormat() const -> EPixelFormat;
		auto GetSourceData() const -> const FTextureCubeSourceData* { return SourceData.get(); }
		auto GetImportedData() const -> const FTextureCubeImportedData& { return ImportedData; }
		auto GetImportedDataIdentity() const -> FXxHash128 { return ImportedData.GetIdentity(); }
		ENGINE_API auto GetPlatformData() const -> const FTextureCubePlatformData*;
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetDerivedDataDiagnostic() const -> const FTextureDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPlatformData() const -> const FBulkData& { return CookedPlatformData; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }

		ENGINE_API auto RebuildPlatformData(std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	public:
		ENGINE_API auto RefreshBuildStatus() -> void;

		// Atomically accepts a complete, validated post-load candidate. Engine owns
		// the live object and render-resource transition; production stays external.
		ENGINE_API auto ApplyBuildResult(
			FTextureCubeImportedData InImportedData,
			ETextureCubeSourceLayout InSourceLayout,
			uint32 InPanoramaFaceDimension,
			float InPanoramaExposureEV,
			uint32 InOriginalSourceWidth,
			uint32 InOriginalSourceHeight,
			bool bInSRGB,
			std::unique_ptr<FTextureCubePlatformData> InPlatformData,
			std::string InDerivedDataKey,
			FTextureDerivedDataDiagnostic InDiagnostic,
			bool bMarkPackageDirty = true) -> void;
		ENGINE_API auto PublishDerivedDataLoad(
			std::unique_ptr<FTextureCubePlatformData> InPlatformData,
			std::string InDerivedDataKey,
			std::string& OutError) -> bool;
	protected:
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;

	private:
		auto InvalidatePlatformData() -> void;

		DPROPERTY(DisplayName = "Source Layout")
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;

		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTextureCubeImportedData ImportedData;

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
		FBulkData CookedPlatformData;
		std::string DerivedDataKey;
		FTextureDerivedDataDiagnostic DerivedDataDiagnostic;
		bool bLoadedFromDerivedDataCache = false;
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;

		auto LoadCookedPlatformData(std::string& OutError) -> bool;
	};

}
