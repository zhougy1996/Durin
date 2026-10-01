#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/VolumeTextureBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
#include "VolumeTextureBuildFunction.h"
#include "TexturePlatformSharedOutput.h"
#include "TextureBuildSession.h"
#include "TextureDerivedDataKey.h"
#include "TextureBuildDiagnostics.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyVolumeTextureBuildResult(DVolumeTexture& Texture, const FTextureSource& Source, const FVolumeTextureBuildSettings& Settings, FVolumeTextureBuildProduct Product, const FVolumeTextureResultApplicationContext& Context) -> std::expected<void, FTextureBuildError>;
	}

	static auto BuildVolumeTextureWithDiagnostic(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTextureBuildProduct, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module, "VolumeTexture authored build orchestration is unavailable outside editor builds."});
#else
		const auto Module = ITextureBuildModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureBuild module is unavailable."});
		const uint32 BuilderVersion = Module->GetVolumeTextureBuilderVersion();
		if (BuilderVersion == 0)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Module, "The VolumeTexture builder version is invalid."});
		}
		const auto& Source = Request.Source;
		const auto ExpectedFormat = [&] {
			switch (Request.Settings.OutputFormat)
			{
			case EVolumeTextureFormat::R8_UNORM: return ETextureSourceFormat::R8_UNORM;
			case EVolumeTextureFormat::RG8_UNORM: return ETextureSourceFormat::RG8_UNORM;
			case EVolumeTextureFormat::RGBA8_UNORM: return ETextureSourceFormat::RGBA8;
			case EVolumeTextureFormat::R16_FLOAT: return ETextureSourceFormat::R16_FLOAT;
			case EVolumeTextureFormat::RGBA16_FLOAT: return ETextureSourceFormat::RGBA16_FLOAT;
			}
			return ETextureSourceFormat::Invalid;
		}();
		if (!Source.IsValid() || Source.GetOwner() || Source.GetKind() != ETextureSourceKind::Volume
			|| Source.GetFormat() != ExpectedFormat || Request.Settings.MipFilter != EVolumeTextureMipFilter::Box)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
				ETextureBuildStage::Normalize, "VolumeTexture source or settings are invalid."});
		auto Definition = TexturePrivate::MakeVolumeTextureSessionDefinition(Request);
		if (!Definition) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
			ETextureBuildStage::Normalize, "Invalid VolumeTexture build definition."});
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = Request.bPersistDerivedData;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumTextureSourceBytes + 16;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.MaximumEncodedBytes = MaximumTexturePayloadBytes;
		auto Built = TexturePrivate::Build(std::move(*Definition),
			TexturePrivate::MakeVolumeTextureInputResolver(Source), std::move(Options));
		if (!Built) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "VolumeTexture build session is unavailable."});
		auto& Completion = *Built;
		if (Completion.GetStatus() == DerivedData::EStatus::Canceled)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "Texture build was cancelled."});
		if (Completion.GetStatus() == DerivedData::EStatus::Error)
		{
			std::string Description = "VolumeTexture derived-data build failed.";
			if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
			return std::unexpected(FTextureBuildError{Completion.GetOutput() ? ETextureBuildFailure::InvalidBuilderOutput : ETextureBuildFailure::Unavailable,
				Completion.GetOutput() ? ETextureBuildStage::Build : ETextureBuildStage::Module, std::move(Description)});
		}
		auto Product = TexturePrivate::AssembleVolumeTextureSharedOutput(*Completion.GetOutput(), Request.TargetPlatform, Request.TargetProfile);
		if (!Product) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Build, std::move(Product.error())});
		return FVolumeTextureBuildProduct{.PlatformData = std::move(*Product), .DerivedDataKey = FCacheKeyProxy(*Completion.GetCacheKey()),
			.Origin = DerivedData::HasBuildStatus(Completion.GetBuildStatus(), DerivedData::EBuildStatus::CacheQueryHit)
				? EVolumeTextureBuildProductOrigin::CacheHit : EVolumeTextureBuildProductOrigin::Rebuilt};
#endif
	}

	auto BuildVolumeTextureDetached(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTextureBuildProduct, FTextureBuildOperationError>
	{
		auto Result = BuildVolumeTextureWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		return std::move(*Result);
	}

	auto BuildVolumeTextureSynchronously(DVolumeTexture& Texture, const FVolumeTextureBuildRequest& Request, const FVolumeTextureResultApplicationContext& Context) -> std::expected<void, FTextureBuildOperationError>
	{
		CheckGameThread();
		auto Result = BuildVolumeTextureWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		auto Applied = ApplyVolumeTextureBuildResult(Texture, Request.Source, Request.Settings, std::move(*Result), Context);
		if (!Applied) return std::unexpected(TexturePrivate::ReportBuildFailure(Applied.error()));
		return {};
	}

	namespace
	{
		auto ApplyVolumeTextureBuildResult(
			DVolumeTexture& Texture,
			const FTextureSource& Source,
			const FVolumeTextureBuildSettings& Settings,
			FVolumeTextureBuildProduct Product,
			const FVolumeTextureResultApplicationContext& Context
		) -> std::expected<void, FTextureBuildError>
		{
			CheckGameThread();
			require(Product.PlatformData != nullptr);
			// The build boundary has already validated these value contracts.
			check(Source.IsValid() && Product.DerivedDataKey.IsValid());
			check(Product.PlatformData->IsValid());
			if (!Context.bPreserveSource)
			{
				Texture.SetSource(Source.CopyTornOff());
			}
			Texture.SetBuildSettings(Settings);
			Texture.SetPlatformData(std::move(Product.PlatformData));
			Texture.UpdateResource();
			if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
			return {};

	}
	}
}

#endif
