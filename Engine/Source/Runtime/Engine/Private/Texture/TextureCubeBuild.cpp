#include "Texture/TextureCubeBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
#include "TexturePlatformBuild.h"
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

	auto TexturePrivate::BuildTextureCubeSource(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, ECookTargetPlatform Platform, ECookTargetProfile Profile, bool bPersist,
		const FTextureCubeCanonicalBuildInput* PreparedInput)
		-> std::expected<FTextureCubeBuildProduct, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "TextureCube authoring is unavailable."});
#else
		auto* Module = ITextureBuildModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureBuild module is unavailable."});
		const bool bHDR = Source.GetKind() == ETextureSourceKind::LongLatCube;
		if (!Source.IsValid() || Source.GetOwner()
			|| (!bHDR && (Source.GetKind() != ETextureSourceKind::TextureCube || Source.GetFormat() != ETextureSourceFormat::RGBA8))
			|| (bHDR && (Source.GetFormat() != ETextureSourceFormat::RGBA32_FLOAT || bSRGB
				|| Source.GetSourceChannelCount() != 4 || Source.HasTransparency()))
			|| !Module->GetTextureCubeProjectionVersion())
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
				ETextureBuildStage::Normalize, "TextureCube source is not canonical."});
		const auto Hash = Source.GetIdentity();
		auto Definition = MakeTextureCubeBuildDefinition({
			.SourceLayout = bHDR ? ETextureCubeBuildSourceLayout::EquirectangularPanorama : ETextureCubeBuildSourceLayout::SixFaces,
			.CanonicalSourceIdentity = Hash,
			.FaceDimension = bHDR ? FaceDimension : 0, .ExposureEV = bHDR && Exposure != 0.0f ? Exposure : 0.0f,
			.bSRGB = bSRGB, .BuilderVersion = Module->GetTextureCubeBuilderVersion(),
			.ProjectionVersion = Module->GetTextureCubeProjectionVersion(), .TargetPlatform = Platform, .TargetProfile = Profile});
		if (!Definition) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
			ETextureBuildStage::Normalize, "Invalid TextureCube build definition."});
		struct FPrepared { FTextureCubeDecodedFaces Faces; Image::FImage Panorama; };
		auto Resolve = [bHDR, PreparedInput](const FTextureSource& Snapshot) -> std::expected<FPrepared, FTextureBuildError> {
			FPrepared Result;
			// Import normalization already owns immutable image buffers; reuse their views.
			if (PreparedInput) return FPrepared{PreparedInput->DecodedFaces, PreparedInput->AuthoredPanorama};
			if (bHDR)
			{
				const auto Mips = Snapshot.GetMipData();
				const auto View = Mips.GetMipImage(0, 0, 0);
				if (View.IsValid())
				{
					auto Image = Image::FImage::TryCreate(View.GetInfo(), Mips.GetMipData(0, 0, 0));
					if (Image) Result.Panorama = std::move(*Image);
				}
			}
			else Result.Faces = ReadTextureCubeFaces(Snapshot);
			if (bHDR ? !Result.Panorama.IsValid() : !Result.Faces.IsValid())
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
					ETextureBuildStage::Normalize, "TextureCube captured source could not be resolved."});
			return Result;
		};
		auto Build = [&](FPrepared& Prepared) {
			return Module->BuildTextureCube({.DecodedFaces = std::cref(Prepared.Faces),
				.bSRGB = bSRGB, .TargetPlatform = Platform, .TargetProfile = Profile,
				.HDRPanorama = bHDR ? &Prepared.Panorama : nullptr,
				.PanoramaSettings = {.FaceDimension = FaceDimension, .ExposureEV = Exposure,
					.Output = bHDR ? ETextureCubeOutput::HDR : ETextureCubeOutput::LDR}});
		};
		TTexturePlatformBuildAdapter<FTextureCubePlatformData, decltype(Resolve), decltype(Build)> Adapter{
			.Source = Source, .Function = Definition->GetFunction(),
			.Inputs = {Definition->GetInputs().begin(), Definition->GetInputs().end()},
			.TargetProfile = Profile, .ResolveSource = Resolve, .BuildProduct = Build};
		DerivedData::TBuildObservations<FTextureBuildError> Observations;
		auto Built = DerivedData::ExecuteBuild(*Definition, Adapter,
			{.bWriteCache = bPersist, .MaximumValueBytes = MaximumTexturePayloadBytes}, {}, Observations);
		AssetDerivedDataBuild::ReportCacheIssues(*Definition, Observations, [](const FTextureBuildError& Error) {
			if (Error.ArchiveCause) return std::format("Archive code {} at {}: {}", static_cast<int>(Error.ArchiveCause->Code), Error.ArchiveCause->Path, Error.ArchiveCause->Message);
			return Error.Diagnostic;
		});
		if (!Built) return std::unexpected(std::move(Built.error()));
		return FTextureCubeBuildProduct{.PlatformData = std::move(*Built),
			.DerivedDataKey = FCacheKeyProxy(Definition->GetKey()),
			.Origin = Observations.Origin == DerivedData::EBuildOrigin::CacheHit
				? ETextureCubeBuildProductOrigin::CacheHit : ETextureCubeBuildProductOrigin::Rebuilt};
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
