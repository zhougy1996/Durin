#pragma once

#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/Cook.h"
#include "Asset/EditorBulkData.h"
#include "DObject/ObjectPtr.h"
#include "EngineAPI.h"
#include "PixelFormat.h"
#include "RHIResources.h"
#include "Texture/Texture.h"

#include "Texture2D.gen.h"

namespace Durin
{
	class FArchive;
	struct FTextureBuildOperations;

	inline constexpr FGuid Texture2DImportedPixelsPayloadId{
		0x7f3301ba, 0x7c9f45c6, 0x8a8ab67c, 0xc85dc65e};
	inline constexpr uint32 Texture2DImportedDataSchemaVersion = 1;
	inline constexpr uint64 MaximumTexture2DImportedPixelBytes =
		512ull * 1024ull * 1024ull;

	// Identifies the decoded pixel layout retained as editable texture source data.
	DENUM()
	enum class ETextureSourceFormat : uint8
	{
		Invalid,
		RGBA8
	};

	// Selects semantic color handling and platform-build defaults for a texture.
	DENUM()
	enum class ETextureUsage : uint8
	{
		Color,
		Normal,
		DataMask DMETA(DisplayName = "Data / Mask")
	};

	// Controls the offline desktop block-compression search effort.
	DENUM(DisplayName = "Texture Compression Quality")
	enum class ETextureCompressionQuality : uint8
	{
		Low,
		Normal,
		High
	};

	// Controls how Color texture alpha is filtered into smaller mip levels.
	DENUM(DisplayName = "Texture Alpha Mip Mode")
	enum class ETextureAlphaMipMode : uint8
	{
		Average,
		PreserveCoverage DMETA(DisplayName = "Preserve Coverage")
	};

	// Owns decoded source pixels before platform-specific conversion.
	struct FTextureSourceData
	{
		std::vector<std::byte> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;
		bool bHasTransparency = false;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetImportedDataIdentity() const -> FXxHash128;
	};

	// Owns the decoder-free RGBA8 value from which every Texture2D build is reconstructed.
	DSTRUCT()
	struct FTexture2DImportedData
	{
		GENERATED_BODY()

		DPROPERTY()
		Asset::FEditorBulkData Pixels;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint8 SourceChannelCount = 0;

		DPROPERTY()
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;

		DPROPERTY()
		bool bHasTransparency = false;

		DPROPERTY()
		uint32 SchemaVersion = Texture2DImportedDataSchemaVersion;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto SetSourceData(const FTextureSourceData& Source) -> bool;
		ENGINE_API auto ToSourceData() const -> FTextureSourceData;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
	};

	// Owns one tightly described platform mip and its byte row pitch.
	struct FTexture2DMipData
	{
		std::vector<std::byte> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;

		ENGINE_API auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	// Supplies the stable target identity carried by a serialized texture platform value.
	struct FTexturePlatformSerializationContext
	{
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;
	};

	// Owns the pixel format and complete mip chain consumed by the render resource.
	struct FTexturePlatformData
	{
		std::vector<FTexture2DMipData> Mips;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		// Serializes the canonical TXPL value for DDC and cooked payload boundaries.
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const FTexturePlatformSerializationContext& Context) -> void;
	};

	class DTexture2D;

	// Overrides usage-derived texture import defaults.
	struct FTexture2DImportSettings
	{
		ETextureUsage Usage = ETextureUsage::Color;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		uint32 MaxResolution = 0;

		// Empty selects the preset default. Keeping this override explicit prevents a
		// usage change from silently preserving an incompatible color space.
		std::optional<bool> bSRGB;
	};

	// Complete object-free imported state accepted by the Engine publication seam.
	// Build modules construct this value after their detached build succeeds.
	struct FTexture2DImportedState
	{
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		std::string BuildDiagnostic;
		ETextureUsage Usage = ETextureUsage::Color;
		bool bSRGB = true;
		uint32 MaxResolution = 0;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		bool bMarkPackageDirty = true;
		bool bReportLoadMutation = false;
		bool bSourceDecoderInvoked = true;
	};

	// Owns imported texture source, derived platform data, and its render resources.
	DCLASS()
	class DTexture2D : public DTexture
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTexture2D(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTexture2D() override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		// Publishes a validated owner-compatible import-data object after the
		// completed imported state has been applied on the editor thread.
		ENGINE_API auto PublishAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
		auto GetSourceData() const -> const FTextureSourceData* { return SourceData.get(); }
		auto GetImportedData() const -> const FTexture2DImportedData&
		{
			return ImportedData;
		}
		auto GetImportedDataIdentity() const -> FXxHash128
		{
			return ImportedData.GetIdentity();
		}
		auto GetSourceWidth() const -> uint32 { return SourceWidth; }
		auto GetSourceHeight() const -> uint32 { return SourceHeight; }
		auto GetSourceChannelCount() const -> uint8 { return SourceChannelCount; }
		auto SourceHasTransparency() const -> bool { return bSourceHasTransparency; }
		ENGINE_API auto GetPlatformData() const -> const FTexturePlatformData*;
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetDerivedDataDiagnostic() const -> const FTextureDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPlatformData() const -> const Asset::FBulkData& { return CookedPlatformData; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetMaxResolution() const -> uint32 { return MaxResolution; }
		auto GetCompressionQuality() const -> ETextureCompressionQuality { return CompressionQuality; }
		auto GetAlphaMipMode() const -> ETextureAlphaMipMode { return AlphaMipMode; }
		auto GetAlphaCoverageThreshold() const -> float { return AlphaCoverageThreshold; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		// Contributes a validated TXPL object and descriptor-bearing runtime metadata to a cook.
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto RefreshBuildStatus() -> void;
		ENGINE_API auto PublishImportedState(
			FTexture2DImportedState State,
			std::string& OutError) -> bool;
		// Narrow value-publication seams for editor-owned uncooked load policy.
		ENGINE_API auto PublishDerivedDataLoad(
			std::unique_ptr<FTexturePlatformData> InPlatformData,
			std::string InDerivedDataKey,
			std::string& OutError) -> bool;
		ENGINE_API auto PublishUncookedLoadFailure(
			ETextureDerivedDataStatus DerivedDataStatus,
			ETextureBuildStatus InBuildStatus,
			std::string Message,
			std::string DerivedDataKey = {}) -> bool;
		// Exchanges persisted and derived import state while preserving object
		// identity. Render resources are rebuilt for both objects.
		ENGINE_API auto ExchangeImportedState(DTexture2D& Other) -> void;

protected:
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;

	private:
		auto InvalidatePlatformData() -> void;
		auto LoadCookedPlatformData(std::string& OutError) -> bool;
		// Complete editor-only replay authority. The concrete class is supplied by
		// the owning authoring framework and is absent from Cooked packages.
		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		uint32 SourceWidth = 0;

		DPROPERTY(EditorOnly)
		uint32 SourceHeight = 0;

		DPROPERTY(EditorOnly)
		uint8 SourceChannelCount = 0;

		DPROPERTY(EditorOnly)
		bool bSourceHasTransparency = false;

		DPROPERTY(EditorOnly)
		FTexture2DImportedData ImportedData;

		DPROPERTY()
		ETextureUsage Usage = ETextureUsage::Color;

		DPROPERTY()
		bool bSRGB = true;

		// Zero retains the source-sized base mip. Other values select the largest
		// generated mip whose dimensions both fit within the limit.
		DPROPERTY()
		uint32 MaxResolution = 0;

		DPROPERTY()
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;

		DPROPERTY()
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;

		// Alpha-test threshold used only by PreserveCoverage Color mip generation.
		DPROPERTY()
		float AlphaCoverageThreshold = 0.5f;

		// Both representations are derived from the imported source file. Keeping them
		// separate lets platform builds replace format/mips without mutating edit data.
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;
		Asset::FBulkData CookedPlatformData;
		std::string DerivedDataKey;
		FTextureDerivedDataDiagnostic DerivedDataDiagnostic;
		bool bLoadedFromDerivedDataCache = false;

		// Persistent failure state. Set by the build pipeline and cleared on success.
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;

		std::string LastBuildError;

	};
}
