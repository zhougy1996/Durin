#include "Misc/Build.h"
#if DURIN_WITH_EDITOR
#include "Asset/ResourceBuildSessions.h"
#endif
#include "Texture/VolumeTexture.h"
#include "Logging/LogMacros.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/VolumeTextureBuild.h"
#include "Texture/ITextureCompressorModule.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataBuild.h"
#include "TexturePlatformSharedOutput.h"
#include "TextureDerivedDataKey.h"
#include "TextureBuildInternal.h"
#include "TexturePlatformCache.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
#if DURIN_WITH_EDITOR

	static auto PrepareVolumeDefinition(const FVolumeTextureBuildRequest& Request)
		-> std::expected<DerivedData::FBuildDefinition, FTextureBuildError>
	{
		const auto Module = ITextureCompressorModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureCompressor module is unavailable."});
		const uint64 BuilderVersion = Module->GetBuildVersion(ETextureBuildKind::VolumeTexture);
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
		return std::move(*Definition);
	}


	static auto ConsumeVolumeBuild(const DerivedData::FBuildCompleteParams& Completion,
		ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError>
	{
		if (Completion.Status == DerivedData::EStatus::Canceled)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "Texture build was cancelled."});
		if (Completion.Status == DerivedData::EStatus::Error)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
				AssetPrivate::GetBuildFailureDiagnostic(Completion, "VolumeTexture derived-data build failed.")});
		}
		auto Product = TexturePrivate::AssembleVolumeTextureSharedOutput(Completion.Output, Platform, Profile);
		if (!Product) return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Build, std::move(Product.error())});
		return std::move(*Product);
	}
#endif

	static auto BuildVolumeTextureWithDiagnostic(const FVolumeTextureBuildRequest& Request)
		-> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module,
			"VolumeTexture authored build orchestration is unavailable outside editor builds."});
#else
		auto Definition = PrepareVolumeDefinition(Request);
		if (!Definition) return std::unexpected(std::move(Definition.error()));
		auto Completion = TexturePrivate::Build(std::move(*Definition),
			TexturePrivate::MakeVolumeTextureInputResolver(Request.Source), Request.bPersistDerivedData ? DerivedData::EBuildPolicy::Default : DerivedData::EBuildPolicy::Build | DerivedData::EBuildPolicy::CacheQuery);
		return ConsumeVolumeBuild(Completion, Request.TargetPlatform, Request.TargetProfile);
#endif
	}

	namespace
	{
		struct FVolumeCacheResult final : FTexturePlatformCacheResult
		{
			std::unique_ptr<FVolumeTexturePlatformData> Data;
			auto Apply(DTexture& Texture) -> void override
			{
				Cast<DVolumeTexture>(&Texture)->SetPlatformData(std::move(Data));
				Texture.UpdateResource();
			}
		};

		struct FVolumeCacheInput final : FTexturePlatformCacheInput
		{
			explicit FVolumeCacheInput(const DVolumeTexture& Texture)
				: Settings(Texture.GetBuildSettings())
			{
				Source = Texture.GetSource().CopyTornOff();
				EstimatedBytes = Source.GetDecodedPayloadSize() * 4;
			}
			FVolumeTextureBuildSettings Settings;
			auto Dispatch(DerivedData::IRequestOwner& Owner, std::function<bool()> ShouldCancel,
				std::function<void(DerivedData::EStatus, std::unique_ptr<FTexturePlatformCacheResult>, std::string)> Completion) const -> void override
			{
#if DURIN_WITH_EDITOR
				if (Owner.IsCanceled() || (ShouldCancel && ShouldCancel()))
				{
					Completion(DerivedData::EStatus::Canceled, {}, {});
					return;
				}
				auto Definition = PrepareVolumeDefinition({.Source = Source, .Settings = Settings});
				if (!Definition) { Completion(DerivedData::EStatus::Error, {}, std::move(Definition.error().Diagnostic)); return; }
				auto Callback = [Completion](DerivedData::FBuildCompleteParams Built) {
					if (Built.Status == DerivedData::EStatus::Canceled)
					{
						Completion(DerivedData::EStatus::Canceled, {}, {});
						return;
					}
					std::unique_ptr<FTexturePlatformCacheResult> Result;
					std::string Diagnostic;
					try
					{
						auto Product = ConsumeVolumeBuild(Built, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
						if (Product)
						{
							auto Volume = std::make_unique<FVolumeCacheResult>();
							Volume->Data = std::move(*Product);
							Result = std::move(Volume);
						}
						else Diagnostic = std::move(Product.error().Diagnostic);
					}
					catch (...) { Diagnostic = "VolumeTexture output assembly failed."; }
					const auto Status = Result ? DerivedData::EStatus::Ok : DerivedData::EStatus::Error;
					Completion(Status, std::move(Result), std::move(Diagnostic));
				};
				TexturePrivate::BuildAsync(std::move(*Definition),
					TexturePrivate::MakeVolumeTextureInputResolver(Source), Owner, std::move(Callback));

#else
				Completion(DerivedData::EStatus::Error, {}, "VolumeTexture authored builds require editor support.");
#endif
			}
		};
	}

	auto BuildVolumeTextureDetached(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTexturePlatformData, FTextureBuildOperationError>
	{
		auto Result = BuildVolumeTextureWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		return std::move(**Result);
	}

	auto DVolumeTexture::CachePlatformData(const FVolumeTextureBuildRequest& Request, const FTextureBuildApplicationContext& Context) -> std::expected<void, FTextureBuildOperationError>
	{
		CheckGameThread();
		if (IsPendingKill())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		BlockOnAnyAsyncBuild();
		if (IsPendingKill())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		auto Result = BuildVolumeTextureWithDiagnostic(Request);
		if (!Result) return std::unexpected(TexturePrivate::ReportBuildFailure(Result.error()));
		// The build boundary has already validated these value contracts.
		check(Request.Source.IsValid());
		check(*Result && (*Result)->IsValid());
		SetSource(Request.Source.CopyTornOff());
		SetBuildSettings(Request.Settings);
		SetPlatformData(std::move(*Result));
		UpdateResource();
		if (Context.bMarkPackageDirty) MarkPackageDirty();
		return {};
	}
}

#endif

namespace Durin
{
	auto DVolumeTexture::BuildPlatformDataForLoad(bool bAsyncCache) -> void
	{
#if DURIN_WITH_EDITORONLY_DATA
		if (bAsyncCache)
		{
			if (!SubmitTexturePlatformCache(*this, std::make_shared<FVolumeCacheInput>(*this)))
				DURIN_ERROR("PostLoad '{}': VolumeTexture cache admission failed.", GetObjectPath());
			return;
		}
		BlockOnAnyAsyncBuild();
		auto Result = BuildVolumeTextureWithDiagnostic({.Source = GetSource().CopyTornOff(), .Settings = BuildSettings});
		if (!Result) { (void)TexturePrivate::ReportBuildFailure(Result.error()); return; }
		SetPlatformData(std::move(*Result));
		UpdateResource();
#else
		DURIN_ERROR("PostLoad '{}': authored VolumeTexture builds require editor-only data support.", GetObjectPath());
#endif
	}
}
