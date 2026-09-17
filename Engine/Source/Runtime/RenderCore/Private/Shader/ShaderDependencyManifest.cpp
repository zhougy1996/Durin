#include "Shader/ShaderCompilerCore.h"

#include "ShaderDataInternal.h"

namespace Durin
{
	namespace
	{
		std::atomic_uint64_t GShaderReloadGeneration = 1;
	}

	auto GetShaderCompilerEnvironmentIdentity() -> std::string
	{
		return GetShaderCompilerEnvironmentIdentityFromProvider();
	}

	auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		return BuildShaderSourceDependencyManifestFromProvider(VirtualShaderPath, Options, OutDependencies);
	}

	auto BuildShaderSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		return BuildShaderSourceTreeFingerprintFromProvider(VirtualShaderPath, Options, OutFingerprint);
	}

	auto GetShaderReloadGeneration() -> uint64
	{
		return GShaderReloadGeneration.load(std::memory_order_acquire);
	}

	auto AdvanceShaderReloadGeneration() -> uint64
	{
		return GShaderReloadGeneration.fetch_add(
			1, std::memory_order_acq_rel) + 1;
	}

	auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		return GetOrCompileGeneratedShader(Request);
	}
}
