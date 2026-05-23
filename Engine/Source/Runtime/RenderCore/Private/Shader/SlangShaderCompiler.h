#pragma once

#include "Shader/ShaderCompiler.h"

#include "slang.h"
#include "slang-com-ptr.h"

namespace Durin
{
	struct FShaderCacheMetaData
	{
		int64 LastWriteTicks = 0;
		uint64 FileSize = 0;
		FXxHash64 ContentHash{};
	};

	class FSlangShaderCompiler : public FShaderCompiler
	{
	public:
		FSlangShaderCompiler();
		~FSlangShaderCompiler() override;

		auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput override;

	private:
		auto NormalizePath(const std::filesystem::path& InPath) const -> std::string;
		auto GetDependencyMetaRootDir(std::string_view ShaderName) const -> std::filesystem::path;
		auto GetDependencyMetaFilePath(std::string_view ShaderName, std::string_view ShaderSourceFilePath) const -> std::filesystem::path;
		auto SaveShaderDependencyMeta(
			std::string_view ShaderName,
			std::string_view ShaderSourceFilePath,
			FXxHash64 SourceSignatureHash,
			const std::vector<std::string>& DependencyPaths
		) const -> void;
		auto ResolveDependencyFiles(const char8* InShaderFilePath, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> Slang::Result;
		auto ComputeShaderSourceSignatureHash(const std::vector<std::string>& InDependencyPaths, const FShaderCompileOptions& Options) -> FXxHash64;
		auto GetOrComputeFileHash(std::string_view InPath) -> FXxHash64;
		auto TryLoadShaderCache(std::string_view ShaderName, const FShaderCompileOptions& Options, FXxHash64 SourceSignatureHash, FShaderCompilerOutput& OutOutput) -> bool;
		auto SaveCompiledShaderCache(std::string_view ShaderName, const FShaderCompileOptions& Options, FXxHash64 SourceSignatureHash, const FShaderCompilerOutput& Output) const -> void;

		auto CompileInternal(const char8* InShaderFilePath, const std::span<const char8* const>& InEntryPoints, std::vector<Slang::ComPtr<slang::IBlob>>& OutCodes, Slang::ComPtr<slang::IBlob>& OutDiagnostics) const -> Slang::Result;

		auto CompileInternal(
			const char8* InShaderFilePath,
			const std::span<const char8* const>& InEntryPoints,
			Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
			Slang::ComPtr<slang::IBlob>& OutDiagnostics
		) const -> Slang::Result;

		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;

		Slang::ComPtr<slang::ISession> Session;

		std::unordered_map<std::string, FShaderCacheMetaData> FileHashCache;
	};
} // namespace Durin
