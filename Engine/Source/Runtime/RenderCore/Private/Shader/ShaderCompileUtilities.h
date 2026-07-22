#pragma once

#include "ShaderCacheStore.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	class FFileFingerprintCache;

	namespace ShaderCompileUtilities
	{
		auto NormalizeMacros(const FShaderCompileOptions& Options, std::vector<FShaderMacroDefinition>& OutMacros, std::string& OutErrorMessage) -> bool;

		auto BuildShaderMetaData(
			const std::vector<std::string>& InDependencyPaths,
			FFileFingerprintCache& FileFingerprintCache,
			FShaderMetaData& OutMetaData,
			std::string& OutErrorMessage
		) -> bool;

		auto BuildVariantKey(
			std::string_view VirtualShaderPath,
			const FShaderMetaData& MetaData,
			const std::vector<FShaderMacroDefinition>& Macros,
			std::string_view CompilerEnvironment,
			FShaderVariantKey& OutVariantKey
		) -> void;

		auto IsMetaDataCurrent(const FShaderMetaData& CurrentMetaData, const FShaderMetaData& CachedMetaData) -> bool;
	} // namespace ShaderCompileUtilities
} // namespace Durin
