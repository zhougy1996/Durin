#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Texture/Texture2DData.h"

namespace Durin
{
	// Value-owned settings frozen before a Texture2D build enters provider code.
	struct FTexture2DBuildSettings
	{
		ETextureUsage Usage = ETextureUsage::Color;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		uint32 MaxResolution = 0;
		std::optional<bool> bSRGB;

		auto operator==(const FTexture2DBuildSettings&) const -> bool = default;
	};

	struct FTexture2DBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 BuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && BuilderVersion != 0;
		}

		auto operator==(const FTexture2DBuildProviderDescriptor&) const -> bool = default;
	};

	struct FTexture2DRecipeMetrics
	{
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PeakIntermediateBytes = 0;
	};

	struct FTexture2DRecipeBuildRequest
	{
		std::span<const Image::FImage> SourceMips;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	struct FTexture2DRecipeBuildProduct
	{
		FTexturePlatformData PlatformData;
		FTexture2DRecipeMetrics Metrics;
	};

	enum class ETexture2DBuildStatus : uint8
	{
		Succeeded,
		Failed,
		Cancelled
	};

	struct FTexture2DBuildResult
	{
		ETexture2DBuildStatus Status = ETexture2DBuildStatus::Failed;
		std::string Diagnostic;

		explicit operator bool() const
		{
			return Status == ETexture2DBuildStatus::Succeeded;
		}
	};

	struct FTexture2DRecipeExecutionControl
	{
		std::function<bool()> ShouldCancel;
		FTexture2DRecipeMetrics* Metrics = nullptr;
	};

	ENGINE_API auto ValidateTexture2DSourceMips(
		std::span<const Image::FImage> Mips, std::string& OutError) -> bool;

	ENGINE_API auto ValidateTexture2DBuildSettings(
		const FTexture2DBuildSettings& Settings,
		std::string& OutError) -> bool;
	ENGINE_API auto ResolveTexture2DSRGB(
		const FTexture2DBuildSettings& Settings) -> bool;

	// Synchronous pure-recipe seam invoked by an Engine-owned worker.
	class ITexture2DBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.Texture2DBuildProvider";
		static constexpr uint32 FeatureVersion = 4;

		virtual auto GetDescriptor() const -> FTexture2DBuildProviderDescriptor = 0;
		virtual auto Build(
			const FTexture2DRecipeBuildRequest& Request,
			FTexture2DRecipeBuildProduct& OutProduct,
			const FTexture2DRecipeExecutionControl* ExecutionControl = nullptr) -> FTexture2DBuildResult = 0;
	};

}
