#include "Texture/TextureCubeBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"
#include "TextureBuildDiagnostics.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyTextureCubeBuildResult(DTextureCube& Texture, FTextureCubeCanonicalBuildInput CanonicalInput, FTextureCubeBuildProduct Product, const FTextureCubeResultApplicationContext& Context) -> std::expected<void, FTextureBuildError>;
	}

	auto TexturePrivate::BuildTextureCubeWithDiagnostic(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module, "TextureCube authored build orchestration is unavailable outside editor builds."});
#else
		auto* Module = ITextureBuildModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureBuild module is unavailable."});
		const FTextureCubeBuildDescriptor Descriptor = Module->GetTextureCubeDescriptor();
		if (!Descriptor.IsValid())
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Module, "The TextureCube builder descriptor is invalid."});
		}
		auto Normalized = Module->NormalizeTextureCube({.Input = std::cref(Request.Input),
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
		if (!Normalized)
		{
			return std::unexpected(std::move(Normalized.error()));
		}
		auto CanonicalInput = std::move(*Normalized);
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
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Normalize, "TextureCube normalization returned invalid canonical input."});
		}
		FXxHash128 CanonicalHash = CanonicalInput.SourceIdentity;
		if (bHDR || CanonicalHash.IsZero())
		{
			auto CanonicalSource = bHDR
				? PrepareTextureCubePanoramaSource(CanonicalInput.AuthoredPanorama.GetView(), 4, 0)
				: PrepareTextureCubeSource(CanonicalInput.DecodedFaces);
			if (!CanonicalSource)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Normalize,
					CanonicalSource.error()});
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
		std::string KeyDiagnostic;
		const FCacheKeyProxy Key = BuildTextureCubeDerivedDataKey(KeyInput, KeyDiagnostic);
		if (!Key.IsValid()) return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Normalize, std::move(KeyDiagnostic)});

		TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
		auto PlatformData = std::make_unique<FTextureCubePlatformData>();
		if (TextureDerivedDataCache::Load(
			Key,
			Request.TargetPlatform, Request.TargetProfile,
			*PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
		{
			return FTextureCubeBuildValue{std::move(CanonicalInput), FTextureCubeBuildProduct{
				.PlatformData = std::move(PlatformData), .DerivedDataKey = Key,
				.Builder = Descriptor, .Origin = ETextureCubeBuildProductOrigin::CacheHit}};
		}

		auto Recipe = Module->BuildTextureCube({.DecodedFaces = std::cref(CanonicalInput.DecodedFaces),
			.bSRGB = CanonicalInput.bSRGB, .TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile,
			.HDRPanorama = bHDR ? &CanonicalInput.AuthoredPanorama : nullptr,
			.PanoramaSettings = {.FaceDimension = CanonicalInput.PanoramaFaceDimension,
				.ExposureEV = CanonicalInput.PanoramaExposureEV, .Output = CanonicalInput.Output}});
		if (!Recipe)
		{
			return std::unexpected(std::move(Recipe.error()));
		}
		auto RecipeProduct = std::move(*Recipe);
		if (!RecipeProduct.PlatformData || !RecipeProduct.PlatformData->IsValid())
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Recipe, "TextureCube recipe returned invalid platform data."});
		}
		TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
		if (Request.bPersistDerivedData)
			TextureDerivedDataCache::Store(
				Key,
				Request.TargetPlatform, Request.TargetProfile,
				*RecipeProduct.PlatformData, StoreDiagnostic);
		return FTextureCubeBuildValue{std::move(CanonicalInput), FTextureCubeBuildProduct{
				.PlatformData = std::move(RecipeProduct.PlatformData), .DerivedDataKey = Key,
				.PersistenceDiagnostic = {std::move(CacheDiagnostic), std::move(StoreDiagnostic)},
				.Builder = Descriptor, .Origin = ETextureCubeBuildProductOrigin::Rebuilt}};
#endif
	}

	auto BuildTextureCubeDetached(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildOperationError>
	{
		auto Result = TexturePrivate::BuildTextureCubeWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		return std::move(*Result);
	}

	auto BuildTextureCubeSynchronously(DTextureCube& Texture, const FTextureCubeBuildRequest& Request, const FTextureCubeResultApplicationContext& Context) -> std::expected<void, FTextureBuildOperationError>
	{
		CheckGameThread();
		auto Result = TexturePrivate::BuildTextureCubeWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		auto Applied = ApplyTextureCubeBuildResult(Texture, std::move(Result->CanonicalInput), std::move(Result->Product), Context);
		if (!Applied) return std::unexpected(TexturePrivate::ReportBuildFailure(Applied.error()));
		return {};
	}

	namespace
	{
		auto ApplyTextureCubeBuildResult(
			DTextureCube& Texture,
			FTextureCubeCanonicalBuildInput CanonicalInput,
			FTextureCubeBuildProduct Product,
			const FTextureCubeResultApplicationContext& Context
		) -> std::expected<void, FTextureBuildError>
		{
			CheckGameThread();
			require(Product.PlatformData != nullptr);
			// The build boundary has already validated these value contracts.
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
				if (!Source) return std::unexpected(FTextureBuildError{ETextureBuildFailure::ApplicationFailed, ETextureBuildStage::Apply,
					Source.error()});
				Texture.SetSource(std::move(*Source));
			}
			Texture.SetBuildSettings(CanonicalInput.SourceLayout, CanonicalInput.PanoramaFaceDimension,
				CanonicalInput.PanoramaExposureEV, CanonicalInput.OriginalSourceWidth,
				CanonicalInput.OriginalSourceHeight, CanonicalInput.bSRGB, CanonicalInput.Output);
			Texture.SetPlatformData(std::move(PlatformData));
			Texture.UpdateResource();
			if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
			return {};
	}
	}
}
