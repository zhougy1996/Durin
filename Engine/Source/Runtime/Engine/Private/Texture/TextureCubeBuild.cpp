#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/TextureCubeBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
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
		const uint32 BuilderVersion = Module->GetTextureCubeBuilderVersion();
		if (BuilderVersion == 0 || Module->GetTextureCubeProjectionVersion() == 0)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Module, "The TextureCube builder version is invalid."});
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
		auto Source = bHDR
			? PrepareTextureCubePanoramaSource(CanonicalInput.AuthoredPanorama.GetView(), 4, 0)
			: PrepareTextureCubeSource(CanonicalInput.DecodedFaces);
		if (!Source) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput,
			ETextureBuildStage::Normalize, Source.error()});
		auto Product = TexturePrivate::BuildTextureCubeSource(*Source, CanonicalInput.bSRGB,
			CanonicalInput.PanoramaFaceDimension, CanonicalInput.PanoramaExposureEV,
			Request.TargetPlatform, Request.TargetProfile, Request.bPersistDerivedData, &CanonicalInput);
		if (!Product) return std::unexpected(std::move(Product.error()));
		return FTextureCubeBuildValue{std::move(CanonicalInput), std::move(*Product)};
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
#if !DURIN_WITH_EDITORONLY_DATA
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Apply,
				"Texture authoring requires editor-only data."});
#else
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
#endif
	}
	}
}

#endif
