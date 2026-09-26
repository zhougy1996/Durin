#include "Texture/VolumeTextureBuild.h"
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
		auto Definition = MakeVolumeTextureBuildDefinition({.CanonicalSourceIdentity = Source.GetIdentity(),
			.Width = Source.GetWidth(), .Height = Source.GetHeight(), .Depth = Source.GetDepth(),
			.Settings = Request.Settings, .BuilderVersion = BuilderVersion,
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
		if (!Definition) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
			ETextureBuildStage::Normalize, "Invalid VolumeTexture build definition."});
		auto Resolve = [&](const FTextureSource& Snapshot) -> std::expected<FVolumeTextureSourceData, FTextureBuildError> {
			FVolumeTextureSourceData Prepared{.Width = Snapshot.GetWidth(), .Height = Snapshot.GetHeight(),
				.Depth = Snapshot.GetDepth(), .Format = Request.Settings.OutputFormat};
			const auto Mips = Snapshot.GetMipData();
			if (!Mips.IsValid() || !Prepared.Voxels.UpdatePayload(Mips.GetData()))
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
					ETextureBuildStage::Normalize, "VolumeTexture captured source could not be resolved."});
			Prepared.CanonicalSourceIdentity = Snapshot.GetIdentity();
			return Prepared;
		};
		auto Build = [&](FVolumeTextureSourceData& Prepared) {
			return Module->BuildVolumeTexture({.SourceData = std::cref(Prepared), .Settings = Request.Settings,
				.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
		};
		TexturePrivate::TTexturePlatformBuildAdapter<FVolumeTexturePlatformData, decltype(Resolve), decltype(Build)> Adapter{
			.Source = Source, .Function = Definition->GetFunction(),
			.Inputs = {Definition->GetInputs().begin(), Definition->GetInputs().end()},
			.TargetProfile = Request.TargetProfile, .ResolveSource = Resolve, .BuildProduct = Build};
		DerivedData::TBuildObservations<FTextureBuildError> Observations;
		auto Built = DerivedData::ExecuteBuild(*Definition, Adapter,
			{.bWriteCache = Request.bPersistDerivedData, .MaximumValueBytes = MaximumTexturePayloadBytes}, {}, Observations);
		AssetDerivedDataBuild::ReportCacheIssues(*Definition, Observations, [](const FTextureBuildError& Error) {
			if (Error.ArchiveCause) return std::format("Archive code {} at {}: {}", static_cast<int>(Error.ArchiveCause->Code), Error.ArchiveCause->Path, Error.ArchiveCause->Message);
			return Error.Diagnostic;
		});
		if (!Built) return std::unexpected(std::move(Built.error()));
		return FVolumeTextureBuildProduct{.PlatformData = std::move(*Built),
			.DerivedDataKey = FCacheKeyProxy(Definition->GetKey()),
			.Origin = Observations.Origin == DerivedData::EBuildOrigin::CacheHit
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
