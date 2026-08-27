#include "Modules/ModuleManager.h"
#include "Texture/Texture2DCompilationDomain.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin
{
	class FTexture2DPostLoadFeature final : public ITexture2DPostLoadFeature
	{
	public:
		auto PostLoadUncooked(DTexture2D& Texture, std::string& OutError) -> bool override
		{
			if (!Texture.GetImportedData().IsValid())
			{
				OutError = "Texture2D canonical imported data is missing or invalid.";
				return false;
			}
			std::string Key;
			if (!Asset::MakeTexture2DDerivedDataKey(Texture, Key, OutError)) return false;
			std::unique_ptr<FTexturePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
			std::string Message;
			if (Asset::LoadTexture2DDerivedData(Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), std::move(Key), OutError);
			Asset::FTexture2DBuildProduct Product;
			if (!Asset::BuildTexture2D({
					.SourceData = Texture.GetImportedData().ToSourceData(),
					.Settings = {
						.Usage = Texture.GetUsage(),
						.CompressionQuality = Texture.GetCompressionQuality(),
						.AlphaMipMode = Texture.GetAlphaMipMode(),
						.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
						.MaxResolution = Texture.GetMaxResolution(),
						.bSRGB = Texture.IsSRGB()}}, Product, OutError)) return false;
			return Asset::PublishTexture2DProduct(Texture, std::move(Product), {
				.bMarkPackageDirty = false,
				.bReportLoadMutation = false,
				.bSourceDecoderInvoked = false}, OutError);
		}
	};

	class FVolumeTexturePostLoadFeature final : public IVolumeTexturePostLoadFeature
	{
	public:
		auto PostLoadUncooked(DVolumeTexture& Texture, std::string& OutError) -> bool override
		{
			const std::string Key = Asset::MakeVolumeTextureDerivedDataKey(
				Texture, OutError);
			if (Key.empty()) return false;
			std::unique_ptr<FVolumeTexturePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
			std::string Message;
			if (Asset::LoadVolumeTextureDerivedData(
				Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), Key, OutError);
			Asset::FVolumeTextureBuildProduct Product;
			if (!Asset::BuildVolumeTexture(Texture.GetSourceData(),
				Texture.GetBuildSettings(), Product, OutError)) return false;
			return Asset::PublishVolumeTextureProduct(
				Texture, std::move(Product), OutError);
		}
	};

	class FTextureCubePostLoadFeature final : public ITextureCubePostLoadFeature
	{
	public:
		auto PostLoadUncooked(DTextureCube& Texture, std::string& OutError) -> bool override
		{
			if (!Texture.GetImportedData().IsValid())
			{
				OutError = "TextureCube canonical imported data is missing or invalid.";
				return false;
			}
			std::string Key = Asset::MakeTextureCubeDerivedDataKey(Texture, OutError);
			if (Key.empty()) return false;
			std::unique_ptr<FTextureCubePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
			std::string Message;
			if (Asset::LoadTextureCubeDerivedData(Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), std::move(Key), OutError);
			Asset::FTextureCubeBuildProduct Product;
			if (!Asset::BuildTextureCubeFaces(
				Texture.GetImportedData().ToSourceData(), {},
				{.bSRGB = Texture.IsSRGB()}, Product, OutError)) return false;
			Product.SourceLayout = Texture.GetSourceLayout();
			Product.SourceWidth = Texture.GetOriginalSourceWidth();
			Product.SourceHeight = Texture.GetOriginalSourceHeight();
			Product.PanoramaFaceDimension = Texture.GetPanoramaFaceDimension();
			Product.PanoramaExposureEV = Texture.GetPanoramaExposureEV();
			return Asset::PublishTextureCubeProduct(Texture, std::move(Product), {}, OutError);
		}
	};

	class FTextureBuildModule final : public IModuleInterface
	{
	public:
			auto StartupModule() -> void override
		{
			Texture2DRegistration = FModuleStartup::RegisterFeature<
				ITexture2DPostLoadFeature>(Texture2DPostLoadFeature);
			VolumeTextureRegistration = FModuleStartup::RegisterFeature<
				IVolumeTexturePostLoadFeature>(VolumeTexturePostLoadFeature);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<
				ITextureCubePostLoadFeature>(TextureCubePostLoadFeature);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<
				ITextureCubePostLoadFeature>(TextureCubePostLoadFeature);
			require(Texture2DRegistration.IsValid());
			require(VolumeTextureRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			AssetCompilingCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("Engine.AssetCompilingManager");
			BuildOperations = FModuleStartup::CreateAsyncOperationGroup("TextureBuild.Operations");
			require(BuildOperations.IsValid());
			std::string Error;
			checkf(Asset::InitializeTextureBuildFunctions(
				AssetCompilingCallbackRegistration.GetGate(), &Error),
				"TextureBuild could not register its build functions: {}", Error);
			Asset::FTexture2DCompilationDomainConfig Config;
			Config.OwnerCancellationToken = BuildOperations.GetCancellationToken();
			Config.OwnerTaskScope = BuildOperations.GetTaskScope();
			checkf(Asset::Private::InitializeTexture2DCompilationDomain(
				AssetCompilingCallbackRegistration.GetGate(), Config),
				"TextureBuild could not register its compilation domain.");
		}

	private:
		FModuleOwnedCallbackRegistration AssetCompilingCallbackRegistration;
		FAsyncOperationGroup BuildOperations;
		FTexture2DPostLoadFeature Texture2DPostLoadFeature;
		FModularFeatureRegistration Texture2DRegistration;
		FVolumeTexturePostLoadFeature VolumeTexturePostLoadFeature;
		FModularFeatureRegistration VolumeTextureRegistration;
		FTextureCubePostLoadFeature TextureCubePostLoadFeature;
		FModularFeatureRegistration TextureCubeRegistration;

		auto ShutdownModule() -> void override
		{
			Asset::Private::ShutdownTexture2DCompilationDomain();
			Asset::ShutdownTextureBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
