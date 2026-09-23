#include "Texture/VolumeTextureBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"
#include "TextureBuildDiagnostics.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ApplyVolumeTextureBuildResult(DVolumeTexture& Texture, const FVolumeTextureSourceData& SourceData, const FVolumeTextureBuildSettings& Settings, FVolumeTextureBuildProduct Product, const FVolumeTextureResultApplicationContext& Context) -> std::expected<void, FTextureBuildError>;
	}

	static auto BuildVolumeTextureWithDiagnostic(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTextureBuildValue, FTextureBuildError>
	{
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable, ETextureBuildStage::Module, "VolumeTexture authored build orchestration is unavailable outside editor builds."});
#else
		const auto Module = ITextureBuildModule::Get();
		if (!Module) return std::unexpected(FTextureBuildError{ETextureBuildFailure::Unavailable,
			ETextureBuildStage::Module, "The TextureBuild module is unavailable."});
		return [&]() -> std::expected<FVolumeTextureBuildValue, FTextureBuildError> {
				const FVolumeTextureBuildDescriptor Descriptor = Module->GetVolumeTextureDescriptor();
				if (!Descriptor.IsValid())
				{
					return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Module, "The VolumeTexture builder descriptor is invalid."});
				}
				const FVolumeTextureSourceData& Source = Request.SourceData.get();
				if (!Source.IsValid() || Source.Format != Request.Settings.OutputFormat
					|| Request.Settings.MipFilter != EVolumeTextureMipFilter::Box)
				{
					return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize, "VolumeTexture source or build settings are invalid or incompatible."});
				}
				const FVolumeTextureBuildKeyInput KeyInput{
					.CanonicalSourceIdentity = Source.GetIdentity(),
					.Width = Source.Width,
					.Height = Source.Height,
					.Depth = Source.Depth,
					.Settings = Request.Settings,
					.BuilderVersion = Descriptor.BuilderVersion,
					.SourcePayloadSchemaVersion = Source.PayloadSchemaVersion,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile};
				std::string KeyDiagnostic;
				const FCacheKeyProxy Key = BuildVolumeTextureDerivedDataKey(KeyInput, KeyDiagnostic);
				if (!Key.IsValid()) return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Module, std::move(KeyDiagnostic)});

				TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
				auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
				if (TextureDerivedDataCache::Load(
					Key,
					Request.TargetPlatform, Request.TargetProfile,
					*PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
				{
					return FVolumeTextureBuildValue{FVolumeTextureBuildProduct{
						.PlatformData = std::move(PlatformData), .DerivedDataKey = Key,
						.Builder = Descriptor, .Origin = EVolumeTextureBuildProductOrigin::CacheHit}};
				}

			auto Recipe = Module->BuildVolumeTexture({.SourceData = std::cref(Source), .Settings = Request.Settings, .TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
				if (!Recipe)
				{
					return std::unexpected(std::move(Recipe.error()));
				}
				auto RecipeProduct = std::move(*Recipe);
				if (!RecipeProduct.PlatformData || !RecipeProduct.PlatformData->IsValid())
				{
					return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidBuilderOutput, ETextureBuildStage::Recipe, "VolumeTexture recipe returned invalid platform data."});
				}
				TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
				if (Request.bPersistDerivedData)
					TextureDerivedDataCache::Store(
						Key,
						Request.TargetPlatform, Request.TargetProfile,
						*RecipeProduct.PlatformData, StoreDiagnostic);
				return FVolumeTextureBuildValue{FVolumeTextureBuildProduct{
						.PlatformData = std::move(RecipeProduct.PlatformData), .DerivedDataKey = Key,
						.PersistenceDiagnostic = {std::move(CacheDiagnostic), std::move(StoreDiagnostic)},
						.Builder = Descriptor, .Origin = EVolumeTextureBuildProductOrigin::Rebuilt}};
			}();
#endif
	}

	auto BuildVolumeTextureDetached(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTextureBuildValue, FTextureBuildOperationError>
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
		auto Applied = ApplyVolumeTextureBuildResult(Texture, Request.SourceData.get(), Request.Settings, std::move(Result->Product), Context);
		if (!Applied) return std::unexpected(TexturePrivate::ReportBuildFailure(Applied.error()));
		return {};
	}

	namespace
	{
		auto ApplyVolumeTextureBuildResult(
			DVolumeTexture& Texture,
			const FVolumeTextureSourceData& SourceData,
			const FVolumeTextureBuildSettings& Settings,
			FVolumeTextureBuildProduct Product,
			const FVolumeTextureResultApplicationContext& Context
		) -> std::expected<void, FTextureBuildError>
		{
			CheckGameThread();
			require(Product.PlatformData != nullptr);
			// The build boundary has already validated these value contracts.
			check(SourceData.IsValid() && SourceData.Format == Settings.OutputFormat && Product.DerivedDataKey.IsValid());
			check(Product.PlatformData->IsValid());
			if (!Context.bPreserveSource)
			{
				auto Source = PrepareVolumeTextureSource(SourceData);
				if (!Source) return std::unexpected(FTextureBuildError{ETextureBuildFailure::ApplicationFailed, ETextureBuildStage::Apply,
					Source.error()});
				Texture.SetSource(std::move(*Source));
			}
			Texture.SetBuildSettings(Settings);
			Texture.SetPlatformData(std::move(Product.PlatformData));
			Texture.UpdateResource();
			if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
			return {};
	}
	}
}
