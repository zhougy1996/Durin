#include "Shader/ShaderCompilerCore.h"

#include "Misc/FileHelper.h"
#include "Shader/ShaderPaths.h"
#include "ShaderCompileService.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

#include <algorithm>
#include <tuple>

namespace Durin
{
	auto GetShaderCompilerEnvironmentIdentity() -> std::string
	{
		FSlangShaderCompiler Compiler;
		return Compiler.GetEnvironmentIdentity();
	}

	auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool
	{
		OutDependencies.clear();
		OutError.clear();
		if (VirtualShaderPath.empty())
		{
			OutError = "Shader dependency manifest requires a virtual root path.";
			return false;
		}
		const std::string PhysicalRoot =
			FShaderPaths::SourcePath(VirtualShaderPath);
		FShaderCompileOptions EffectiveOptions = Options;
		EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
		EffectiveOptions.CompilerEnvironment =
			GetShaderCompilerEnvironmentIdentity();

		std::vector<std::string> PhysicalDependencies;
		FSlangShaderDependencyResolver Resolver;
		if (!Resolver.Resolve(
			PhysicalRoot, EffectiveOptions, PhysicalDependencies, OutError))
			return false;

		OutDependencies.reserve(PhysicalDependencies.size());
		for (const std::string& PhysicalPath : PhysicalDependencies)
		{
			std::string VirtualPath;
			if (!FShaderPaths::TryMakeVirtualSourcePath(
				PhysicalPath, VirtualPath))
			{
				OutError = "Shader dependency cannot be represented by a registered virtual path.";
				OutDependencies.clear();
				return false;
			}
			FXxHash128 ContentHash;
			std::error_code ErrorCode;
			if (!FFileHelper::HashFileXx128(
				PhysicalPath, ContentHash, ErrorCode))
			{
				OutError = std::format(
					"Failed to fingerprint shader dependency {}: {}",
					VirtualPath, ErrorCode.message());
				OutDependencies.clear();
				return false;
			}
			OutDependencies.push_back({
				.VirtualPath = std::move(VirtualPath),
				.ContentHash = ContentHash});
		}
		std::ranges::sort(OutDependencies, [](const auto& A, const auto& B) {
			return std::tie(A.VirtualPath, A.ContentHash.HashHigh,
				A.ContentHash.HashLow)
				< std::tie(B.VirtualPath, B.ContentHash.HashHigh,
					B.ContentHash.HashLow);
		});
		OutDependencies.erase(
			std::unique(OutDependencies.begin(), OutDependencies.end()),
			OutDependencies.end());
		return true;
	}

	auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		return GetOrCompileGeneratedShader(Request);
	}
}
