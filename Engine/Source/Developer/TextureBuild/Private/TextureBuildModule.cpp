#include "Modules/ModuleManager.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildService.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin
{
	class FVolumeTextureAuthoringFeature final : public IVolumeTextureAuthoringFeature
	{
	public:
		auto PostLoadUncooked(DVolumeTexture& Texture, std::string& OutError) -> bool override
		{
			const std::string Key = Asset::Build::MakeVolumeTextureDerivedDataKey(
				Texture, OutError);
			if (Key.empty()) return false;
			std::unique_ptr<FVolumeTexturePlatformData> Cached;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
			std::string Message;
			if (Asset::Build::LoadVolumeTextureDerivedData(
				Key, Cached, Status, Message))
				return Texture.PublishDerivedDataLoad(
					std::move(Cached), Key, OutError);
			if (const std::optional<bool> AssetForge =
				TryInvokeVolumeTextureImportRecovery(Texture, OutError))
				return *AssetForge;
			Asset::Build::FVolumeTextureBuildProduct Product;
			if (!Asset::Build::BuildVolumeTexture(Texture.GetSourceData(),
				Texture.GetBuildSettings(), Product, OutError)) return false;
			return Asset::Build::PublishVolumeTextureProduct(
				Texture, std::move(Product), OutError);
		}
	};

	class FTextureBuildModule final : public IModuleInterface
	{
	public:
			auto StartupModule() -> void override
		{
			VolumeTextureRegistration = FModuleStartup::RegisterFeature<
				IVolumeTextureAuthoringFeature>(VolumeTextureAuthoringFeature);
			require(VolumeTextureRegistration.IsValid());
			AssetCompilingCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("Engine.AssetCompilingManager");
			BuildOperations = FModuleStartup::CreateAsyncOperationGroup("TextureBuild.Operations");
			require(BuildOperations.IsValid());
			std::string Error;
			checkf(Asset::Build::InitializeTextureBuildFunctions(
				AssetCompilingCallbackRegistration.GetGate(), &Error),
				"TextureBuild could not register its build functions: {}", Error);
			Asset::Build::FTexture2DBuildCoordinatorConfig Config;
			Config.OwnerCancellationToken = BuildOperations.GetCancellationToken();
			Config.OwnerTaskScope = BuildOperations.GetTaskScope();
			checkf(Asset::Build::InitializeTextureBuildService(
				AssetCompilingCallbackRegistration.GetGate(), Config),
				"TextureBuild could not register its authoring service.");
		}

	private:
		FModuleOwnedCallbackRegistration AssetCompilingCallbackRegistration;
		FAsyncOperationGroup BuildOperations;
		FVolumeTextureAuthoringFeature VolumeTextureAuthoringFeature;
		FModularFeatureRegistration VolumeTextureRegistration;

		auto ShutdownModule() -> void override
		{
			Asset::Build::ShutdownTextureBuildService();
			Asset::Build::ShutdownTextureBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
