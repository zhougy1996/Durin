#pragma once

#include "Asset/Cook.h"
#include "Asset/SourcePath.h"
#include "EngineAPI.h"
#include "PixelFormat.h"
#include "RHIResources.h"
#include "Texture/Texture.h"

#include "Texture2D.gen.h"

namespace Durin
{
	class FArchive;
	struct FTextureBuildOperations;

	// Identifies the decoded pixel layout retained as editable texture source data.
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

	// Identifies one portable editor source file without retaining a workstation path.
	DSTRUCT()
	struct FTextureSourceFile
	{
		GENERATED_BODY()

		// Complete portable path to an authoring file in a registered mount.
		DPROPERTY()
		FSourcePath SourcePath;

		DPROPERTY()
		uint64 SourceContentHashLow = 0;

		DPROPERTY()
		uint64 SourceContentHashHigh = 0;

		auto HasSource() const -> bool { return !SourcePath.IsEmpty(); }
		auto HasContentHash() const -> bool
		{
			return SourceContentHashLow != 0 || SourceContentHashHigh != 0;
		}
		auto operator==(const FTextureSourceFile&) const -> bool = default;
	};

	// Stores optional Texture2D source provenance used only for editor rebuild and reimport.
	DSTRUCT()
	struct FTexture2DSourceImportData
	{
		GENERATED_BODY()

		DPROPERTY()
		FTextureSourceFile Source;

		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;

		auto HasSource() const -> bool { return Source.HasSource(); }
		auto operator==(const FTexture2DSourceImportData&) const -> bool = default;
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

	// Reports texture import success and the created asset, when available.
	struct FTexture2DImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTexture2D* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};

	// Overrides usage-derived texture import defaults.
	struct FTexture2DImportSettings
	{
		// Portable mount-relative copy destination for the authoring file.
		// Empty stores the source under Textures using the asset name.
		std::string SourceDestination;
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
	// Authoring modules construct this value after their detached build succeeds.
	struct FTexture2DImportedState
	{
		FTexture2DSourceImportData SourceImportData;
		std::string SourceContentHash;
		uint64 SourceFileSize = 0;
		int64 SourceLastWriteTime = 0;
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		ETextureUsage Usage = ETextureUsage::Color;
		bool bSRGB = true;
		uint32 MaxResolution = 0;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		bool bMarkPackageDirty = true;
		bool bReportLoadMutation = false;
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

		auto GetSourceFile() const -> const std::string&
		{
			return SourceImportData.Source.SourcePath.Path;
		}
		auto GetSourceImportData() const -> const FTexture2DSourceImportData& { return SourceImportData; }
		auto GetImportProvenance() const -> std::string_view
		{
			return ImportProvenance;
		}
		auto GetSourceData() const -> const FTextureSourceData* { return SourceData.get(); }
		auto GetSourceContentHash() const -> const std::string& { return SourceContentHash; }
		auto GetSourceFileSize() const -> uint64 { return SourceFileSize; }
		auto GetSourceWidth() const -> uint32 { return SourceWidth; }
		auto GetSourceHeight() const -> uint32 { return SourceHeight; }
		auto GetSourceChannelCount() const -> uint8 { return SourceChannelCount; }
		auto SourceHasTransparency() const -> bool { return bSourceHasTransparency; }
		auto GetPlatformData() const -> const FTexturePlatformData* { return PlatformData.get(); }
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetDerivedDataDiagnostic() const -> const FTextureDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetMaxResolution() const -> uint32 { return MaxResolution; }
		auto GetCompressionQuality() const -> ETextureCompressionQuality { return CompressionQuality; }
		auto GetAlphaMipMode() const -> ETextureAlphaMipMode { return AlphaMipMode; }
		auto GetAlphaCoverageThreshold() const -> float { return AlphaCoverageThreshold; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }
		ENGINE_API auto InspectSource() const -> FTextureSourceDiagnostic;
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
			bool bSourceAvailable,
			std::string& OutError) -> bool;
		ENGINE_API auto PublishUncookedLoadFailure(
			ETextureDerivedDataStatus DerivedDataStatus,
			ETextureBuildStatus InBuildStatus,
			std::string Message,
			std::string DerivedDataKey = {}) -> bool;
		ENGINE_API auto PublishSourceFingerprint(
			uint64 FileSize, int64 LastWriteTime) -> void;
		ENGINE_API auto PublishImportProvenance(
			std::vector<std::byte> Provenance) -> void;

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
		DPROPERTY(EditorOnly)
		FTexture2DSourceImportData SourceImportData;

		// Opaque editor-framework reproduction metadata. Runtime Engine keeps the
		// bytes without depending on the editor AssetForge schema.
		DPROPERTY(EditorOnly)
		std::string ImportProvenance;

		// Imported content identity and lightweight diagnostics remain in the package
		// so a warm derived-data load does not need to decode the source image.
		DPROPERTY(EditorOnly)
		std::string SourceContentHash;

		DPROPERTY(EditorOnly)
		uint64 SourceFileSize = 0;

		DPROPERTY(EditorOnly)
		int64 SourceLastWriteTime = 0;

		DPROPERTY(EditorOnly)
		uint32 SourceWidth = 0;

		DPROPERTY(EditorOnly)
		uint32 SourceHeight = 0;

		DPROPERTY(EditorOnly)
		uint8 SourceChannelCount = 0;

		DPROPERTY(EditorOnly)
		bool bSourceHasTransparency = false;

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

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		// Both representations are derived from the imported source file. Keeping them
		// separate lets platform builds replace format/mips without mutating edit data.
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		FTextureDerivedDataDiagnostic DerivedDataDiagnostic;
		bool bLoadedFromDerivedDataCache = false;

		// Persistent failure state. Set by the build pipeline and cleared on success.
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;

		std::string LastBuildError;

	};
}
