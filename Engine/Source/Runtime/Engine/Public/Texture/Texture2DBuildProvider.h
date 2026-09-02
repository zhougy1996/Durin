#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Texture/Texture2D.h"

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

	// Engine-owned orchestration request. Cache persistence policy is intentionally
	// not forwarded through the recipe-provider boundary.
	struct FTexture2DBuildRequest
	{
		FTextureSourceData SourceData;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
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

	// Separates deterministic build/DDC identity from the Engine request serial
	// used to enforce latest-wins result application for one live object.
	struct FTexture2DBuildInputIdentity
	{
		FXxHash128 ImportedDataIdentity;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		FTexture2DBuildProviderDescriptor Provider;

		auto operator==(const FTexture2DBuildInputIdentity&) const -> bool = default;
	};

	struct FTexture2DRecipeMetrics
	{
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PersistenceNanoseconds = 0;
		uint64 PeakIntermediateBytes = 0;
	};

	struct FTexture2DRecipeBuildRequest
	{
		std::reference_wrapper<const FTextureSourceData> SourceData;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	struct FTexture2DRecipeBuildProduct
	{
		FTexturePlatformData PlatformData;
		FTexture2DRecipeMetrics Metrics;
	};

	// Identifies whether the provider returned cached data or ran the local recipe.
	enum class ETexture2DBuildProductOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// This observation/control value is borrowed only for the duration of Build.
	struct FTexture2DBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
		std::function<void()> OnPersisting;
		FTexture2DRecipeMetrics* Metrics = nullptr;
	};

	struct FTexture2DRecipeExecutionControl
	{
		std::function<bool()> ShouldCancel;
		FTexture2DRecipeMetrics* Metrics = nullptr;
	};

	// Detached Engine-owned CPU product. Applying it remains a separate
	// GameThread operation and does not execute provider code.
	struct FTexture2DBuildProduct
	{
		FTexturePlatformData PlatformData;
		std::string DerivedDataKey;
		std::string PersistenceDiagnostic;
		FTexture2DBuildProviderDescriptor Provider;
		FTexture2DRecipeMetrics Metrics;
		ETexture2DBuildProductOrigin Origin = ETexture2DBuildProductOrigin::Rebuilt;
	};

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
		static constexpr uint32 FeatureVersion = 1;

		virtual auto GetDescriptor() const -> FTexture2DBuildProviderDescriptor = 0;
		virtual auto Build(
			const FTexture2DRecipeBuildRequest& Request,
			FTexture2DRecipeBuildProduct& OutProduct,
			std::string& OutError,
			const FTexture2DRecipeExecutionControl* ExecutionControl = nullptr) -> bool = 0;
	};

	// Invokes the single registered provider under its module-owned invocation
	// gate. The returned product and identity contain only Engine-owned values.
	ENGINE_API auto InvokeTexture2DBuildProvider(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr) -> bool;
}
