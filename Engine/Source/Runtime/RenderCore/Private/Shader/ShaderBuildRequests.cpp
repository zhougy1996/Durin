#include "Shader/ShaderCompilerCore.h"

#include "ShaderDataInternal.h"

namespace Durin
{
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

	auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		return GetOrCompileGeneratedShader(Request);
	}
}
