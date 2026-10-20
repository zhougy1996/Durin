#include "Shader/IShaderBuildModule.h"

namespace Durin
{
	auto FormatShaderBuildAdmissionError(const FShaderBuildAdmissionError& Error) -> std::string
	{
		switch (Error.Reason)
		{
		case EShaderBuildAdmissionReason::Closed: return "Shader build admission is closed.";
		case EShaderBuildAdmissionReason::Capacity: return "Shader build admission capacity was exceeded.";
		case EShaderBuildAdmissionReason::DispatchRejected: return "Shader build task dispatch was rejected.";
		case EShaderBuildAdmissionReason::InvalidRequest: return "Shader build request is invalid.";
		}
		return "Shader build admission failed.";
	}

	auto IShaderBuildModule::Get() -> IShaderBuildModule*
	{
#if DURIN_WITH_EDITOR
		const auto Info = FModuleManager::Get().FindModule("ShaderBuild");
		if (Info && Info->State.load() == EModuleState::Active)
			return static_cast<IShaderBuildModule*>(Info->Module.get());
#endif
		return nullptr;
	}



	auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		if (auto* Module = IShaderBuildModule::Get())
			return Module->BuildSourceDependencyManifest(VirtualShaderPath, Options, OutDependencies);
		OutDependencies.clear();
		return std::unexpected(std::string("ShaderBuild module is unavailable."));
	}

	auto BuildShaderSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		if (auto* Module = IShaderBuildModule::Get())
			return Module->BuildSourceTreeFingerprint(VirtualShaderPath, Options, OutFingerprint);
		OutFingerprint = {};
		return std::unexpected(std::string("ShaderBuild module is unavailable."));
	}

	auto GetShaderBuildStats() -> FShaderBuildStats
	{
		if (auto* Module = IShaderBuildModule::Get()) return Module->GetStats();
		return {};
	}

	auto GetShaderCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderCompletion
	{
		if (auto* Module = IShaderBuildModule::Get()) return Module->GetCookInputIdentity(OutIdentity, IsCancelled);
		OutIdentity.clear();
		return std::unexpected(std::string("ShaderBuild module is unavailable."));
	}

	auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		const std::function<bool()>& IsCancelled) -> FShaderCompletion
	{
		if (auto* Module = IShaderBuildModule::Get())
			return Module->BuildCookedLibrary(TargetPlatform, TargetProfile, OutBytes, {}, IsCancelled);
		OutBytes.clear();
		return std::unexpected(std::string("ShaderBuild module is unavailable."));
	}
}
