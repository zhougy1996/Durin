#include "Modules/ModuleManager.h"
#include "Texture/Texture2DDerivedData.h"
#include "Texture/Texture2DBuildProvider.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTextureBuildOperations.h"

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
			const FTexture2DBuildRequest& Request,
			FTexture2DBuildProduct& OutProduct,
			std::string& OutError,
			const FTexture2DBuildExecutionControl* ExecutionControl) -> bool override
		{
			return BuildTexture2D(Request, OutProduct, OutError, ExecutionControl);
		}
	};

	class FVolumeTextureBuildProvider final : public IVolumeTextureBuildProvider
	{
	public:
		auto GetDescriptor() const
			-> FVolumeTextureBuildProviderDescriptor override
		{
			return {
				.ProducerIdentity = "Durin.TextureBuild.VolumeTexture",
				.SchemaVersion = VolumeTextureBuilderVersion};
		}

		auto Build(
			const FVolumeTextureBuildRequest& Request,
			FVolumeTextureBuildProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			if (Request.TargetPlatform != ECookTargetPlatform::Win64
				|| Request.TargetProfile != ECookTargetProfile::Game)
			{
				OutError = "VolumeTexture build target is unsupported.";
				return false;
			}
			return BuildVolumeTexture(
				Request.SourceData.get(), Request.Settings, OutProduct, OutError,
				Request.bPersistDerivedData);
		}
	};

	class FTextureCubeBuildProvider final : public ITextureCubeBuildProvider
	{
	public:
		auto GetDescriptor() const -> FTextureCubeBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "Durin.TextureBuild.TextureCube",
				.SchemaVersion = TextureCubeBuilderVersion};
		}

		auto Build(const FTextureCubeBuildRequest& Request,
			FTextureCubeCanonicalBuildInput& OutCanonicalInput,
			FTextureCubeBuildProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			return BuildTextureCube(Request, OutCanonicalInput, OutProduct, OutError);
		}
	};

	class FTextureBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration(
					"DerivedDataCache.BuildFunctions");
			std::string Error;
			requiref(InitializeTextureBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"TextureBuild could not register its build functions: {}", Error);
			Texture2DBuildProviderRegistration = FModuleStartup::RegisterFeature<
				ITexture2DBuildProvider>(Texture2DBuildProvider);
			TextureCubeBuildProviderRegistration = FModuleStartup::RegisterFeature<
				ITextureCubeBuildProvider>(TextureCubeBuildProvider);
			VolumeTextureBuildProviderRegistration = FModuleStartup::RegisterFeature<
				IVolumeTextureBuildProvider>(VolumeTextureBuildProvider);
			require(Texture2DBuildProviderRegistration.IsValid());
			require(TextureCubeBuildProviderRegistration.IsValid());
			require(VolumeTextureBuildProviderRegistration.IsValid());
		}

	private:
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;
		FTexture2DBuildProvider Texture2DBuildProvider;
		FModularFeatureRegistration Texture2DBuildProviderRegistration;
		FVolumeTextureBuildProvider VolumeTextureBuildProvider;
		FModularFeatureRegistration VolumeTextureBuildProviderRegistration;
		FTextureCubeBuildProvider TextureCubeBuildProvider;
		FModularFeatureRegistration TextureCubeBuildProviderRegistration;

		auto ShutdownModule() -> void override
		{
			VolumeTextureBuildProviderRegistration.Reset();
			TextureCubeBuildProviderRegistration.Reset();
			Texture2DBuildProviderRegistration.Reset();
			ShutdownTextureBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
