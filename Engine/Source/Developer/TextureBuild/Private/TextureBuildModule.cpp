#include "Modules/ModuleManager.h"
#include "Texture/Texture2DBuildProvider.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureDerivedData.h"
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
				.BuilderVersion = Texture2DBuilderVersion};
		}

		auto Build(
			const FTexture2DRecipeBuildRequest& Request,
			const FTexture2DRecipeExecutionControl* ExecutionControl) -> std::expected<FTexture2DRecipeBuildProduct, FTexture2DBuildError> override
		{
			return BuildTexture2D(Request, ExecutionControl);
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
				.BuilderVersion = VolumeTextureBuilderVersion};
		}

		auto Build(const FVolumeTextureRecipeBuildRequest& Request) -> std::expected<FVolumeTextureRecipeBuildProduct, FTextureBuildError> override
		{
			return BuildVolumeTexture(Request);
		}
	};

	class FTextureCubeBuildProvider final : public ITextureCubeBuildProvider
	{
	public:
		auto GetDescriptor() const -> FTextureCubeBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "Durin.TextureBuild.TextureCube",
				.BuilderVersion = TextureCubeBuilderVersion,
				.ProjectionVersion = TextureCubeProjectionVersion};
		}

		auto Normalize(const FTextureCubeBuildRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> override
		{
			return NormalizeTextureCube(Request);
		}

		auto Build(const FTextureCubeRecipeBuildRequest& Request) -> std::expected<FTextureCubeRecipeBuildProduct, FTextureBuildError> override
		{
			return BuildTextureCube(Request);
		}
	};

	class FTextureBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
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
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
