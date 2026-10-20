#include "ShaderBuildSession.h"
#include "MetalLibraryBuild.h"
#include "ShaderBuildTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/ShaderData.h"
#include "RHIGlobals.h"
#include "ShaderBuild/ShaderPaths.h"
#include "ShaderBuilder.h"
#include "ShaderLibraryProducer.h"
#include "ShaderCookInput.h"

namespace Durin
{

	class FShaderBuildModule;
	static FShaderBuildModule* ActiveModule = nullptr;
	class FShaderBuildModule final : public IShaderBuildModule
	{
	public:
		auto ResetService(DerivedData::FBuildServiceOptions Options) -> void
		{
			AdvanceShaderReloadGeneration();
			if (Builder) Builder->Close();
			if (BuildService) BuildService->Close();
			Builder.reset();
			BuildService->ResetCacheForTests(std::move(Options));
			Builder = std::make_unique<FShaderBuilder>(BuildService);
		}
		auto SubmitMounted(std::string_view Path, const FShaderCompileOptions& CompileOptions,
			FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion)
			-> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError> override
		{ return Builder->SubmitMounted(Path, CompileOptions, std::move(Options), std::move(Completion)); }

		auto SubmitGenerated(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
			FShaderBuildCompletion Completion) -> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError> override
		{ return Builder->SubmitGenerated(std::move(Request), std::move(Options), std::move(Completion)); }

		auto GetBuildVersion() const -> uint64 override { return Builder->GetBuildVersion(); }
		auto GetCompileAdapterVersion() const -> FShaderCompileAdapterVersion override
		{
			const auto Descriptor = GetShaderBuildDescriptor();
			return {Descriptor.Version, Descriptor.ConstantsSchema, Descriptor.OutputSchema};
		}
		auto GetCompilerEnvironmentIdentity() -> std::string override
		{
			return Builder->GetCompilerEnvironmentIdentity();
		}

		auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult override
		{
			return Builder->BuildSourceDependencyManifest(VirtualShaderPath, Options, OutDependencies);
		}

		auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult override
		{
			return Builder->BuildSourceTreeFingerprint(VirtualShaderPath, Options, OutFingerprint);
		}

		auto GetNativeLibraryProducerIdentity() -> std::expected<std::string, std::string> override
		{ return GetMetalLibraryProducerIdentity(); }

		auto GetTargetProducerIdentity(const FShaderTargetIdentity& Target) -> std::expected<std::string, std::string> override
		{
			if (Target != MetalLibraryShaderTarget) return IShaderBuildModule::GetTargetProducerIdentity(Target);
			auto Native = GetMetalLibraryProducerIdentity();
			if (!Native) return std::unexpected(Native.error());
			return std::format("{};metal-library={};native={}", GetCompilerEnvironmentIdentity(), MetalLibraryBuildVersion, *Native);
		}

		auto GetCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderCompletion override
		{ return BuildCookShaderInputIdentity(*Builder, OutIdentity, IsCancelled); }

		auto GetStats() const -> FShaderBuildStats override
		{
			return Builder->GetStats();
		}

		auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::shared_ptr<const FShaderSourceArtifacts> Artifacts,
			const std::function<bool()>& IsCancelled) -> FShaderCompletion override
		{
			return ProduceCookedShaderLibrary(
				*Builder, TargetPlatform, TargetProfile, OutBytes, std::move(Artifacts), IsCancelled);
		}

		// Resident until normal editor/tool shutdown; dynamic reloading is unsupported.
		auto StartupModule() -> void override
		{
			ActiveModule = this;
			FShaderPaths::InitDefaultMountPoints();
			BuildService = std::make_shared<FShaderBuildService>();
			Builder = std::make_unique<FShaderBuilder>(BuildService);
			const auto Backend = ResolveRHIBackend(std::getenv("DURIN_RHI_BACKEND"));
			requiref(Backend.has_value(), "Invalid RHI backend for ShaderBuild startup");
			const auto TargetPlatform = *Backend == ERHIBackend::Metal
				? EShaderTargetPlatform::MacOS : EShaderTargetPlatform::Win64;
			const auto Result = InitializeShaderData(
				FShaderDataConfiguration::Authored(TargetPlatform));
			requiref(Result,
				"Authored Shader data initialization failed: {}", std::string(Result.error()));
		}

		auto ShutdownModule() -> void override
		{
			ActiveModule = nullptr;
			if (Builder) Builder->Close();
			if (BuildService) BuildService->Close();
			Builder.reset();
			BuildService.reset();
			ShutdownShaderData();
		}
	private:
		std::shared_ptr<FShaderBuildService> BuildService;
		std::unique_ptr<FShaderBuilder> Builder;
	};
	namespace ShaderBuildPrivate
	{
		auto ResetServiceForTests(DerivedData::FBuildServiceOptions Options) -> void
		{
			require(ActiveModule);
			ActiveModule->ResetService(std::move(Options));
		}
	}

	IMPLEMENT_MODULE(FShaderBuildModule, ShaderBuild)
}
