#include "Texture/VolumeTextureBuildProvider.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyVolumeTextureBuildResult(DVolumeTexture& Texture,
			const FVolumeTextureSourceData& SourceData,
			const FVolumeTextureBuildSettings& Settings,
			FVolumeTextureBuildProduct Product,
			const FVolumeTextureResultApplicationContext& Context,
			std::string& OutError) -> bool;
	}

	auto InvokeVolumeTextureBuildProvider(
		const FVolumeTextureBuildRequest& Request,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
#if !DURIN_WITH_EDITOR
		OutError = "VolumeTexture authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IVolumeTextureBuildProvider>([&](IVolumeTextureBuildProvider& Provider) {
				const FVolumeTextureBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
				if (!Descriptor.IsValid())
				{
					OutError = "The VolumeTexture build provider descriptor is invalid.";
					return false;
				}
				const FVolumeTextureSourceData& Source = Request.SourceData.get();
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
				const std::string Key = BuildVolumeTextureDerivedDataKey(KeyInput, OutError);
				if (Key.empty()) return false;

				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
				if (TextureDerivedDataCache::Load(
					TextureDerivedDataCache::VolumeTextureBucket, Key,
					Request.TargetPlatform, Request.TargetProfile,
					*PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					OutProduct = {.PlatformData = std::move(PlatformData),
						.DerivedDataKey = Key,
						.Provider = Descriptor,
						.Origin = EVolumeTextureBuildProductOrigin::CacheHit};
					return true;
				}

				FVolumeTextureRecipeBuildProduct RecipeProduct;
				if (!Provider.Build({
					.SourceData = std::cref(Source),
					.Settings = Request.Settings,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile}, RecipeProduct, OutError)) return false;
				if (!RecipeProduct.PlatformData || !RecipeProduct.PlatformData->IsValid())
				{
					OutError = "VolumeTexture provider returned invalid platform data.";
					return false;
				}
				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
					TextureDerivedDataCache::Store(
						TextureDerivedDataCache::VolumeTextureBucket, Key,
						Request.TargetPlatform, Request.TargetProfile,
						*RecipeProduct.PlatformData, StoreDiagnostic);
				OutProduct = {.PlatformData = std::move(RecipeProduct.PlatformData),
					.DerivedDataKey = Key,
					.PersistenceDiagnostic = AssetDerivedDataCache::CombineDiagnostics(
						CacheDiagnostic, StoreDiagnostic),
					.Provider = Descriptor,
					.Origin = EVolumeTextureBuildProductOrigin::Rebuilt};
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
			OutError = "The VolumeTexture build provider is unavailable.";
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "Multiple VolumeTexture build providers are registered.";
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "The VolumeTexture build provider invocation failed.";
		else if (OutError.empty())
			OutError = "The VolumeTexture build provider failed without a diagnostic.";
		return false;
#endif
	}

	auto BuildVolumeTextureSynchronously(
		DVolumeTexture& Texture,
		const FVolumeTextureBuildRequest& Request,
		const FVolumeTextureResultApplicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FVolumeTextureBuildProduct Product;
		if (!InvokeVolumeTextureBuildProvider(Request, Product, OutError)) return false;
		return ApplyVolumeTextureBuildResult(Texture, Request.SourceData.get(),
			Request.Settings, std::move(Product), Context, OutError);
	}

	namespace
	{
	auto ApplyVolumeTextureBuildResult(
		DVolumeTexture& Texture,
		const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTextureBuildProduct Product,
		const FVolumeTextureResultApplicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!SourceData.IsValid() || !Product.PlatformData
			|| !Product.PlatformData->IsValid() || Product.DerivedDataKey.empty()
			|| SourceData.Format != Settings.OutputFormat)
		{
			OutError = "VolumeTexture result application requires compatible source, settings, platform data, and key.";
			return false;
		}
		if (!Texture.SetSourceData(SourceData, OutError)
			|| !Texture.SetBuildSettings(Settings, OutError)
			|| !Texture.SetPlatformData(std::move(Product.PlatformData), OutError)) return false;
		Texture.UpdateResource();
		if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
		OutError.clear();
		return true;
	}
	}
}
