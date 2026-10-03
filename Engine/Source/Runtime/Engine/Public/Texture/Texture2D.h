#pragma once

#include "EngineAPI.h"
#include "Texture/Texture2DData.h"
#include "RHIResources.h"
#include "Texture/Texture.h"
#include <expected>

#include "Texture2D.gen.h"

namespace Durin
{
	class FArchive;
	struct FTexture2DBuildRequest;
	struct FTexture2DBuildSettings;
	struct FTexture2DInputError;

	class DTexture2D;
	class FTextureCompilingManager;

#if DURIN_WITH_EDITORONLY_DATA
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

	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	ENGINE_API auto PrepareTexture2DSourceMipChain(std::span<const Image::FImageView> Mips,
		uint8 SourceChannelCount, uint8 TransparencyMask) -> std::optional<FTextureSource>;

#endif

	// Adds 2D build settings and typed platform data to the shared texture state.
	DCLASS()
	class DTexture2D : public DTexture
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTexture2D(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTexture2D() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
#if DURIN_WITH_EDITORONLY_DATA
		auto GetMaxResolution() const -> uint32 { return MaxResolution; }
		auto GetCompressionQuality() const -> ETextureCompressionQuality { return CompressionQuality; }
		auto GetAlphaMipMode() const -> ETextureAlphaMipMode { return AlphaMipMode; }
		auto GetAlphaCoverageThreshold() const -> float { return AlphaCoverageThreshold; }
		// GameThread only. Assigns validated settings and cancels pending authored builds.
		ENGINE_API auto SetBuildSettings(ETextureUsage InUsage, bool bInSRGB,
			uint32 InMaxResolution, ETextureCompressionQuality InCompressionQuality,
			ETextureAlphaMipMode InAlphaMipMode, float InAlphaCoverageThreshold) -> void;

		// Captures a detached source snapshot without reading or decompressing its payload.
		ENGINE_API auto CreateBuildRequest(const FTexture2DBuildSettings& Settings) const
			-> std::expected<FTexture2DBuildRequest, FTexture2DInputError>;
#endif

		// Returns installed CPU data only; never loads bulk data or updates resources.
		auto GetPlatformData() const -> const FTexturePlatformData*
		{
			return PlatformData.get();
		}
		// Immutable input identity for uploads and CPU previews; replacement leaves existing readers valid.
		auto GetPlatformDataShared() const -> std::shared_ptr<const FTexturePlatformData> { return PlatformData; }
		auto HasPlatformData() const -> bool override
		{
			return PlatformData && PlatformData->IsValid();
		}
		// Adopts data already validated by the producer on GameThread; does not update resources.
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FTexturePlatformData> Data) -> void;

	protected:
		auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> override;

	private:
		friend class FTextureCompilingManager;

		auto ResetPlatformData() -> void override { PlatformData.reset(); }
		auto BuildPlatformDataForLoad() -> void override;
		auto LoadCookedPlatformData() -> bool override;

		DPROPERTY()
		ETextureUsage Usage = ETextureUsage::Color;

		DPROPERTY()
		bool bSRGB = true;

#if DURIN_WITH_EDITORONLY_DATA
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
#endif

		// Installed runtime data is rebuilt from Source but has an independent lifetime.
		std::shared_ptr<FTexturePlatformData> PlatformData;
	};
}
