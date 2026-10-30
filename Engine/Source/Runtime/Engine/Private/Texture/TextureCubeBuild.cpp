#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/TextureCubeBuild.h"
#include "Texture/ITextureCompressorModule.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataKey.h"
#include "TextureBuildInternal.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto TexturePrivate::PrepareTextureCubeBuild(const FTextureCubeBuildRequest& Request)
		-> std::expected<FPreparedTextureCubeBuild, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module, "TextureCube authored build orchestration is unavailable outside editor builds."});
#else
		auto* Module = ITextureCompressorModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureCompressor module is unavailable."});
		const uint64 BuilderVersion = Module->GetBuildVersion(ETextureBuildKind::TextureCube);
		if (BuilderVersion == 0)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Module, "The TextureCube builder version is invalid."});
		}
		auto Normalized = Module->NormalizeTextureCube({.Input = std::cref(Request.Input),
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
		if (!Normalized)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				FormatTextureBuildOperationError(Normalized.error())});
		}
		auto CanonicalInput = std::move(*Normalized);
		const bool bHDR = CanonicalInput.GetOutput() == ETextureCubeOutput::HDR;
		const auto* LDR = std::get_if<FTextureCubeLDRCanonicalInput>(&CanonicalInput.Pixels);
		if ((LDR && !LDR->FaceImages.IsValid())
			|| (bHDR && (!CanonicalInput.GetAuthoredPanorama().IsValid()
				|| CanonicalInput.GetAuthoredPanorama().GetInfo().Format != Image::ERawImageFormat::RGBA32F))
			|| (CanonicalInput.GetSourceLayout() != ETextureCubeSourceLayout::SixFaces
				&& CanonicalInput.GetSourceLayout() != ETextureCubeSourceLayout::EquirectangularPanorama)
			|| !std::isfinite(CanonicalInput.PanoramaExposureEV)
			|| CanonicalInput.PanoramaExposureEV < MinimumTextureCubePanoramaExposureEV
			|| CanonicalInput.PanoramaExposureEV > MaximumTextureCubePanoramaExposureEV
			|| CanonicalInput.OriginalSourceWidth == 0 || CanonicalInput.OriginalSourceHeight == 0)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Normalize, "TextureCube normalization returned invalid canonical input."});
		}
		auto Source = bHDR
			? PrepareTextureCubePanoramaSource(CanonicalInput.GetAuthoredPanorama().GetView(), 4, 0)
			: PrepareTextureCubeSource(LDR->FaceImages);
		if (!Source) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput,
			ETextureBuildStage::Normalize, Source.error()});
		return FPreparedTextureCubeBuild{std::move(CanonicalInput), std::move(*Source)};
#endif
	}

	auto TexturePrivate::BuildTextureCubeWithDiagnostic(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildError>
	{
		auto Prepared = PrepareTextureCubeBuild(Request);
		if (!Prepared) return std::unexpected(std::move(Prepared.error()));
		auto& CanonicalInput = Prepared->CanonicalInput;
		auto Product = BuildTextureCubeSource(Prepared->Source, CanonicalInput.GetSRGB(),
			CanonicalInput.PanoramaFaceDimension, CanonicalInput.PanoramaExposureEV,
			Request.TargetPlatform, Request.TargetProfile, Request.bPersistDerivedData, &CanonicalInput);
		if (!Product) return std::unexpected(std::move(Product.error()));
		return FTextureCubeBuildValue{std::move(CanonicalInput), std::move(**Product)};
	}

	auto BuildTextureCubeDetached(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildOperationError>
	{
		auto Result = TexturePrivate::BuildTextureCubeWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		return std::move(*Result);
	}

	auto DTextureCube::CachePlatformData(const FTextureCubeBuildRequest& Request, const FTextureBuildApplicationContext& Context) -> std::expected<void, FTextureBuildOperationError>
	{
		CheckGameThread();
		if (IsPendingKill())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		BlockOnAnyAsyncBuild();
		if (IsPendingKill())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		auto Prepared = TexturePrivate::PrepareTextureCubeBuild(Request);
		if (!Prepared) return std::unexpected(TexturePrivate::ReportBuildFailure(Prepared.error()));
		const auto& CanonicalInput = Prepared->CanonicalInput;
		auto Product = TexturePrivate::BuildTextureCubeSource(Prepared->Source, CanonicalInput.GetSRGB(),
			CanonicalInput.PanoramaFaceDimension, CanonicalInput.PanoramaExposureEV,
			Request.TargetPlatform, Request.TargetProfile, Request.bPersistDerivedData, &CanonicalInput);
		if (!Product) return std::unexpected(TexturePrivate::ReportBuildFailure(Product.error()));
		SetSource(std::move(Prepared->Source));
		SetBuildSettings(CanonicalInput.GetSourceLayout(), CanonicalInput.PanoramaFaceDimension,
			CanonicalInput.PanoramaExposureEV, CanonicalInput.OriginalSourceWidth,
			CanonicalInput.OriginalSourceHeight, CanonicalInput.GetSRGB(), CanonicalInput.GetOutput());
		SetPlatformData(std::move(*Product));
		UpdateResource();
		if (Context.bMarkPackageDirty) MarkPackageDirty();
		return {};
	}
}

#endif
