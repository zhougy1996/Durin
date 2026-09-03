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

	ENGINE_API auto IsValidTextureUsage(ETextureUsage Usage) -> bool;
	ENGINE_API auto GetDefaultTextureSRGB(ETextureUsage Usage) -> bool;
	ENGINE_API auto IsValidTextureCompressionQuality(
		ETextureCompressionQuality Quality) -> bool;
	ENGINE_API auto IsValidTextureAlphaMipMode(ETextureAlphaMipMode Mode) -> bool;
	ENGINE_API auto IsValidTextureAlphaCoverageThreshold(float Threshold) -> bool;

	// Owns decoded source pixels before platform-specific conversion.
	struct FTextureSourceData
	{
		FByteArray Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;
		bool bHasTransparency = false;

		ENGINE_API auto IsValid() const -> bool;
	};

	// Detached request snapshot of canonical RGBA8 source; never asset-resident.
	DSTRUCT()
	struct FTexture2DImportedData
	{
		GENERATED_BODY()

		FTexture2DImportedData() = default;
		ENGINE_API FTexture2DImportedData(const FTextureSourceData& Source);
		ENGINE_API FTexture2DImportedData(FTextureSourceData&& Source);

		DPROPERTY()
		FEditorBulkData Pixels;

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
		FByteArray Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;

		ENGINE_API auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	// Supplies the stable target identity carried by a serialized texture platform value.
	struct FTexturePlatformSerializationContext
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
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
	class FTextureCompilingManager;

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

	// Owns imported texture source, derived platform data, and its render resources.
	DCLASS()
	class DTexture2D : public DTexture
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTexture2D(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTexture2D() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		ENGINE_API auto SetSourceData(
			const FTexture2DImportedData& Value, std::string& OutError) -> bool;
		ENGINE_API auto SetBuildSettings(ETextureUsage InUsage, bool bInSRGB,
			uint32 InMaxResolution, ETextureCompressionQuality InCompressionQuality,
			ETextureAlphaMipMode InAlphaMipMode, float InAlphaCoverageThreshold,
			std::string& OutError) -> bool;
		ENGINE_API auto GetImportedDataIdentity() const -> FXxHash128;
		ENGINE_API auto CreateBuildInput() const -> FTexture2DImportedData;
		// Returns installed CPU data only; never loads bulk data or updates resources.
		auto GetPlatformData() const -> const FTexturePlatformData*
		{
			return PlatformData.get();
		}
		auto HasPlatformData() const -> bool override
		{
			return PlatformData && PlatformData->IsValid();
		}
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FTexturePlatformData> Data,
			std::string& OutError) -> bool;
		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetMaxResolution() const -> uint32 { return MaxResolution; }
		auto GetCompressionQuality() const -> ETextureCompressionQuality { return CompressionQuality; }
		auto GetAlphaMipMode() const -> ETextureAlphaMipMode { return AlphaMipMode; }
		auto GetAlphaCoverageThreshold() const -> float { return AlphaCoverageThreshold; }
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
	private:
		friend class FTextureCompilingManager;
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	protected:
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> override;

	private:
		auto LoadCookedPlatformData(std::string& OutError) -> bool override;
		DPROPERTY()
		ETextureUsage Usage = ETextureUsage::Color;

		DPROPERTY()
		bool bSRGB = true;

		// Zero retains the source-sized base mip. Other values select the largest
		// generated mip whose dimensions both fit within the limit.
		DPROPERTY(EditorOnly)
		uint32 MaxResolution = 0;

		DPROPERTY(EditorOnly)
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;

		DPROPERTY(EditorOnly)
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;

		// Alpha-test threshold used only by PreserveCoverage Color mip generation.
		DPROPERTY(EditorOnly)
		float AlphaCoverageThreshold = 0.5f;

		// Installed runtime data is rebuilt from Source but has an independent lifetime.
		std::unique_ptr<FTexturePlatformData> PlatformData;
	};
}
