#include "Shader/IShaderBuildModule.h"

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
			// Startup initializes the domain before the manager publishes the module as Active.
			const auto Info = FModuleManager::Get().FindModule("ShaderBuild");
			if (!Info || !Info->Module || (Info->State.load() != EModuleState::Loading
				&& Info->State.load() != EModuleState::Active))
			{
				return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleRequired});
			}
			Configuration.TargetPlatform = EShaderTargetPlatform::Win64;
			Configuration.TargetProfile = EShaderTargetProfile::EditorValidation;
			Configuration.CookRoot.clear();
		}
		else if (IShaderBuildModule::Get())
		{
			return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleForbidden});
		}
		else if (Configuration.TargetPlatform != EShaderTargetPlatform::Win64
			|| Configuration.TargetProfile != EShaderTargetProfile::Game
			|| Configuration.CookRoot.empty()
			|| !Configuration.CookRoot.is_absolute()
			|| Configuration.CookRoot.lexically_normal() != Configuration.CookRoot)
		{
			return std::unexpected(FShaderError{.Code = EShaderError::DataConfigurationInvalid});
		}
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		if (State.Configuration)
		{
			return std::unexpected(FShaderError{.Code = EShaderError::DataAlreadyInitialized});
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
			return std::unexpected(FShaderError{.Code = EShaderError::CookedDomainRequired});
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
			return std::unexpected(FShaderError{.Code = EShaderError::RequestUnregistered, .ActualIdentity = std::string(RequestName)});
		}
		if (Found->Members.size() != ShaderTypes.size())
		{
			return std::unexpected(FShaderError{.Code = EShaderError::RequestTypeCountMismatch,
				.Expected = Found->Members.size(),
				.Actual = ShaderTypes.size()});
		}
		for (size_t Index = 0; Index < ShaderTypes.size(); ++Index)
			if (!ShaderTypes[Index]
				|| Found->Members[Index].TypeName != ShaderTypes[Index]->GetName())
			{
				return std::unexpected(FShaderError{.Code = EShaderError::RequestTypesMismatch, .Index = Index});
			}
		return State.Library.Load(*Found, OutOutput);
	}

}
