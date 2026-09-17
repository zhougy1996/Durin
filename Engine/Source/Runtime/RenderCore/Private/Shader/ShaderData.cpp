#include "Serialization/BinaryFormat.h"
#include "ShaderDataInternal.h"

#include "Shader/Shader.h"
#include "Shader/ShaderData.h"

namespace Durin
{
	namespace
	{
		thread_local IShaderBuildProvider* CapturedProvider = nullptr;

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
			if (CapturedProvider)
				return {EFeatureInvokeStatus::Invoked, Visitor(*CapturedProvider), 1, 0};
			return FModularFeatureRegistry::Get().InvokeSingle<IShaderBuildProvider>(
				std::forward<TVisitor>(Visitor));
		}

		auto ProviderFailure(EFeatureInvokeStatus Status, uint32 MatchingCount) -> FShaderError
		{
			return {.Code = Status == EFeatureInvokeStatus::Unavailable
				? EShaderError::ProviderUnavailable : EShaderError::ProviderInvocationFailed,
				.Actual = MatchingCount, .ProviderStatus = Status};
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

	auto InitializeShaderData(FShaderDataConfiguration Configuration) -> FShaderOperationResult
	{
		if (Configuration.Domain == EShaderDataDomain::Authored)
		{
			if (!IsShaderBuildProviderAvailable())
			{
				return FShaderOperationResult::Failure(EShaderError::ProviderRequired);
			}
			Configuration.TargetPlatform = EShaderTargetPlatform::Win64;
			Configuration.TargetProfile = EShaderTargetProfile::EditorValidation;
			Configuration.CookRoot.clear();
		}
		else if (IsShaderBuildProviderAvailable())
		{
			return FShaderOperationResult::Failure(EShaderError::ProviderForbidden);
		}
		else if (Configuration.TargetPlatform != EShaderTargetPlatform::Win64
			|| Configuration.TargetProfile != EShaderTargetProfile::Game
			|| Configuration.CookRoot.empty()
			|| !Configuration.CookRoot.is_absolute()
			|| Configuration.CookRoot.lexically_normal() != Configuration.CookRoot)
		{
			return FShaderOperationResult::Failure(EShaderError::DataConfigurationInvalid);
		}
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		if (State.Configuration)
		{
			return FShaderOperationResult::Failure(EShaderError::DataAlreadyInitialized);
		}
		State.Configuration = std::move(Configuration);

		return {};
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
		FShaderCompilerOutput& OutOutput) -> FShaderOperationResult
	{
		OutOutput = {};
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		if (!State.Configuration
			|| State.Configuration->Domain != EShaderDataDomain::Cooked)
		{
			return FShaderOperationResult::Failure(EShaderError::CookedDomainRequired);
		}
		if (!State.Library.IsOpen())
		{
			if (auto Result = FreezeShaderRuntimeInventory(State.Configuration->TargetPlatform, State.Configuration->TargetProfile, State.Requests); !Result) return Result;
			const std::filesystem::path LibraryPath = State.Configuration->CookRoot
				/ ShaderCookedLibraryRelativePath;
			if (auto Result = FShaderCookedLibrary::Open(LibraryPath, State.Configuration->TargetPlatform, State.Configuration->TargetProfile, State.Requests, State.Library); !Result) return Result;
		}
		const auto Found = std::ranges::find_if(State.Requests,
			[RequestName](const FShaderRuntimeRequest& Request) {
				return Request.Name == RequestName;
			});
		if (Found == State.Requests.end())
		{
			return {.Error = {.Code = EShaderError::RequestUnregistered, .ActualIdentity = std::string(RequestName)}};
		}
		if (Found->Members.size() != ShaderTypes.size())
		{
			return {.Error = {.Code = EShaderError::RequestTypeCountMismatch,
				.Expected = Found->Members.size(),
				.Actual = ShaderTypes.size()}};
		}
		for (size_t Index = 0; Index < ShaderTypes.size(); ++Index)
			if (!ShaderTypes[Index]
				|| Found->Members[Index].TypeName != ShaderTypes[Index]->GetName())
			{
				return {.Error = {.Code = EShaderError::RequestTypesMismatch, .Index = Index}};
			}
		return State.Library.Load(*Found, OutOutput);
	}

	auto GetOrCompileShader(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options) -> FShaderCompilerOutput
	{
		auto Result = InvokeProvider<FShaderCompilerOutput>(
			[&](IShaderBuildProvider& Provider) {
				auto EffectiveOptions = Options;
				return Provider.CompileMounted(VirtualShaderPath, EffectiveOptions);
			});
		return Result.WasInvoked() && Result.Value
			? std::move(*Result.Value) : FShaderCompilerOutput{.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
	}

	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request) -> FShaderCompilerOutput
	{
		auto Result = InvokeProvider<FShaderCompilerOutput>(
			[&](IShaderBuildProvider& Provider) {
				auto EffectiveRequest = Request;
				return Provider.CompileGenerated(EffectiveRequest);
			});
		return Result.WasInvoked() && Result.Value
			? std::move(*Result.Value) : FShaderCompilerOutput{.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
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
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		auto Result = InvokeProvider<FShaderOperationResult>([&](IShaderBuildProvider& Provider) {
			auto EffectiveOptions = Options;
			return Provider.BuildSourceDependencyManifest(VirtualShaderPath, EffectiveOptions, OutDependencies);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		OutDependencies.clear();
		return {.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
	}

	auto BuildShaderSourceTreeFingerprintFromProvider(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		auto Result = InvokeProvider<FShaderOperationResult>([&](IShaderBuildProvider& Provider) {
			auto EffectiveOptions = Options;
			return Provider.BuildSourceTreeFingerprint(VirtualShaderPath, EffectiveOptions, OutFingerprint);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		OutFingerprint = {};
		return {.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
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

	auto WithShaderBuildProvider(const std::function<bool(IShaderBuildProvider&)>& Work) -> FShaderOperationResult
	{
		if (!Work || CapturedProvider)
		{
			return FShaderOperationResult::Failure(EShaderError::InvalidProviderCapture);
		}
		auto Result = FModularFeatureRegistry::Get().InvokeSingle<IShaderBuildProvider>(
			[&](IShaderBuildProvider& Provider) {
				struct FResetProvider
				{
					~FResetProvider() { CapturedProvider = nullptr; }
				} Reset;
				CapturedProvider = &Provider;
				return Work(Provider);
			});
		if (Result.WasInvoked() && Result.Value)
			return *Result.Value ? FShaderOperationResult{} : FShaderOperationResult{.Error = {.Code = EShaderError::ProviderWorkFailed}};
		return {.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
	}

	auto GetShaderCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderOperationResult
	{
		auto Result = InvokeProvider<FShaderOperationResult>([&](IShaderBuildProvider& Provider) {
			return Provider.GetCookInputIdentity(OutIdentity, IsCancelled);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		return {.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
	}

	auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		const std::function<bool()>& IsCancelled) -> FShaderOperationResult
	{
		auto Result = InvokeProvider<FShaderOperationResult>([&](IShaderBuildProvider& Provider) {
			return Provider.BuildCookedLibrary(
				TargetPlatform, TargetProfile, OutBytes, {}, IsCancelled);
		});
		if (Result.WasInvoked() && Result.Value) return *Result.Value;
		OutBytes.clear();
		return {.Error = ProviderFailure(Result.Status, Result.MatchingRegistrationCount)};
	}
}
