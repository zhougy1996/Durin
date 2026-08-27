#include "Modules/ModuleManager.h"
#include "Texture/Texture2DCompilationDomain.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin
{
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

	class FTextureBuildModule final : public IModuleInterface
	{
	public:
			auto StartupModule() -> void override
		{
			VolumeTextureRegistration = FModuleStartup::RegisterFeature<
				IVolumeTexturePostLoadFeature>(VolumeTexturePostLoadFeature);
			require(VolumeTextureRegistration.IsValid());
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
		FVolumeTexturePostLoadFeature VolumeTexturePostLoadFeature;
		FModularFeatureRegistration VolumeTextureRegistration;

		auto ShutdownModule() -> void override
		{
			Asset::Private::ShutdownTexture2DCompilationDomain();
			Asset::ShutdownTextureBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
