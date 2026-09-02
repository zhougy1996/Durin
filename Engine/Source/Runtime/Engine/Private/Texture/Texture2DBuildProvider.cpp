#include "Texture/Texture2DBuildProvider.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"

namespace Durin
{
	auto ValidateTexture2DBuildSettings(
		const FTexture2DBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!IsValidTextureUsage(Settings.Usage)
			|| !IsValidTextureCompressionQuality(Settings.CompressionQuality)
			|| !IsValidTextureAlphaMipMode(Settings.AlphaMipMode)
			|| !IsValidTextureAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D build settings are invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto ResolveTexture2DSRGB(const FTexture2DBuildSettings& Settings) -> bool
	{
		return Settings.bSRGB.value_or(GetDefaultTextureSRGB(Settings.Usage));
	}

	auto InvokeTexture2DBuildProvider(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		OutIdentity = {
			.ImportedDataIdentity = Request.SourceData.GetImportedDataIdentity(),
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		OutIdentity.Settings.bSRGB = ResolveTexture2DSRGB(Request.Settings);
#if !DURIN_WITH_EDITOR
		OutError = "Texture2D authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITexture2DBuildProvider>([&](ITexture2DBuildProvider& Provider) {
				OutIdentity.Provider = Provider.GetDescriptor();
				if (!OutIdentity.Provider.IsValid())
				{
					OutError = "The Texture2D build provider descriptor is invalid.";
					return false;
				}
				const FTexture2DBuildKeyInput KeyInput{
					.ImportedDataIdentity = OutIdentity.ImportedDataIdentity,
					.Usage = Request.Settings.Usage,
					.bSRGB = ResolveTexture2DSRGB(Request.Settings),
					.CompressionQuality = Request.Settings.CompressionQuality,
					.AlphaMipMode = Request.Settings.AlphaMipMode,
					.MaximumResolution = Request.Settings.MaxResolution,
					.AlphaCoverageThreshold = Request.Settings.AlphaCoverageThreshold,
					.BuilderVersion = OutIdentity.Provider.BuilderVersion,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile};
				const std::string Key = BuildTexture2DDerivedDataKey(KeyInput);
				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				FTexturePlatformData PlatformData;
				if (TextureDerivedDataCache::Load(
					TextureDerivedDataCache::Texture2DBucket, Key,
					Request.TargetPlatform, Request.TargetProfile,
					PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					OutProduct = {.PlatformData = std::move(PlatformData),
						.DerivedDataKey = Key,
						.Provider = OutIdentity.Provider,
						.Origin = ETexture2DBuildProductOrigin::CacheHit};
					return true;
				}
				if (ExecutionControl && ExecutionControl->ShouldCancel
					&& ExecutionControl->ShouldCancel())
				{
					OutError = "Texture2D build was cancelled.";
					return false;
				}

				FTexture2DRecipeBuildProduct RecipeProduct;
				FTexture2DRecipeMetrics RecipeMetrics;
				const FTexture2DRecipeExecutionControl RecipeControl{
					.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
						: std::function<bool()>{},
					.Metrics = &RecipeMetrics};
				if (!Provider.Build({
					.SourceData = std::cref(Request.SourceData),
					.Settings = Request.Settings,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile},
					RecipeProduct, OutError, &RecipeControl)) return false;
				if (!RecipeProduct.PlatformData.IsValid())
				{
					OutError = "Texture2D provider returned invalid platform data.";
					return false;
				}
				if (ExecutionControl && ExecutionControl->ShouldCancel
					&& ExecutionControl->ShouldCancel())
				{
					OutError = "Texture2D build was cancelled.";
					return false;
				}

				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
				{
					if (ExecutionControl && ExecutionControl->OnPersisting)
						ExecutionControl->OnPersisting();
					TextureDerivedDataCache::Store(
						TextureDerivedDataCache::Texture2DBucket, Key,
						Request.TargetPlatform, Request.TargetProfile,
						RecipeProduct.PlatformData, StoreDiagnostic);
					RecipeMetrics.PersistenceNanoseconds =
						StoreDiagnostic.DurationNanoseconds;
				}
				if (ExecutionControl && ExecutionControl->Metrics)
					*ExecutionControl->Metrics = RecipeMetrics;
				OutProduct = {.PlatformData = std::move(RecipeProduct.PlatformData),
					.DerivedDataKey = Key,
					.PersistenceDiagnostic = !StoreDiagnostic.Message.empty()
						? std::move(StoreDiagnostic.Message)
						: std::move(CacheDiagnostic.Message),
					.Provider = OutIdentity.Provider,
					.Metrics = RecipeMetrics,
					.Origin = ETexture2DBuildProductOrigin::Rebuilt};
				return true;
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutProduct = {};
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
			OutError = "The Texture2D build provider is unavailable.";
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "Multiple Texture2D build providers are registered.";
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "The Texture2D build provider invocation failed.";
		else if (OutError.empty())
			OutError = "The Texture2D build provider failed without a diagnostic.";
		return false;
#endif
	}
}
