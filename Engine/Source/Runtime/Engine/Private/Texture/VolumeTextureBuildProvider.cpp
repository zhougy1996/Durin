#include "Texture/VolumeTextureBuildProvider.h"

#include "Threading/RunnableThread.h"

namespace Durin
{
	auto InvokeVolumeTextureBuildProvider(
		const FVolumeTextureBuildRequest& Request,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IVolumeTextureBuildProvider>([&](IVolumeTextureBuildProvider& Provider) {
				const FVolumeTextureBuildProviderDescriptor Descriptor =
					Provider.GetDescriptor();
				if (!Descriptor.IsValid())
				{
					OutError = "The VolumeTexture build provider descriptor is invalid.";
					return false;
				}
				if (!Provider.Build(Request, OutProduct, OutError)) return false;
				OutProduct.Provider = Descriptor;
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
	}

	auto BuildVolumeTextureSynchronously(
		DVolumeTexture& Texture,
		const FVolumeTextureBuildRequest& Request,
		const FVolumeTexturePublicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FVolumeTextureBuildProduct Product;
		if (!InvokeVolumeTextureBuildProvider(Request, Product, OutError)) return false;
		return PublishVolumeTextureProduct(Texture, Request.SourceData.get(),
			Request.Settings, std::move(Product), Context, OutError);
	}

	auto PublishVolumeTextureProduct(
		DVolumeTexture& Texture,
		const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTextureBuildProduct Product,
		const FVolumeTexturePublicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		return Texture.PublishBuiltData(SourceData, Settings,
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			std::move(Product.PersistenceDiagnostic), OutError,
			Product.Origin == EVolumeTextureBuildProductOrigin::CacheHit,
			Context.bMarkPackageDirty, Context.bSourceDecoderInvoked);
	}
}
