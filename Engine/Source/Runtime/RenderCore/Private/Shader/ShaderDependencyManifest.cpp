#include "Shader/ShaderCompilerCore.h"

#include "ShaderCompileService.h"

namespace Durin
{
	namespace
	{
		std::atomic_uint64_t GShaderReloadGeneration = 1;
	}

	auto GetShaderCompilerEnvironmentIdentity() -> std::string
	{
		return GetShaderCompilerEnvironmentIdentityFromService();
	}

	auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool
	{
		return BuildShaderSourceDependencyManifestFromService(
			VirtualShaderPath, Options, OutDependencies, OutError);
	}

	auto BuildShaderSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint,
		std::string& OutError) -> bool
	{
		return BuildShaderSourceTreeFingerprintFromService(
			VirtualShaderPath, Options, OutFingerprint, OutError);
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
