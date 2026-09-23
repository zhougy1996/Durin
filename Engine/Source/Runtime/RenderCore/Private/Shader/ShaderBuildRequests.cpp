#include "Shader/ShaderCompilerCore.h"

#include "ShaderDataInternal.h"

namespace Durin
{
	auto GetShaderCompilerEnvironmentIdentity() -> std::string
	{
		return GetShaderCompilerEnvironmentIdentityFromModule();
	}

	auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		return BuildShaderSourceDependencyManifestFromModule(VirtualShaderPath, Options, OutDependencies);
	}

	auto BuildShaderSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		return BuildShaderSourceTreeFingerprintFromModule(VirtualShaderPath, Options, OutFingerprint);
	}

	auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		return GetOrCompileGeneratedShader(Request);
	}
}
