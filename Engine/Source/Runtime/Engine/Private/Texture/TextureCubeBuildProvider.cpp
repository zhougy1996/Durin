#include "Texture/TextureCubeBuildProvider.h"

#include "Threading/RunnableThread.h"

namespace Durin
{
	auto InvokeTextureCubeBuildProvider(
		const FTextureCubeBuildRequest& Request,
		FTextureCubeCanonicalBuildInput& OutCanonicalInput,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutCanonicalInput = {};
		OutProduct = {};
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITextureCubeBuildProvider>([&](ITextureCubeBuildProvider& Provider) {
				const FTextureCubeBuildProviderDescriptor Descriptor =
					Provider.GetDescriptor();
				if (!Descriptor.IsValid())
				{
					OutError = "The TextureCube build provider descriptor is invalid.";
					return false;
				}
				if (!Provider.Build(Request, OutCanonicalInput, OutProduct, OutError))
					return false;
				OutProduct.Provider = Descriptor;
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
	}

	auto BuildTextureCubeSynchronously(
		DTextureCube& Texture,
		const FTextureCubeBuildRequest& Request,
		const FTextureCubePublicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
		return InvokeTextureCubeBuildProvider(
			Request, CanonicalInput, Product, OutError)
			&& PublishTextureCubeProduct(Texture, std::move(CanonicalInput),
				std::move(Product), Context, OutError);
	}

	auto PublishTextureCubeProduct(
		DTextureCube& Texture,
		FTextureCubeCanonicalBuildInput CanonicalInput,
		FTextureCubeBuildProduct Product,
		const FTextureCubePublicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!CanonicalInput.ImportedData.IsValid()
			|| !Product.PlatformData || !Product.PlatformData->IsValid()
			|| Product.DerivedDataKey.empty())
		{
			OutError = "TextureCube publication requires canonical source, platform data, and a derived-data key.";
			return false;
		}
		const bool bCacheHit = Product.Origin == ETextureCubeBuildProductOrigin::CacheHit;
		const std::string DiagnosticKey = Product.DerivedDataKey;
		Texture.PublishBuildProduct(std::move(CanonicalInput.ImportedData),
			CanonicalInput.SourceLayout, CanonicalInput.PanoramaFaceDimension,
			CanonicalInput.PanoramaExposureEV, CanonicalInput.OriginalSourceWidth,
			CanonicalInput.OriginalSourceHeight, CanonicalInput.bSRGB,
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			{.Status = bCacheHit ? ETextureDerivedDataStatus::Hit
					: ETextureDerivedDataStatus::Rebuilt,
				.Key = DiagnosticKey,
				.Message = bCacheHit
					? "Loaded TextureCube build candidate from DDC."
					: !Product.PersistenceDiagnostic.empty()
						? std::format("Built TextureCube; DDC persistence was best effort: {}",
							Product.PersistenceDiagnostic)
						: "Built TextureCube from canonical normalized source.",
				.bSourceDecoderInvoked = Context.bSourceDecoderInvoked},
			Context.bMarkPackageDirty);
		OutError.clear();
		return true;
	}
}
