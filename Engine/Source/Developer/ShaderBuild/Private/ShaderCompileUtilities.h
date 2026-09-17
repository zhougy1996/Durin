#pragma once

#include "ShaderDependencyManifestStore.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	class FFileFingerprintCache;

	namespace ShaderCompileUtilities
	{
		enum class EMetaDataReuseStatus : uint8
		{
			Current,
			Stale,
			Failed
		};

		struct FMetaDataReuseResult
		{
			EMetaDataReuseStatus Status = EMetaDataReuseStatus::Failed;
			FShaderError Error;
		};

		auto NormalizeMacros(const FShaderCompileOptions& Options, std::vector<FShaderMacroDefinition>& OutMacros) -> FShaderOperationResult;

		auto BuildShaderMetaData(
			const std::vector<std::string>& InDependencyPaths,
			FFileFingerprintCache& FileFingerprintCache,
			FShaderMetaData& OutMetaData) -> FShaderOperationResult;

		auto BuildVariantKey(
			std::string_view VirtualShaderPath,
			const FShaderMetaData& MetaData,
			const std::vector<FShaderMacroDefinition>& Macros,
			std::string_view CompilerEnvironment,
			FShaderVariantKey& OutVariantKey
		) -> void;

		auto BuildDependencyKey(
			std::string_view VirtualShaderPath,
			const std::vector<FShaderMacroDefinition>& Macros,
			std::string_view CompilerEnvironment,
			FShaderDependencyKey& OutDependencyKey
		) -> void;

		auto TryReuseMetaData(
			const FShaderMetaData& CachedMetaData,
			FFileFingerprintCache& FileFingerprintCache
		) -> FMetaDataReuseResult;
	} // namespace ShaderCompileUtilities
} // namespace Durin
