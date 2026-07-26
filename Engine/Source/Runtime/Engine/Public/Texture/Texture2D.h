#pragma once

#include "EngineAPI.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "PixelFormat.h"

#include "Texture2D.gen.h"

namespace Durin
{
	class FTexture2DRenderResource;

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

	// Reports the persistent result of source decoding, platform build, and GPU upload.
	DENUM(DisplayName = "Texture Build Status")
	enum class ETextureBuildStatus : uint8
	{
		Unbuilt DMETA(DisplayName = "Not Built"), // No valid platform data is installed.
		Ready,             // Platform data is valid and its render build is queued.
		MissingSource,     // The copied source path is empty or missing.
		DecodeFailure,     // Source bytes could not be decoded.
		BuildFailure,      // Platform-data construction failed.
		UploadFailure,     // The current render-resource revision failed.
		UnsupportedFormat, // The selected platform format is unavailable.
	};

	// Tracks the revisioned render-thread lifecycle of a texture resource.
	DENUM()
	enum class ERenderResourceState : uint8
	{
		Idle,
		Pending,
		Building,
		Ready,
		Failed,
		Released,
	};

	// Identifies the current render-resource revision's actionable failure boundary.
	enum class ETextureRenderFailure : uint8
	{
		None,
		UnsupportedFormat,
		CreateOrUpload,
	};

	enum class ETextureDerivedDataStatus : uint8
	{
		None,
		Hit,
		Missing,
		Corrupt,
		Incompatible,
		Rebuilt,
		WriteFailure,
		SourceUnavailableCached,
		SourceUnavailable,
		CookedLoaded,
		CookedFailure
	};

	struct FTextureDerivedDataDiagnostic
	{
		ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
		std::string Key;
		std::string Message;
		bool bSourceDecoderInvoked = false;
	};

	// Identifies one portable editor source file without retaining a workstation path.
	DSTRUCT()
	struct FTextureSourceFile
	{
		GENERATED_BODY()

		// Project- or engine-relative path beneath SourceAssets/Textures.
		DPROPERTY()
		std::string SourcePath;

		DPROPERTY()
		uint64 SourceContentHashLow = 0;

		DPROPERTY()
		uint64 SourceContentHashHigh = 0;

		auto HasSource() const -> bool { return !SourcePath.empty(); }
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

	enum class ETextureSourceStatus : uint8
	{
		NoSource,
		Available,
		Missing,
		Invalid
	};

	struct FTextureSourceDiagnostic
	{
		ETextureSourceStatus Status = ETextureSourceStatus::NoSource;
		std::string PhysicalPath;
		std::string Message;
	};

	// Owns decoded source pixels before platform-specific conversion.
	struct FTextureSourceData
	{
		std::vector<uint8> Pixels;
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
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;

		ENGINE_API auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	// Owns the pixel format and complete mip chain consumed by the render resource.
	struct FTexturePlatformData
	{
		std::vector<FTexture2DMipData> Mips;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
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
		ETextureUsage Usage = ETextureUsage::Color;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		uint32 MaxResolution = 0;

		// Empty selects the preset default. Keeping this override explicit prevents a
		// usage change from silently preserving an incompatible color space.
		std::optional<bool> bSRGB;
	};

	// Owns imported texture source, derived platform data, and a cross-thread render proxy.
	DCLASS()
	class DTexture2D : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTexture2D(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTexture2D() override;

		auto GetSourceFile() const -> const std::string&
		{
			return SourceImportData.Source.SourcePath;
		}
		auto GetSourceImportData() const -> const FTexture2DSourceImportData& { return SourceImportData; }
		auto GetSourceData() const -> const FTextureSourceData* { return SourceData.get(); }
		auto GetSourceContentHash() const -> const std::string& { return SourceContentHash; }
		auto GetSourceWidth() const -> uint32 { return SourceWidth; }
		auto GetSourceHeight() const -> uint32 { return SourceHeight; }
		auto GetSourceChannelCount() const -> uint8 { return SourceChannelCount; }
		auto SourceHasTransparency() const -> bool { return bSourceHasTransparency; }
		auto GetPlatformData() const -> const FTexturePlatformData* { return PlatformData.get(); }
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetDerivedDataDiagnostic() const -> const FTextureDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		auto GetRenderResource() const -> const std::shared_ptr<FTexture2DRenderResource>& { return RenderResource; }
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }
		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetMaxResolution() const -> uint32 { return MaxResolution; }
		auto GetCompressionQuality() const -> ETextureCompressionQuality { return CompressionQuality; }
		auto GetAlphaMipMode() const -> ETextureAlphaMipMode { return AlphaMipMode; }
		auto GetAlphaCoverageThreshold() const -> float { return AlphaCoverageThreshold; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }
		ENGINE_API auto SetUsage(ETextureUsage InUsage, std::string& OutError) -> bool;
		ENGINE_API auto SetSRGB(bool bInSRGB, std::string& OutError) -> bool;
		ENGINE_API auto SetMaxResolution(uint32 InMaxResolution, std::string& OutError) -> bool;
		ENGINE_API auto SetCompressionQuality(ETextureCompressionQuality InQuality, std::string& OutError) -> bool;
		ENGINE_API auto SetAlphaMipMode(ETextureAlphaMipMode InMode, std::string& OutError) -> bool;
		ENGINE_API auto SetAlphaCoverageThreshold(float InThreshold, std::string& OutError) -> bool;

		ENGINE_API auto RebuildPlatformData(std::string& OutError) -> bool;
		ENGINE_API auto InspectSource() const -> FTextureSourceDiagnostic;
		ENGINE_API auto ReimportSource(std::string_view FilePath, std::string& OutError) -> bool;
		ENGINE_API auto RepairSourcePath(std::string_view FilePath, std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		// Contributes a validated TXPL object and descriptor-bearing runtime metadata to a cook.
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError,
			bool bRetainDiagnosticSourceMetadata = false) -> bool;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto RefreshBuildStatus() -> void;

		ENGINE_API static auto ImportAsset(std::string_view FilePath, std::string_view AssetPath, const FTexture2DImportSettings& Settings = {}) -> FTexture2DImportResult;

	private:
		auto BuildSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto DecodeSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto EnsureSourceData(std::string& OutError) -> bool;
		auto UpdateSourceFingerprint(const std::filesystem::path& PhysicalFilePath) -> void;
		auto BuildPlatformData(ETextureUsage InUsage, bool bInSRGB, uint32 InMaxResolution,
			ETextureCompressionQuality InCompressionQuality, ETextureAlphaMipMode InAlphaMipMode,
			float InAlphaCoverageThreshold,
			std::unique_ptr<FTexturePlatformData>& OutPlatformData, std::string& OutError) const -> bool;
		auto InvalidatePlatformData() -> void;
		auto QueueRenderResourceBuild() -> void;
		auto LoadCookedPlatformData(std::string& OutError) -> bool;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		FTexture2DSourceImportData SourceImportData;

		// Imported content identity and lightweight diagnostics remain in the package
		// so a warm derived-data load does not need to decode the source image.
		DPROPERTY()
		std::string SourceContentHash;

		DPROPERTY()
		uint64 SourceFileSize = 0;

		DPROPERTY()
		int64 SourceLastWriteTime = 0;

		DPROPERTY()
		uint32 SourceWidth = 0;

		DPROPERTY()
		uint32 SourceHeight = 0;

		DPROPERTY()
		uint8 SourceChannelCount = 0;

		DPROPERTY()
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

		// The shared proxy can outlive this UObject while queued render commands drain.
		// Its RHI member is intentionally never accessed through DTexture2D.
		std::shared_ptr<FTexture2DRenderResource> RenderResource;
		uint64 BuildRevision = 0;

		// Persistent failure state. Set by the build pipeline and cleared on success.
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;

		// Reflected edits validate and build detached settings before live storage
		// changes. PostEditChangeProperty consumes this exact candidate atomically.
		std::unique_ptr<FTexturePlatformData> PendingEditPlatformData;
		ETextureUsage PendingEditUsage = ETextureUsage::Color;
		bool bPendingEditSRGB = true;
		uint32 PendingEditMaxResolution = 0;
		ETextureCompressionQuality PendingEditCompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode PendingEditAlphaMipMode = ETextureAlphaMipMode::Average;
		float PendingEditAlphaCoverageThreshold = 0.5f;
	};
}
