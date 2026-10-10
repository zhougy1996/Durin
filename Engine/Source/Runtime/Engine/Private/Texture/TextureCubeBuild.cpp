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
		auto ApplyTextureCubeBuildResult(DTextureCube& Texture, FTextureCubeCanonicalBuildInput CanonicalInput, std::unique_ptr<FTextureCubePlatformData> Product, const FTextureCubeResultApplicationContext& Context) -> std::expected<void, FTextureBuildError>;
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
		auto Product = TexturePrivate::BuildTextureCubeSource(*Source, CanonicalInput.GetSRGB(),
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
		auto Applied = ApplyTextureCubeBuildResult(Texture, std::move(Result->CanonicalInput), std::move(Result->PlatformData), Context);
		if (!Applied) return std::unexpected(TexturePrivate::ReportBuildFailure(Applied.error()));
		return {};
	}

	namespace
	{
		auto ApplyTextureCubeBuildResult(
			DTextureCube& Texture,
			FTextureCubeCanonicalBuildInput CanonicalInput,
			std::unique_ptr<FTextureCubePlatformData> Product,
			const FTextureCubeResultApplicationContext& Context
		) -> std::expected<void, FTextureBuildError>
		{
			CheckGameThread();
#if !DURIN_WITH_EDITORONLY_DATA
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Apply,
				"Texture authoring requires editor-only data."});
#else
			require(Product != nullptr);
			// The build boundary has already validated these value contracts.
			const auto* LDR = std::get_if<FTextureCubeLDRCanonicalInput>(&CanonicalInput.Pixels);
			check(!LDR || LDR->FaceImages.IsValid());
			check(Product->IsValid());
			auto PlatformData = std::move(Product);
			if (!Context.bPreserveSource)
			{
				auto Source = CanonicalInput.GetAuthoredPanorama().IsValid()
					? PrepareTextureCubePanoramaSource(CanonicalInput.GetAuthoredPanorama().GetView(),
						Image::GetRawImageFormatInfo(CanonicalInput.GetAuthoredPanorama().GetInfo().Format).ChannelCount, 0)
					: PrepareTextureCubeSource(LDR->FaceImages);
				if (!Source) return std::unexpected(FTextureBuildError{ETextureBuildFailure::ApplicationFailed, ETextureBuildStage::Apply,
					Source.error()});
				Texture.SetSource(std::move(*Source));
			}
			Texture.SetBuildSettings(CanonicalInput.GetSourceLayout(), CanonicalInput.PanoramaFaceDimension,
				CanonicalInput.PanoramaExposureEV, CanonicalInput.OriginalSourceWidth,
				CanonicalInput.OriginalSourceHeight, CanonicalInput.GetSRGB(), CanonicalInput.GetOutput());
			Texture.SetPlatformData(std::move(PlatformData));
			Texture.UpdateResource();
			if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
			return {};
#endif
	}
	}
}

#endif
