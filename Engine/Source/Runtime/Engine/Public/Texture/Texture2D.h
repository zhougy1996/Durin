#pragma once

#include "Asset/Cook.h"
#include "EngineAPI.h"
#include "Texture/Texture2DData.h"
#include "RHIResources.h"
#include "Texture/Texture.h"

#include "Texture2D.gen.h"

namespace Durin
{
	class FArchive;
	struct FTextureBuildOperations;
	struct FTexture2DBuildRequest;
	struct FTexture2DBuildSettings;

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

		// Replaces authored source on the GameThread and cancels pending builds.
		using DTexture::SetSource;
		ENGINE_API auto SetSourceData(
			const FTextureSourceData& Value, std::string& OutError) -> bool;
		ENGINE_API auto SetSourceMipChain(std::span<const Image::FImageView> Mips,
			uint8 SourceChannelCount, uint8 TransparencyMask,
			std::string& OutError) -> bool;
		ENGINE_API auto SetBuildSettings(ETextureUsage InUsage, bool bInSRGB,
			uint32 InMaxResolution, ETextureCompressionQuality InCompressionQuality,
			ETextureAlphaMipMode InAlphaMipMode, float InAlphaCoverageThreshold,
			std::string& OutError) -> bool;
		// Reads/decompresses source on the caller thread. Failure returns an empty mip chain.
		ENGINE_API auto CreateBuildRequest(const FTexture2DBuildSettings& Settings) const
			-> FTexture2DBuildRequest;
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
