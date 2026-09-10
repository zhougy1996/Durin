#include "Texture/TextureCubeBuildProvider.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyTextureCubeBuildResult(DTextureCube& Texture, FTextureCubeCanonicalBuildInput CanonicalInput, FTextureCubeBuildProduct Product, const FTextureCubeResultApplicationContext& Context) -> FTextureBuildOutcome;
	}

	auto InvokeTextureCubeBuildProvider(const FTextureCubeBuildRequest& Request)
		-> TTextureBuildResult<FTextureCubeBuildValue>
	{
		FTextureBuildOutcome Outcome;
		FTextureCubeBuildProduct Product;
		FTextureCubeCanonicalBuildInput CanonicalInput;
#if !DURIN_WITH_EDITOR
		Outcome.Diagnostic = "TextureCube authored build orchestration is unavailable outside editor builds.";
		return {.Outcome = {ETextureBuildFailure::Unavailable, ETextureBuildStage::Provider, std::move(Outcome.Diagnostic)}};
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITextureCubeBuildProvider>([&](ITextureCubeBuildProvider& Provider) {
				const FTextureCubeBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
				if (!Descriptor.IsValid())
				{
					Outcome.Code = ETextureBuildFailure::InvalidProviderOutput;
					Outcome.Diagnostic = "The TextureCube build provider descriptor is invalid.";
					return false;
				}
				Outcome.Stage = ETextureBuildStage::Normalize;
				auto Normalized = Provider.Normalize(Request);
				if (!Normalized)
				{
					Outcome = std::move(Normalized.Outcome);
					return false;
				}
				CanonicalInput = std::move(*Normalized.Value);
				const bool bHDR = CanonicalInput.Output == ETextureCubeOutput::HDR;
				if ((!bHDR && !CanonicalInput.DecodedFaces.IsValid())
					|| (CanonicalInput.Output != ETextureCubeOutput::LDR && !bHDR)
					|| (bHDR && (!CanonicalInput.AuthoredPanorama.IsValid() || CanonicalInput.bSRGB
						|| CanonicalInput.AuthoredPanorama.GetInfo().Format != Image::ERawImageFormat::RGBA32F
						|| CanonicalInput.SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama))
					|| (CanonicalInput.SourceLayout != ETextureCubeSourceLayout::SixFaces
						&& CanonicalInput.SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
					|| !std::isfinite(CanonicalInput.PanoramaExposureEV)
					|| CanonicalInput.PanoramaExposureEV < MinimumTextureCubePanoramaExposureEV
					|| CanonicalInput.PanoramaExposureEV > MaximumTextureCubePanoramaExposureEV
					|| CanonicalInput.OriginalSourceWidth == 0 || CanonicalInput.OriginalSourceHeight == 0)
				{
					Outcome = {ETextureBuildFailure::InvalidProviderOutput, ETextureBuildStage::Normalize, "TextureCube provider returned invalid canonical input."};
					return false;
				}
				FXxHash128 CanonicalHash = CanonicalInput.SourceIdentity;
				if (bHDR || CanonicalHash.IsZero())
				{
					auto CanonicalSource = bHDR
						? PrepareTextureCubePanoramaSource(CanonicalInput.AuthoredPanorama.GetView(), 4, 0)
						: PrepareTextureCubeSource(CanonicalInput.DecodedFaces);
					if (!CanonicalSource)
					{
						Outcome = {ETextureBuildFailure::InvalidProviderOutput, ETextureBuildStage::Normalize,
							"TextureCube canonical source preparation failed."};
						return false;
					}
					CanonicalHash = CanonicalSource->GetIdentity();
				}
				const FTextureCubeBuildKeyInput KeyInput{
					.SourceLayout = bHDR ? ETextureCubeBuildSourceLayout::EquirectangularPanorama
						: ETextureCubeBuildSourceLayout::SixFaces,
					.FaceContentHashes = {CanonicalHash, CanonicalHash, CanonicalHash, CanonicalHash, CanonicalHash, CanonicalHash},
					.PanoramaContentHash = bHDR ? CanonicalHash : FXxHash128{},
					.FaceDimension = bHDR ? CanonicalInput.PanoramaFaceDimension : 0,
					.ExposureEV = bHDR && CanonicalInput.PanoramaExposureEV != 0.0f ? CanonicalInput.PanoramaExposureEV : 0.0f,
					.bSRGB = CanonicalInput.bSRGB,
					.BuilderVersion = Descriptor.BuilderVersion,
					.ProjectionVersion = Descriptor.ProjectionVersion,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile
				};
				const FCacheKeyProxy Key = BuildTextureCubeDerivedDataKey(KeyInput, Outcome.Diagnostic);
				if (!Key.IsValid()) return false;

				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				auto PlatformData = std::make_unique<FTextureCubePlatformData>();
				if (TextureDerivedDataCache::Load(
					Key,
					Request.TargetPlatform, Request.TargetProfile,
					*PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					Product = {.PlatformData = std::move(PlatformData), .DerivedDataKey = Key, .Provider = Descriptor, .Origin = ETextureCubeBuildProductOrigin::CacheHit};
					return true;
				}

				Outcome.Stage = ETextureBuildStage::Recipe;
				auto Recipe = Provider.Build({.DecodedFaces = std::cref(CanonicalInput.DecodedFaces),
					.bSRGB = CanonicalInput.bSRGB, .TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile,
					.HDRPanorama = bHDR ? &CanonicalInput.AuthoredPanorama : nullptr,
					.PanoramaSettings = {.FaceDimension = CanonicalInput.PanoramaFaceDimension,
						.ExposureEV = CanonicalInput.PanoramaExposureEV, .Output = CanonicalInput.Output}});
				if (!Recipe)
				{
					Outcome = std::move(Recipe.Outcome);
					return false;
				}
				auto RecipeProduct = std::move(*Recipe.Value);
				if (!RecipeProduct.PlatformData || !RecipeProduct.PlatformData->IsValid())
				{
					Outcome.Code = ETextureBuildFailure::InvalidProviderOutput;
					Outcome.Diagnostic = "TextureCube provider returned invalid platform data.";
					return false;
				}
				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
					TextureDerivedDataCache::Store(
						Key,
						Request.TargetPlatform, Request.TargetProfile,
						*RecipeProduct.PlatformData, StoreDiagnostic);
				Product = {.PlatformData = std::move(RecipeProduct.PlatformData), .DerivedDataKey = Key, .PersistenceDiagnostic = AssetDerivedDataCache::CombineDiagnostics(CacheDiagnostic, StoreDiagnostic), .Provider = Descriptor, .Origin = ETextureCubeBuildProductOrigin::Rebuilt};
				return true;
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			return {.Outcome = {ETextureBuildFailure::None}, .Value = FTextureCubeBuildValue{std::move(CanonicalInput), std::move(Product)}};
		}
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
		{
			Outcome.Code = ETextureBuildFailure::Unavailable;
			Outcome.Stage = ETextureBuildStage::Provider;
			Outcome.Diagnostic = "The TextureCube build provider is unavailable.";
		}
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
		{
			Outcome.Code = ETextureBuildFailure::Ambiguous;
			Outcome.Stage = ETextureBuildStage::Provider;
			Outcome.Diagnostic = "Multiple TextureCube build providers are registered.";
		}
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
		{
			Outcome.Code = ETextureBuildFailure::InvocationFailed;
			Outcome.Stage = ETextureBuildStage::Provider;
			Outcome.Diagnostic = "The TextureCube build provider invocation failed.";
		}
		else if (Outcome.Diagnostic.empty())
			Outcome.Diagnostic = "The TextureCube build provider failed without a diagnostic.";
		if (Outcome.Code == ETextureBuildFailure::None) Outcome.Code = ETextureBuildFailure::InvalidProviderOutput;
		return {.Outcome = std::move(Outcome)};
#endif
	}

	auto BuildTextureCubeSynchronously(DTextureCube& Texture, const FTextureCubeBuildRequest& Request, const FTextureCubeResultApplicationContext& Context) -> FTextureBuildOutcome
	{
		CheckGameThread();
		auto Result = InvokeTextureCubeBuildProvider(Request);
		if (!Result) return std::move(Result.Outcome);
		return ApplyTextureCubeBuildResult(Texture, std::move(Result.Value->CanonicalInput), std::move(Result.Value->Product), Context);
	}

	namespace
	{
		auto ApplyTextureCubeBuildResult(
			DTextureCube& Texture,
			FTextureCubeCanonicalBuildInput CanonicalInput,
			FTextureCubeBuildProduct Product,
			const FTextureCubeResultApplicationContext& Context
		) -> FTextureBuildOutcome
		{
			CheckGameThread();
			require(Product.PlatformData != nullptr);
			// The provider boundary has already validated these value contracts.
			check((CanonicalInput.DecodedFaces.IsValid() || CanonicalInput.Output == ETextureCubeOutput::HDR)
				&& Product.DerivedDataKey.IsValid());
			check(Product.PlatformData->IsValid());
			auto PlatformData = std::move(Product.PlatformData);
			if (!Context.bPreserveSource)
			{
				auto Source = CanonicalInput.AuthoredPanorama.IsValid()
					? PrepareTextureCubePanoramaSource(CanonicalInput.AuthoredPanorama.GetView(),
						Image::GetRawImageFormatInfo(CanonicalInput.AuthoredPanorama.GetInfo().Format).ChannelCount, 0)
					: PrepareTextureCubeSource(CanonicalInput.DecodedFaces);
				if (!Source) return {ETextureBuildFailure::ApplicationFailed, ETextureBuildStage::Apply,
					"TextureCube source preparation failed; see log for details."};
				Texture.SetSource(std::move(*Source));
			}
			Texture.SetBuildSettings(CanonicalInput.SourceLayout, CanonicalInput.PanoramaFaceDimension,
				CanonicalInput.PanoramaExposureEV, CanonicalInput.OriginalSourceWidth,
				CanonicalInput.OriginalSourceHeight, CanonicalInput.bSRGB, CanonicalInput.Output);
			Texture.SetPlatformData(std::move(PlatformData));
			Texture.UpdateResource();
			if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
			return {ETextureBuildFailure::None};
	}
	}
}
