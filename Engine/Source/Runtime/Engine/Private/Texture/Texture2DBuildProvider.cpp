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
		const FTexture2DBuildExecutionControl* ExecutionControl) -> FTexture2DBuildResult
	{
		OutProduct = {};
		OutIdentity = {
			.ImportedDataIdentity = Request.ImportedData.GetIdentity(),
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		OutIdentity.Settings.bSRGB = ResolveTexture2DSRGB(Request.Settings);
#if !DURIN_WITH_EDITOR
		return {ETexture2DBuildStatus::Failed,
			"Texture2D authored build orchestration is unavailable outside editor builds."};
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITexture2DBuildProvider>([&](ITexture2DBuildProvider& Provider) {
				OutIdentity.Provider = Provider.GetDescriptor();
				if (!OutIdentity.Provider.IsValid())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Failed,
						"The Texture2D build provider descriptor is invalid."};
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
				const FCacheKeyProxy Key = BuildTexture2DDerivedDataKey(KeyInput);
				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				FTexturePlatformData PlatformData;
				if (TextureDerivedDataCache::Load(
					Key,
					Request.TargetPlatform, Request.TargetProfile,
					PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					OutProduct = {.PlatformData = std::move(PlatformData),
						.DerivedDataKey = Key,
						.Provider = OutIdentity.Provider,
						.Origin = ETexture2DBuildProductOrigin::CacheHit};
					return FTexture2DBuildResult{ETexture2DBuildStatus::Succeeded, {}};
				}
				if (ExecutionControl && ExecutionControl->ShouldCancel
					&& ExecutionControl->ShouldCancel())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Cancelled,
						"Texture2D build was cancelled."};
				}

				const FTextureSourceData SourceData = Request.ImportedData.ToSourceData();
				if (!SourceData.IsValid())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Failed,
						"Texture2D imported data could not be materialized."};
				}
				FTexture2DRecipeBuildProduct RecipeProduct;
				FTexture2DRecipeMetrics RecipeMetrics;
				const FTexture2DRecipeExecutionControl RecipeControl{
					.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
						: std::function<bool()>{},
					.Metrics = &RecipeMetrics};
				const FTexture2DBuildResult RecipeResult = Provider.Build({
					.SourceData = std::cref(SourceData),
					.SuppliedMips = Request.ImportedData.SuppliedMips,
					.Settings = Request.Settings,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile},
					RecipeProduct, &RecipeControl);
				if (!RecipeResult) return RecipeResult;
				if (!RecipeProduct.PlatformData.IsValid())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Failed,
						"Texture2D provider returned invalid platform data."};
				}
				if (ExecutionControl && ExecutionControl->ShouldCancel
					&& ExecutionControl->ShouldCancel())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Cancelled,
						"Texture2D build was cancelled."};
				}

				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
				{
					if (ExecutionControl && ExecutionControl->OnPersisting)
						ExecutionControl->OnPersisting();
					TextureDerivedDataCache::Store(
						Key,
						Request.TargetPlatform, Request.TargetProfile,
						RecipeProduct.PlatformData, StoreDiagnostic);
					RecipeMetrics.PersistenceNanoseconds =
						StoreDiagnostic.DurationNanoseconds;
				}
				if (ExecutionControl && ExecutionControl->Metrics)
					*ExecutionControl->Metrics = RecipeMetrics;
				OutProduct = {.PlatformData = std::move(RecipeProduct.PlatformData),
					.DerivedDataKey = Key,
					.PersistenceDiagnostic = AssetDerivedDataCache::CombineDiagnostics(
						CacheDiagnostic, StoreDiagnostic),
					.Provider = OutIdentity.Provider,
					.Metrics = RecipeMetrics,
					.Origin = ETexture2DBuildProductOrigin::Rebuilt};
				return FTexture2DBuildResult{ETexture2DBuildStatus::Succeeded, {}};
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value())
		{
			if (!*Invocation.Value) OutProduct = {};
			return std::move(*Invocation.Value);
		}
		OutProduct = {};
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
			return {ETexture2DBuildStatus::Failed,
				"The Texture2D build provider is unavailable."};
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			return {ETexture2DBuildStatus::Failed,
				"Multiple Texture2D build providers are registered."};
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
			return {ETexture2DBuildStatus::Failed,
				"The Texture2D build provider invocation failed."};
		return {ETexture2DBuildStatus::Failed,
			"The Texture2D build provider failed without a diagnostic."};
#endif
	}
}
