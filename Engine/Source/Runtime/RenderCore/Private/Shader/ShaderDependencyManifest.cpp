#include "Shader/ShaderCompilerCore.h"

#include "ShaderCompileService.h"

namespace Durin
{
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

	auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		return GetOrCompileGeneratedShader(Request);
	}
}
