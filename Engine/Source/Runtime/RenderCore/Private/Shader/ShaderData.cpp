#include "ShaderDataInternal.h"

#include "Shader/Shader.h"
#include "Shader/ShaderData.h"

namespace Durin
{
	namespace
	{
		struct FShaderDataState
		{
			std::mutex Mutex;
			std::optional<FShaderDataConfiguration> Configuration;
			std::vector<FShaderRuntimeRequest> Requests;
			FShaderCookedLibrary Library;
		};

		auto ShaderDataState() -> FShaderDataState&
		{
			static FShaderDataState State;
			return State;
		}

		template<typename TResult, typename TVisitor>
		auto InvokeProvider(TVisitor&& Visitor) -> TFeatureInvokeResult<TResult>
		{
			return FModularFeatureRegistry::Get().InvokeSingle<IShaderBuildProvider>(
				std::forward<TVisitor>(Visitor));
		}

		auto UnavailableOutput(std::string_view Operation)
			-> FShaderCompilerOutput
		{
			FShaderCompilerOutput Output;
			Output.ErrorMessage = std::format(
				"ShaderBuild provider is unavailable for {}.", Operation);
			return Output;
		}
	}

	auto FShaderDataConfiguration::Authored() -> FShaderDataConfiguration
	{
		return {};
	}

	auto FShaderDataConfiguration::Cooked(
		std::filesystem::path InCookRoot,
		EShaderTargetPlatform InTargetPlatform,
		EShaderTargetProfile InTargetProfile) -> FShaderDataConfiguration
	{
		return {EShaderDataDomain::Cooked, InTargetPlatform, InTargetProfile,
			std::move(InCookRoot)};
	}

	auto InitializeShaderData(
		FShaderDataConfiguration Configuration,
		std::string& OutError) -> bool
	{
		if (Configuration.Domain == EShaderDataDomain::Authored)
		{
			if (!IsShaderBuildProviderAvailable())
			{
				OutError = "Authored Shader data requires a ShaderBuild provider.";
				return false;
			}
			Configuration.TargetPlatform = EShaderTargetPlatform::Win64;
			Configuration.TargetProfile = EShaderTargetProfile::EditorValidation;
			Configuration.CookRoot.clear();
		}
		else if (IsShaderBuildProviderAvailable())
		{
			OutError = "Cooked Shader data forbids a ShaderBuild provider.";
			return false;
		}
		else if (Configuration.TargetPlatform != EShaderTargetPlatform::Win64
			|| Configuration.TargetProfile != EShaderTargetProfile::Game
			|| Configuration.CookRoot.empty()
			|| !Configuration.CookRoot.is_absolute()
			|| Configuration.CookRoot.lexically_normal() != Configuration.CookRoot)
		{
			OutError = "Cooked Shader data configuration is invalid.";
			return false;
		}
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		if (State.Configuration)
		{
			OutError = "Shader data domain is already initialized.";
			return false;
		}
		State.Configuration = std::move(Configuration);
		OutError.clear();
		return true;
	}

	auto ShutdownShaderData() -> void
	{
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		State.Library = {};
		State.Requests.clear();
		State.Configuration.reset();
	}

	auto GetShaderDataDomain() -> EShaderDataDomain
	{
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		requiref(State.Configuration.has_value(),
			"Shader data domain has not been initialized");
		return State.Configuration->Domain;
	}

	auto LoadCookedShaderRuntimeRequest(
		std::string_view RequestName,
		std::span<const FShaderType* const> ShaderTypes,
		FShaderCompilerOutput& OutOutput,
		std::string& OutError) -> bool
	{
		OutOutput = {};
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		if (!State.Configuration
			|| State.Configuration->Domain != EShaderDataDomain::Cooked)
		{
			OutError = "Cooked Shader data was requested outside the Cooked domain.";
			return false;
		}
		if (!State.Library.IsOpen())
		{
			if (!FreezeShaderRuntimeInventory(
					State.Configuration->TargetPlatform,
					State.Configuration->TargetProfile,
					State.Requests, OutError)) return false;
			const std::filesystem::path LibraryPath = State.Configuration->CookRoot
				/ ShaderCookedLibraryRelativePath;
			if (!FShaderCookedLibrary::Open(
					LibraryPath, State.Configuration->TargetPlatform,
					State.Configuration->TargetProfile, State.Requests,
					State.Library, OutError)) return false;
		}
		const auto Found = std::ranges::find_if(State.Requests,
			[RequestName](const FShaderRuntimeRequest& Request) {
				return Request.Name == RequestName;
			});
		if (Found == State.Requests.end())
		{
			OutError = std::format(
				"Cooked Shader request '{}' is not registered.", RequestName);
			return false;
		}
		if (Found->Members.size() != ShaderTypes.size())
		{
			OutError = "Cooked Shader request type count does not match registration.";
			return false;
		}
		for (size_t Index = 0; Index < ShaderTypes.size(); ++Index)
			if (!ShaderTypes[Index]
				|| Found->Members[Index].TypeName != ShaderTypes[Index]->GetName())
			{
				OutError = "Cooked Shader request types do not match registration.";
				return false;
			}
		return State.Library.Load(*Found, OutOutput, OutError);
	}

	auto GetOrCompileShader(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options) -> FShaderCompilerOutput
	{
		auto Result = InvokeProvider<FShaderCompilerOutput>(
			[&](IShaderBuildProvider& Provider) {
				return Provider.CompileMounted(VirtualShaderPath, Options);
			});
		return Result.WasInvoked() && Result.Value
			? std::move(*Result.Value) : UnavailableOutput("mounted compilation");
	}

	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request) -> FShaderCompilerOutput
	{
		auto Result = InvokeProvider<FShaderCompilerOutput>(
			[&](IShaderBuildProvider& Provider) {
				return Provider.CompileGenerated(Request);
			});
		return Result.WasInvoked() && Result.Value
			? std::move(*Result.Value) : UnavailableOutput("generated compilation");
	}

	auto GetShaderCompilerEnvironmentIdentityFromProvider() -> std::string
	{
		auto Result = InvokeProvider<std::string>(
			[](IShaderBuildProvider& Provider) {
				return Provider.GetCompilerEnvironmentIdentity();
			});
		return Result.WasInvoked() && Result.Value
			? std::move(*Result.Value) : std::string{};
	}

	auto BuildShaderSourceDependencyManifestFromProvider(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool
	{
		auto Result = InvokeProvider<bool>([&](IShaderBuildProvider& Provider) {
			return Provider.BuildSourceDependencyManifest(
				VirtualShaderPath, Options, OutDependencies, OutError);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		OutDependencies.clear();
		OutError = "ShaderBuild provider is unavailable for dependency inspection.";
		return false;
	}

	auto BuildShaderSourceTreeFingerprintFromProvider(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint,
		std::string& OutError) -> bool
	{
		auto Result = InvokeProvider<bool>([&](IShaderBuildProvider& Provider) {
			return Provider.BuildSourceTreeFingerprint(
				VirtualShaderPath, Options, OutFingerprint, OutError);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		OutFingerprint = {};
		OutError = "ShaderBuild provider is unavailable for source fingerprinting.";
		return false;
	}

	auto IsShaderBuildProviderAvailable() -> bool
	{
		auto Result = InvokeProvider<bool>(
			[](IShaderBuildProvider&) { return true; });
		return Result.WasInvoked() && Result.Value && *Result.Value;
	}

	auto GetShaderBuildStats() -> FShaderBuildStats
	{
		auto Result = InvokeProvider<FShaderBuildStats>(
			[](IShaderBuildProvider& Provider) { return Provider.GetStats(); });
		return Result.WasInvoked() && Result.Value
			? *Result.Value : FShaderBuildStats{};
	}

	auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		auto Result = InvokeProvider<bool>([&](IShaderBuildProvider& Provider) {
			return Provider.BuildCookedLibrary(
				TargetPlatform, TargetProfile, OutBytes, OutError);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		OutBytes.clear();
		OutError = "ShaderBuild provider is unavailable for Cook production.";
		return false;
	}
}
