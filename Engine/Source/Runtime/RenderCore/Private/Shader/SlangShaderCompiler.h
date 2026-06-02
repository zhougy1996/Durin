#pragma once

#include "Misc/FileFingerprintCache.h"
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
		auto NormalizePath(const std::filesystem::path& InPath) const -> std::string;
		auto CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool;
		auto ResolveDependencyFiles(slang::ISession* InSession, const char8* InShaderFilePath, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> Slang::Result;

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
		FFileFingerprintCache FileFingerprintCache;
	};
} // namespace Durin
