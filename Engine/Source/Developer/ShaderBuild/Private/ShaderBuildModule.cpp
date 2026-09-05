#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Shader/ShaderBuildProvider.h"
#include "Shader/ShaderData.h"
#include "ShaderBuild/ShaderBuildLifecycle.h"
#include "ShaderBuild/ShaderPaths.h"
#include "ShaderCompileService.h"
#include "ShaderLibraryProducer.h"

namespace Durin
{
	class FShaderBuildProvider final : public IShaderBuildProvider
	{
	public:
		auto CompileMounted(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options) -> FShaderCompilerOutput override
		{
			return GetOrCompileShader(VirtualShaderPath, Options);
		}

		auto CompileGenerated(const FGeneratedShaderCompileRequest& Request)
			-> FShaderCompilerOutput override
		{
			return GetOrCompileGeneratedShader(Request);
		}

		auto GetCompilerEnvironmentIdentity() -> std::string override
		{
			return GetShaderCompilerEnvironmentIdentityFromService();
		}

		auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
			std::string& OutError) -> bool override
		{
			return BuildShaderSourceDependencyManifestFromService(
				VirtualShaderPath, Options, OutDependencies, OutError);
		}

		auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint,
			std::string& OutError) -> bool override
		{
			return BuildShaderSourceTreeFingerprintFromService(
				VirtualShaderPath, Options, OutFingerprint, OutError);
		}

		auto GetStats() const -> FShaderBuildStats override
		{
			return GetShaderCompileServiceStats();
		}

		auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::string& OutError) -> bool override
		{
			return ProduceCookedShaderLibrary(
				TargetPlatform, TargetProfile, OutBytes, OutError);
		}
	};

	namespace
	{
		FShaderBuildProvider Provider;
		FModularFeatureRegistration ProviderRegistration;
		std::unique_ptr<FModuleTestOwner> TestOwner;

		auto InitializeShaderBuildServices() -> void
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompileService();
			std::string Error;
			requiref(InitializeShaderData(
				FShaderDataConfiguration::Authored(), Error),
				"Authored Shader data initialization failed: {}", Error);
		}
	}

	auto InitializeShaderBuild() -> void
	{
		requiref(!ProviderRegistration.IsValid(),
			"ShaderBuild is already initialized");
		ProviderRegistration =
			FModuleStartup::RegisterFeature<IShaderBuildProvider>(Provider);
		require(ProviderRegistration.IsValid());
		InitializeShaderBuildServices();
	}

	auto InitializeShaderBuildForTesting() -> void
	{
		requiref(!ProviderRegistration.IsValid(),
			"ShaderBuild is already initialized");
		TestOwner = std::make_unique<FModuleTestOwner>("ShaderBuildTestRoot");
		ProviderRegistration = TestOwner->RegisterFeature(Provider);
		require(ProviderRegistration.IsValid());
		InitializeShaderBuildServices();
	}

	auto ShutdownShaderBuild() -> void
	{
		const FModularFeatureRetirementResult Retirement =
			ProviderRegistration.Reset();
		requiref(Retirement.Succeeded(),
			"ShaderBuild provider retirement failed: {}", Retirement.Message);
		ShutdownShaderCompileService();
		ShutdownShaderData();
		TestOwner.reset();
	}

	class FShaderBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			InitializeShaderBuild();
		}

		auto ShutdownModule() -> void override
		{
			ShutdownShaderBuild();
		}
	};

	IMPLEMENT_MODULE(FShaderBuildModule, ShaderBuild)
}
