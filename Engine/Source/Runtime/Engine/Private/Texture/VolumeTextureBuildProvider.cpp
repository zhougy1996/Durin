#include "Texture/VolumeTextureBuildProvider.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyVolumeTextureBuildResult(DVolumeTexture& Texture, const FVolumeTextureSourceData& SourceData, const FVolumeTextureBuildSettings& Settings, FVolumeTextureBuildProduct Product, const FVolumeTextureResultApplicationContext& Context) -> FTextureBuildOutcome;
	}

	auto InvokeVolumeTextureBuildProvider(const FVolumeTextureBuildRequest& Request)
		-> TTextureBuildResult<FVolumeTextureBuildValue>
	{
		FTextureBuildOutcome Outcome;
		FVolumeTextureBuildProduct Product;
#if !DURIN_WITH_EDITOR
		Outcome.Diagnostic = "VolumeTexture authored build orchestration is unavailable outside editor builds.";
		return {.Outcome = {ETextureBuildFailure::Unavailable, ETextureBuildStage::Provider, std::move(Outcome.Diagnostic)}};
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IVolumeTextureBuildProvider>([&](IVolumeTextureBuildProvider& Provider) {
				const FVolumeTextureBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
				if (!Descriptor.IsValid())
				{
					Outcome.Code = ETextureBuildFailure::InvalidProviderOutput;
					Outcome.Diagnostic = "The VolumeTexture build provider descriptor is invalid.";
					return false;
				}
				const FVolumeTextureSourceData& Source = Request.SourceData.get();
				if (!Source.IsValid() || Source.Format != Request.Settings.OutputFormat
					|| Request.Settings.MipFilter != EVolumeTextureMipFilter::Box)
				{
					Outcome = {ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize, "VolumeTexture source or build settings are invalid or incompatible."};
					return false;
				}
				const FVolumeTextureBuildKeyInput KeyInput{
					.CanonicalSourceIdentity = Source.GetIdentity(),
					.Width = Source.Width,
					.Height = Source.Height,
					.Depth = Source.Depth,
					.Settings = Request.Settings,
					.BuilderVersion = Descriptor.BuilderVersion,
					.SourcePayloadSchemaVersion = Source.PayloadSchemaVersion,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile};
				const FCacheKeyProxy Key = BuildVolumeTextureDerivedDataKey(KeyInput, Outcome.Diagnostic);
				if (!Key.IsValid()) return false;

				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
				if (TextureDerivedDataCache::Load(
					Key,
					Request.TargetPlatform, Request.TargetProfile,
					*PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					Product = {.PlatformData = std::move(PlatformData), .DerivedDataKey = Key, .Provider = Descriptor, .Origin = EVolumeTextureBuildProductOrigin::CacheHit};
					return true;
				}

				Outcome.Stage = ETextureBuildStage::Recipe;
				auto Recipe = Provider.Build({.SourceData = std::cref(Source), .Settings = Request.Settings, .TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
				if (!Recipe)
				{
					Outcome = std::move(Recipe.Outcome);
					return false;
				}
				auto RecipeProduct = std::move(*Recipe.Value);
				if (!RecipeProduct.PlatformData || !RecipeProduct.PlatformData->IsValid())
				{
					Outcome.Code = ETextureBuildFailure::InvalidProviderOutput;
					Outcome.Diagnostic = "VolumeTexture provider returned invalid platform data.";
					return false;
				}
				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
					TextureDerivedDataCache::Store(
						Key,
						Request.TargetPlatform, Request.TargetProfile,
						*RecipeProduct.PlatformData, StoreDiagnostic);
				Product = {.PlatformData = std::move(RecipeProduct.PlatformData), .DerivedDataKey = Key, .PersistenceDiagnostic = AssetDerivedDataCache::CombineDiagnostics(CacheDiagnostic, StoreDiagnostic), .Provider = Descriptor, .Origin = EVolumeTextureBuildProductOrigin::Rebuilt};
				return true;
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			return {.Outcome = {ETextureBuildFailure::None}, .Value = FVolumeTextureBuildValue{std::move(Product)}};
		}
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
		{
			Outcome.Code = ETextureBuildFailure::Unavailable;
			Outcome.Stage = ETextureBuildStage::Provider;
			Outcome.Diagnostic = "The VolumeTexture build provider is unavailable.";
		}
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
		{
			Outcome.Code = ETextureBuildFailure::Ambiguous;
			Outcome.Stage = ETextureBuildStage::Provider;
			Outcome.Diagnostic = "Multiple VolumeTexture build providers are registered.";
		}
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
		{
			Outcome.Code = ETextureBuildFailure::InvocationFailed;
			Outcome.Stage = ETextureBuildStage::Provider;
			Outcome.Diagnostic = "The VolumeTexture build provider invocation failed.";
		}
		else if (Outcome.Diagnostic.empty())
			Outcome.Diagnostic = "The VolumeTexture build provider failed without a diagnostic.";
		if (Outcome.Code == ETextureBuildFailure::None) Outcome.Code = ETextureBuildFailure::InvalidProviderOutput;
		return {.Outcome = std::move(Outcome)};
#endif
	}

	auto BuildVolumeTextureSynchronously(DVolumeTexture& Texture, const FVolumeTextureBuildRequest& Request, const FVolumeTextureResultApplicationContext& Context) -> FTextureBuildOutcome
	{
		CheckGameThread();
		auto Result = InvokeVolumeTextureBuildProvider(Request);
		if (!Result) return std::move(Result.Outcome);
		return ApplyVolumeTextureBuildResult(Texture, Request.SourceData.get(), Request.Settings, std::move(Result.Value->Product), Context);
	}

	namespace
	{
		auto ApplyVolumeTextureBuildResult(
			DVolumeTexture& Texture,
			const FVolumeTextureSourceData& SourceData,
			const FVolumeTextureBuildSettings& Settings,
			FVolumeTextureBuildProduct Product,
			const FVolumeTextureResultApplicationContext& Context
		) -> FTextureBuildOutcome
		{
			CheckGameThread();
			require(Product.PlatformData != nullptr);
			// The provider boundary has already validated these value contracts.
			check(SourceData.IsValid() && SourceData.Format == Settings.OutputFormat && Product.DerivedDataKey.IsValid());
			check(Product.PlatformData->IsValid());
			if (!Context.bPreserveSource)
			{
				auto Source = PrepareVolumeTextureSource(SourceData);
				if (!Source) return {ETextureBuildFailure::ApplicationFailed, ETextureBuildStage::Apply,
					"VolumeTexture source preparation failed; see log for details."};
				Texture.SetSource(std::move(*Source));
			}
			Texture.SetBuildSettings(Settings);
			Texture.SetPlatformData(std::move(Product.PlatformData));
			Texture.UpdateResource();
			if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
			return {ETextureBuildFailure::None};
	}
	}
}
