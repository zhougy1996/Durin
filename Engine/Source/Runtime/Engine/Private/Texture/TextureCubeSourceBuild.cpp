#include "TextureBuildDiagnostics.h"
#include "TextureCubeBuildFunction.h"
#include "TexturePlatformSharedOutput.h"
#include "TextureBuildSession.h"
#include "TextureDerivedDataKey.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
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
		auto Definition = MakeTextureCubeSessionDefinition({
			.SourceLayout = bHDR ? ETextureCubeBuildSourceLayout::EquirectangularPanorama : ETextureCubeBuildSourceLayout::SixFaces,
			.CanonicalSourceIdentity = Hash,
			.FaceDimension = bHDR ? FaceDimension : 0, .ExposureEV = bHDR && Exposure != 0.0f ? Exposure : 0.0f,
			.bSRGB = bSRGB, .BuilderVersion = Module->GetTextureCubeBuilderVersion(),
			.ProjectionVersion = Module->GetTextureCubeProjectionVersion(), .TargetPlatform = Platform, .TargetProfile = Profile});
		if (!Definition) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
			ETextureBuildStage::Normalize, "Invalid TextureCube build definition."});
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = bPersist;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumTextureSourceBytes + 16;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.MaximumEncodedBytes = MaximumTexturePayloadBytes;
		auto Built = TexturePrivate::Build(std::move(*Definition),
			MakeTextureCubeInputResolver(Source, PreparedInput), std::move(Options));
		if (!Built) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "TextureCube build session is unavailable."});
		auto& Completion = *Built;
		if (Completion.GetStatus() == DerivedData::EStatus::Canceled)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "Texture build was cancelled."});
		if (Completion.GetStatus() == DerivedData::EStatus::Error)
		{
			std::string Description = "TextureCube derived-data build failed.";
			if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
			return std::unexpected(FTextureBuildError{Completion.GetOutput() ? ETextureBuildFailure::InvalidBuilderOutput : ETextureBuildFailure::Unavailable,
				Completion.GetOutput() ? ETextureBuildStage::Build : ETextureBuildStage::Module, std::move(Description)});
		}
		auto Product = AssembleTextureCubeSharedOutput(*Completion.GetOutput(), Platform, Profile);
		if (!Product) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Build, std::move(Product.error())});
		return FTextureCubeBuildProduct{.PlatformData = std::move(*Product), .DerivedDataKey = FCacheKeyProxy(*Completion.GetCacheKey()),
			.Origin = DerivedData::HasBuildStatus(Completion.GetBuildStatus(), DerivedData::EBuildStatus::CacheQueryHit)
				? ETextureCubeBuildProductOrigin::CacheHit : ETextureCubeBuildProductOrigin::Rebuilt};
#endif
	}

}
