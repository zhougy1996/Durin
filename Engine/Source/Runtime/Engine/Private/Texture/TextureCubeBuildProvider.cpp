#include "Texture/TextureCubeBuildProvider.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyTextureCubeBuildResult(DTextureCube& Texture,
			FTextureCubeCanonicalBuildInput CanonicalInput,
			FTextureCubeBuildProduct Product,
			const FTextureCubeResultApplicationContext& Context,
			std::string& OutError) -> bool;
	}

	auto InvokeTextureCubeBuildProvider(
		const FTextureCubeBuildRequest& Request,
		FTextureCubeCanonicalBuildInput& OutCanonicalInput,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutCanonicalInput = {};
		OutProduct = {};
#if !DURIN_WITH_EDITOR
		OutError = "TextureCube authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITextureCubeBuildProvider>([&](ITextureCubeBuildProvider& Provider) {
				const FTextureCubeBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
				if (!Descriptor.IsValid())
				{
					OutError = "The TextureCube build provider descriptor is invalid.";
					return false;
				}
				if (!Provider.Normalize(Request, OutCanonicalInput, OutError)) return false;
				const FXxHash128 CanonicalHash = OutCanonicalInput.ImportedData.GetIdentity();
				const FTextureCubeBuildKeyInput KeyInput{
					.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
					.FaceContentHashes = {CanonicalHash, CanonicalHash, CanonicalHash,
						CanonicalHash, CanonicalHash, CanonicalHash},
					.bSRGB = OutCanonicalInput.bSRGB,
					.BuilderVersion = Descriptor.BuilderVersion,
					.ProjectionVersion = Descriptor.ProjectionVersion,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile};
				const FCacheKeyProxy Key = BuildTextureCubeDerivedDataKey(KeyInput, OutError);
				if (!Key.IsValid()) return false;

				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				auto PlatformData = std::make_unique<FTextureCubePlatformData>();
				if (TextureDerivedDataCache::Load(
					Key,
					Request.TargetPlatform, Request.TargetProfile,
					*PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					OutProduct = {.PlatformData = std::move(PlatformData),
						.DerivedDataKey = Key,
						.Provider = Descriptor,
						.Origin = ETextureCubeBuildProductOrigin::CacheHit};
					return true;
				}

				FTextureCubeRecipeBuildProduct RecipeProduct;
				if (!Provider.Build({
					.ImportedData = std::cref(OutCanonicalInput.ImportedData),
					.bSRGB = OutCanonicalInput.bSRGB,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile}, RecipeProduct, OutError)) return false;
				if (!RecipeProduct.PlatformData || !RecipeProduct.PlatformData->IsValid())
				{
					OutError = "TextureCube provider returned invalid platform data.";
					return false;
				}
				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
					TextureDerivedDataCache::Store(
						Key,
						Request.TargetPlatform, Request.TargetProfile,
						*RecipeProduct.PlatformData, StoreDiagnostic);
				OutProduct = {.PlatformData = std::move(RecipeProduct.PlatformData),
					.DerivedDataKey = Key,
					.PersistenceDiagnostic = AssetDerivedDataCache::CombineDiagnostics(
						CacheDiagnostic, StoreDiagnostic),
					.Provider = Descriptor,
					.Origin = ETextureCubeBuildProductOrigin::Rebuilt};
				return true;
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutCanonicalInput = {};
		OutProduct = {};
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
			OutError = "The TextureCube build provider is unavailable.";
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "Multiple TextureCube build providers are registered.";
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "The TextureCube build provider invocation failed.";
		else if (OutError.empty())
			OutError = "The TextureCube build provider failed without a diagnostic.";
		return false;
#endif
	}

	auto BuildTextureCubeSynchronously(
		DTextureCube& Texture,
		const FTextureCubeBuildRequest& Request,
		const FTextureCubeResultApplicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
		return InvokeTextureCubeBuildProvider(Request, CanonicalInput, Product, OutError)
			&& ApplyTextureCubeBuildResult(Texture, std::move(CanonicalInput),
				std::move(Product), Context, OutError);
	}

	namespace
	{
	auto ApplyTextureCubeBuildResult(
		DTextureCube& Texture,
		FTextureCubeCanonicalBuildInput CanonicalInput,
		FTextureCubeBuildProduct Product,
		const FTextureCubeResultApplicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!CanonicalInput.ImportedData.IsValid()
			|| !Product.PlatformData || !Product.PlatformData->IsValid()
			|| !Product.DerivedDataKey.IsValid())
		{
			OutError = "TextureCube result application requires canonical source, platform data, and a derived-data key.";
			return false;
		}
		auto PlatformData = std::move(Product.PlatformData);
		const bool bSourceSet = CanonicalInput.AuthoredPanorama.IsValid()
			? Texture.SetPanoramaSourceData(CanonicalInput.AuthoredPanorama.GetView(),
				Image::GetRawImageFormatInfo(
					CanonicalInput.AuthoredPanorama.GetInfo().Format).ChannelCount,
				0, OutError)
			: Texture.SetSourceData(CanonicalInput.ImportedData, OutError);
		if (!bSourceSet
			|| !Texture.SetBuildSettings(CanonicalInput.SourceLayout,
				CanonicalInput.PanoramaFaceDimension,
				CanonicalInput.PanoramaExposureEV,
				CanonicalInput.OriginalSourceWidth,
				CanonicalInput.OriginalSourceHeight,
				CanonicalInput.bSRGB, OutError)
			|| !Texture.SetPlatformData(std::move(PlatformData), OutError)) return false;
		Texture.UpdateResource();
		if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
		OutError.clear();
		return true;
	}
	}
}
