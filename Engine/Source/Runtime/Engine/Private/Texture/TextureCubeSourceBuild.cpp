#include "TextureBuildDiagnostics.h"
#include "TextureCubeBuildFunction.h"
#include "TexturePlatformSharedOutput.h"
#include "Asset/AssetBuildServicePrivate.h"
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
		auto Session = AssetBuildPrivate::CreateSession(MakeTextureCubeInputResolver(Source, PreparedInput));
		if (!Session) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module, Session.error().Description});
		AssetBuildPrivate::FSessionScope Scope{*Session};
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.WriteCache = bPersist;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumTextureSourceBytes + 16;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.MaximumEncodedBytes = MaximumTexturePayloadBytes;
		FCacheKeyProxy Key;
		bool Hit = false;
		Options.Observer.OnAction = [&](const auto& Action) { Key = FCacheKeyProxy(Action.GetKey()); };
		Options.Observer.OnCacheHit = [&] { Hit = true; };
		auto Built = (*Session)->ExecuteInline(std::move(*Definition), std::move(Options));
		if (!Built) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module, Built.error().Description});
		if (Built->Status == DerivedData::EBuildStatus::Cancelled)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "Texture build was cancelled."});
		if (Built->Status != DerivedData::EBuildStatus::Succeeded || !Built->Output)
		{
			const auto* Error = Built->Error ? &*Built->Error : nullptr;
			return std::unexpected(FTextureBuildError{
				Error && Error->Category == DerivedData::EBuildErrorCategory::InvalidInput ? ETextureBuildFailure::InvalidInput : ETextureBuildFailure::InvalidBuilderOutput,
				Error && Error->Phase <= DerivedData::EBuildSessionPhase::Resolve ? ETextureBuildStage::Normalize : ETextureBuildStage::Build,
				Error ? Error->Description : "TextureCube build was cancelled."});
		}
		auto Product = AssembleTextureCubeSharedOutput(*Built->Output, Platform, Profile);
		if (!Product) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Build, std::move(Product.error())});
		return FTextureCubeBuildProduct{.PlatformData = std::move(*Product), .DerivedDataKey = std::move(Key),
			.Origin = Hit ? ETextureCubeBuildProductOrigin::CacheHit : ETextureCubeBuildProductOrigin::Rebuilt};
#endif
	}

}
