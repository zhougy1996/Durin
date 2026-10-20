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
			std::atomic<EShaderCodeFormat> DefaultFormat{EShaderCodeFormat{0}};
			std::vector<FShaderRuntimeRequest> Requests;
			FShaderCookedLibrary Library;
		};

		auto ShaderDataState() -> FShaderDataState&
		{
			static FShaderDataState State;
			return State;
		}
	}

	auto FShaderDataConfiguration::Authored(
		EShaderTargetPlatform InTargetPlatform) -> FShaderDataConfiguration
	{
		return {.TargetPlatform = InTargetPlatform};
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
			if (Configuration.TargetPlatform != EShaderTargetPlatform::Win64
				&& Configuration.TargetPlatform != EShaderTargetPlatform::MacOS)
				return std::unexpected(std::string("Cooked Shader data configuration is invalid."));
			// Startup initializes the domain before the manager publishes the module as Active.
			const auto Info = FModuleManager::Get().FindModule("ShaderBuild");
			if (!Info || !Info->Module || (Info->State.load() != EModuleState::Loading
				&& Info->State.load() != EModuleState::Active))
			{
				return std::unexpected(std::string("Authored Shader data requires a ShaderBuild module."));
			}
			Configuration.TargetProfile = EShaderTargetProfile::EditorValidation;
			Configuration.CookRoot.clear();
		}
		else if (IShaderBuildModule::Get())
		{
			return std::unexpected(std::string("Cooked Shader data forbids a ShaderBuild module."));
		}
		else if ((Configuration.TargetPlatform != EShaderTargetPlatform::Win64
				&& Configuration.TargetPlatform != EShaderTargetPlatform::MacOS)
			|| Configuration.TargetProfile != EShaderTargetProfile::Game
			|| Configuration.CookRoot.empty()
			|| !Configuration.CookRoot.is_absolute()
			|| Configuration.CookRoot.lexically_normal() != Configuration.CookRoot)
		{
			return std::unexpected(std::string("Cooked Shader data configuration is invalid."));
		}
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		if (State.Configuration)
		{
			return std::unexpected(std::string("Shader data domain is already initialized."));
		}
		State.Configuration = std::move(Configuration);
		State.DefaultFormat.store(State.Configuration->TargetPlatform != EShaderTargetPlatform::MacOS
			? EShaderCodeFormat::Spirv15 : State.Configuration->Domain == EShaderDataDomain::Cooked
			? EShaderCodeFormat::MetalLibrary : EShaderCodeFormat::Msl20Source, std::memory_order_release);

		return {};
	}

	auto ShutdownShaderData() -> void
	{
		FShaderDataState& State = ShaderDataState();
		std::lock_guard Lock(State.Mutex);
		State.Library = {};
		State.Requests.clear();
		State.Configuration.reset();
		State.DefaultFormat.store(EShaderCodeFormat{0}, std::memory_order_release);
	}

	auto GetConfiguredShaderTarget() -> std::optional<FShaderTargetIdentity>
	{
		// Library loading holds the configuration lock while constructing options.
		// Read only the independently published immutable default here.
		switch (ShaderDataState().DefaultFormat.load(std::memory_order_acquire))
		{
		case EShaderCodeFormat::Spirv15: return VulkanShaderTarget;
		case EShaderCodeFormat::Msl20Source: return MetalShaderTarget;
		case EShaderCodeFormat::MetalLibrary: return MetalLibraryShaderTarget;
		default: return {};
		}
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
			return std::unexpected(std::string("Cooked Shader data was requested outside the Cooked domain."));
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
			return std::unexpected(std::format("Cooked Shader request '{}' is not registered.", (std::string(RequestName))));
		}
		if (Found->Members.size() != ShaderTypes.size())
		{
			return std::unexpected(std::string("Cooked Shader request type count does not match registration."));
		}
		for (size_t Index = 0; Index < ShaderTypes.size(); ++Index)
			if (!ShaderTypes[Index]
				|| Found->Members[Index].TypeName != ShaderTypes[Index]->GetName())
			{
				return std::unexpected(std::string("Cooked Shader request types do not match registration."));
			}
		return State.Library.Load(*Found, OutOutput);
	}

}
