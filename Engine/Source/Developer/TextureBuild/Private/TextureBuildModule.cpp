#include "Modules/ModuleManager.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/Texture2DDerivedData.h"
#include "Texture/Texture2DBuildProvider.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin
{
	class FTexture2DBuildProvider final : public ITexture2DBuildProvider
	{
	public:
		auto GetDescriptor() const -> FTexture2DBuildProviderDescriptor override
		{
			return {
				.ProducerIdentity = "Durin.TextureBuild.Texture2D",
				.SchemaVersion = Texture2DBuilderVersion};
		}

		auto Build(
			FTexture2DBuildRequest Request,
			FTexture2DBuildProduct& OutProduct,
			std::string& OutError,
			const FTexture2DBuildExecutionControl* ExecutionControl) -> bool override
		{
			return BuildTexture2D(
				std::move(Request), OutProduct, OutError, ExecutionControl);
		}
	};

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
			if (!MakeTexture2DDerivedDataKey(Texture, Key, OutError)) return false;
			std::unique_ptr<FTexturePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
			std::string Message;
			if (LoadTexture2DDerivedData(Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), std::move(Key), OutError);
			FTexture2DBuildProduct Product;
			if (!BuildTexture2D({
					.SourceData = Texture.GetImportedData().ToSourceData(),
					.Settings = {
						.Usage = Texture.GetUsage(),
						.CompressionQuality = Texture.GetCompressionQuality(),
						.AlphaMipMode = Texture.GetAlphaMipMode(),
						.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
						.MaxResolution = Texture.GetMaxResolution(),
						.bSRGB = Texture.IsSRGB()}}, Product, OutError)) return false;
			return PublishTexture2DProduct(Texture, std::move(Product), {
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
			const std::string Key = MakeVolumeTextureDerivedDataKey(
				Texture, OutError);
			if (Key.empty()) return false;
			std::unique_ptr<FVolumeTexturePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
			std::string Message;
			if (LoadVolumeTextureDerivedData(
				Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), Key, OutError);
			FVolumeTextureBuildProduct Product;
			if (!BuildVolumeTexture(Texture.GetSourceData(),
				Texture.GetBuildSettings(), Product, OutError)) return false;
			return PublishVolumeTextureProduct(
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
			std::string Key = MakeTextureCubeDerivedDataKey(Texture, OutError);
			if (Key.empty()) return false;
			std::unique_ptr<FTextureCubePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
			std::string Message;
			if (LoadTextureCubeDerivedData(Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), std::move(Key), OutError);
			FTextureCubeBuildProduct Product;
			if (!BuildTextureCubeFaces(
				Texture.GetImportedData().ToSourceData(), {},
				{.bSRGB = Texture.IsSRGB()}, Product, OutError)) return false;
			Product.SourceLayout = Texture.GetSourceLayout();
			Product.SourceWidth = Texture.GetOriginalSourceWidth();
			Product.SourceHeight = Texture.GetOriginalSourceHeight();
			Product.PanoramaFaceDimension = Texture.GetPanoramaFaceDimension();
			Product.PanoramaExposureEV = Texture.GetPanoramaExposureEV();
			return PublishTextureCubeProduct(Texture, std::move(Product), {}, OutError);
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
			Texture2DBuildProviderRegistration = FModuleStartup::RegisterFeature<
				ITexture2DBuildProvider>(Texture2DBuildProvider);
			require(Texture2DRegistration.IsValid());
			require(VolumeTextureRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			require(Texture2DBuildProviderRegistration.IsValid());
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("DerivedDataCache.BuildFunctions");
			std::string Error;
			checkf(InitializeTextureBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"TextureBuild could not register its build functions: {}", Error);
		}

	private:
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;
		FTexture2DPostLoadFeature Texture2DPostLoadFeature;
		FModularFeatureRegistration Texture2DRegistration;
		FTexture2DBuildProvider Texture2DBuildProvider;
		FModularFeatureRegistration Texture2DBuildProviderRegistration;
		FVolumeTexturePostLoadFeature VolumeTexturePostLoadFeature;
		FModularFeatureRegistration VolumeTextureRegistration;
		FTextureCubePostLoadFeature TextureCubePostLoadFeature;
		FModularFeatureRegistration TextureCubeRegistration;

		auto ShutdownModule() -> void override
		{
			ShutdownTextureBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
