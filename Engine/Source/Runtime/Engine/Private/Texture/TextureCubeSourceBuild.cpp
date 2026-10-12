#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "TextureBuildDiagnostics.h"
#include "TextureCubeBuildFunction.h"
#include "TexturePlatformSharedOutput.h"
#include "TextureBuildSession.h"
#include "TextureDerivedDataKey.h"
#include "Texture/TextureDerivedData.h"
#include "TextureCubeBuildAsync.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	static auto PrepareCubeDefinition(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<DerivedData::FBuildDefinition, FTextureBuildError>
	{
		auto* Module = ITextureCompressorModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureCompressor module is unavailable."});
		const bool bHDR = Source.GetKind() == ETextureSourceKind::LongLatCube;
		if (!Source.IsValid() || Source.GetOwner()
			|| (!bHDR && (Source.GetKind() != ETextureSourceKind::TextureCube || Source.GetFormat() != ETextureSourceFormat::RGBA8))
			|| (bHDR && (Source.GetFormat() != ETextureSourceFormat::RGBA32_FLOAT || bSRGB
				|| Source.GetSourceChannelCount() != 4 || Source.HasTransparency()))
			|| !Module->GetBuildVersion(ETextureBuildKind::TextureCube))
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
				ETextureBuildStage::Normalize, "TextureCube source is not canonical."});
		const auto Hash = Source.GetIdentity();
		auto Definition = TexturePrivate::MakeTextureCubeSessionDefinition({
			.SourceLayout = bHDR ? ETextureCubeBuildSourceLayout::EquirectangularPanorama : ETextureCubeBuildSourceLayout::SixFaces,
			.CanonicalSourceIdentity = Hash,
			.FaceDimension = bHDR ? FaceDimension : 0, .ExposureEV = bHDR && Exposure != 0.0f ? Exposure : 0.0f,
			.bSRGB = bSRGB, .BuilderVersion = Module->GetBuildVersion(ETextureBuildKind::TextureCube),
			.TargetPlatform = Platform, .TargetProfile = Profile});
		if (!Definition) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
			ETextureBuildStage::Normalize, "Invalid TextureCube build definition."});
		return std::move(*Definition);
	}
	static auto CubeBuildOptions(bool bPersist) -> DerivedData::FBuildRequestOptions
	{
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = bPersist;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumTextureSourceBytes + 16;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.MaximumEncodedBytes = MaximumTexturePayloadBytes;
		return Options;
	}
	static auto ConsumeCubeBuild(const DerivedData::FBuildCompleteParams& Completion,
		ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError>
	{
		if (Completion.GetStatus() == DerivedData::EStatus::Canceled)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "Texture build was cancelled."});
		if (Completion.GetStatus() == DerivedData::EStatus::Error)
		{
			std::string Description = "TextureCube derived-data build failed.";
			if (!Completion.GetDiagnostic().empty()) Description = Completion.GetDiagnostic();
			if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
			return std::unexpected(FTextureBuildError{Completion.GetOutput() ? ETextureBuildFailure::InvalidBuilderOutput : ETextureBuildFailure::Unavailable,
				Completion.GetOutput() ? ETextureBuildStage::Build : ETextureBuildStage::Module, std::move(Description)});
		}
		if (!Completion.GetOutput()) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput,
			ETextureBuildStage::Build, "TextureCube build returned no output."});
		auto Product = TexturePrivate::AssembleTextureCubeSharedOutput(*Completion.GetOutput(), Platform, Profile);
		if (!Product) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Build, std::move(Product.error())});
		return std::move(*Product);
	}
#endif

#if DURIN_WITH_EDITOR
	auto TexturePrivate::BuildTextureCubeSourceAsync(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, DerivedData::IRequestOwner& Owner,
		std::function<bool()> ShouldCancel, FCubeSourceCompletion Completion,
		const FTextureCubeCanonicalBuildInput* PreparedInput) -> void
	{
		if (Owner.IsCanceled() || (ShouldCancel && ShouldCancel()))
		{
			Completion(std::unexpected(FTextureBuildError{ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "Texture build was cancelled."}));
			return;
		}
		auto Definition = PrepareCubeDefinition(Source, bSRGB, FaceDimension, Exposure,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		if (!Definition) { Completion(std::unexpected(std::move(Definition.error()))); return; }
		auto Options = CubeBuildOptions(true);
		Options.Cancellation = DerivedData::FBuildCancellation(std::move(ShouldCancel));
		auto Callback = [Completion](DerivedData::FBuildCompleteParams Result) mutable {
			auto Product = [&]() -> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError> {
				try { return ConsumeCubeBuild(Result, ECookTargetPlatform::Win64, ECookTargetProfile::Game); }
				catch (...) { return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput,
					ETextureBuildStage::Build, "TextureCube output assembly failed."}); }
			}();
			Completion(std::move(Product));
		};
		auto Admitted = BuildAsync(std::move(*Definition), MakeTextureCubeInputResolver(Source, PreparedInput), Owner,
			std::move(Callback), std::move(Options));
		if (!Admitted)
			Completion(std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
				ETextureBuildStage::Module, Admitted.error().Description}));
	}
#endif

	auto TexturePrivate::BuildTextureCubeSource(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, ECookTargetPlatform Platform, ECookTargetProfile Profile, bool bPersist,
		const FTextureCubeCanonicalBuildInput* PreparedInput)
		-> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "TextureCube authoring is unavailable."});
#else
		auto Definition = PrepareCubeDefinition(Source, bSRGB, FaceDimension, Exposure, Platform, Profile);
		if (!Definition) return std::unexpected(std::move(Definition.error()));
		auto Options = CubeBuildOptions(bPersist);
		auto Built = TexturePrivate::Build(std::move(*Definition),
			MakeTextureCubeInputResolver(Source, PreparedInput), std::move(Options));
		if (!Built) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, Built.error().Description});
		auto& Completion = *Built;
		return ConsumeCubeBuild(Completion, Platform, Profile);
#endif
	}

}

#endif
