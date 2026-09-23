#include "Shader/IShaderBuildModule.h"

namespace Durin
{
	auto IShaderBuildModule::Get() -> IShaderBuildModule*
	{
#if DURIN_WITH_EDITOR
		const auto Info = FModuleManager::Get().FindModule("ShaderBuild");
		if (Info && Info->State.load() == EModuleState::Active)
			return static_cast<IShaderBuildModule*>(Info->Module.get());
#endif
		return nullptr;
	}

	auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request) -> FShaderCompilerOutput
	{
		if (auto* Module = IShaderBuildModule::Get()) return Module->CompileGenerated(Request);
		return {.Error = {.Code = EShaderError::BuildModuleUnavailable}};
	}

	auto GetShaderCompilerEnvironmentIdentity() -> std::string
	{
		if (auto* Module = IShaderBuildModule::Get()) return Module->GetCompilerEnvironmentIdentity();
		return {};
	}

	auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		if (auto* Module = IShaderBuildModule::Get())
			return Module->BuildSourceDependencyManifest(VirtualShaderPath, Options, OutDependencies);
		OutDependencies.clear();
		return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleUnavailable});
	}

	auto BuildShaderSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		if (auto* Module = IShaderBuildModule::Get())
			return Module->BuildSourceTreeFingerprint(VirtualShaderPath, Options, OutFingerprint);
		OutFingerprint = {};
		return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleUnavailable});
	}

	auto GetShaderBuildStats() -> FShaderBuildStats
	{
		if (auto* Module = IShaderBuildModule::Get()) return Module->GetStats();
		return {};
	}

	auto GetShaderCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderOperationResult
	{
		if (auto* Module = IShaderBuildModule::Get()) return Module->GetCookInputIdentity(OutIdentity, IsCancelled);
		OutIdentity.clear();
		return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleUnavailable});
	}

	auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		const std::function<bool()>& IsCancelled) -> FShaderOperationResult
	{
		if (auto* Module = IShaderBuildModule::Get())
			return Module->BuildCookedLibrary(TargetPlatform, TargetProfile, OutBytes, {}, IsCancelled);
		OutBytes.clear();
		return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleUnavailable});
	}
}
