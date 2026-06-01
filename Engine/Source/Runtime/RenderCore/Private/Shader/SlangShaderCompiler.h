#pragma once

#include "Shader/ShaderCacheStore.h"
#include "Shader/ShaderCompiler.h"

#include "slang.h"
#include "slang-com-ptr.h"

namespace Durin
{
	class FSlangShaderCompiler : public FShaderCompiler
	{
	public:
		FSlangShaderCompiler();
		~FSlangShaderCompiler() override;

		auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput override;

	private:
		auto TryMakeShaderVirtualPath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) const -> bool;
		auto NormalizePath(const std::filesystem::path& InPath) const -> std::string;
		auto CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool;
		auto ResolveDependencyFiles(slang::ISession* InSession, const char8* InShaderFilePath, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> Slang::Result;
		auto NormalizeMacros(const FShaderCompileOptions& Options, std::vector<FShaderMacroDefinition>& OutMacros, std::string& OutErrorMessage) const -> bool;
		auto BuildShaderMetaData(
			std::string_view VirtualShaderPath,
			std::string_view ShaderSourceFilePath,
			const std::vector<std::string>& InDependencyPaths,
			FShaderMetaData& OutMetaData,
			std::string& OutErrorMessage
		) const -> bool;
		auto BuildVariantKey(
			const FShaderMetaData& MetaData,
			const std::vector<FShaderMacroDefinition>& Macros,
			FShaderVariantKey& OutVariantKey
		) const -> void;

		auto CompileInternal(
			slang::ISession* InSession,
			const char8* InShaderFilePath,
			const std::span<const char8* const>& InEntryPoints,
			Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
			Slang::ComPtr<slang::IBlob>& OutDiagnostics
		) const -> Slang::Result;

		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;
		FShaderCacheStore CacheStore;
	};
} // namespace Durin
