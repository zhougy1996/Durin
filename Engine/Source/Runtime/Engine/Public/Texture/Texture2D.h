#pragma once

#include "Asset/Cook.h"
#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Texture/Texture2DData.h"
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

		// Detached recipe-only mip chain. Persistent authored storage lives in FTextureSource.
		std::vector<FTextureSourceData> SuppliedMips;
		FXxHash128 CanonicalSourceIdentity;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto SetSourceData(const FTextureSourceData& Source) -> bool;
		ENGINE_API auto ToSourceData() const -> FTextureSourceData;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
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

	// Adds 2D build settings and typed platform data to the shared texture state.
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
		ENGINE_API auto SetSourceMipChain(std::span<const Image::FImageView> Mips,
			uint8 SourceChannelCount, uint8 TransparencyMask,
			std::string& OutError) -> bool;
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
		// Adopts data already validated by the producer on GameThread; does not update resources.
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FTexturePlatformData> Data) -> void;
		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetMaxResolution() const -> uint32 { return MaxResolution; }
		auto GetCompressionQuality() const -> ETextureCompressionQuality { return CompressionQuality; }
		auto GetAlphaMipMode() const -> ETextureAlphaMipMode { return AlphaMipMode; }
		auto GetAlphaCoverageThreshold() const -> float { return AlphaCoverageThreshold; }
		ENGINE_API auto PostLoad() -> void override;
	private:
		friend class FTextureCompilingManager;
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	protected:
		auto ValidateSettingsAfterImportOrEdit(
			const FTextureSource& ProposedSource) const -> bool override;
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference)
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
