#pragma once

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
		struct FShaderDependencyInfo
		{
			std::string Path;
			uint64 FileSize = 0;
			FXxHash64 ContentHash{};
		};

		struct FShaderMetaData
		{
			// Source-level cache identity shared by all entry points and frequencies compiled from the same shader file.
			std::string VirtualShaderPath;
			FXxHash64 MainSourceHash{};
			FXxHash128 SourceTreeSignature{};
			std::vector<FShaderDependencyInfo> Dependencies;
		};

		struct FShaderVariantKey
		{
			FXxHash128 Value{};
			std::string Hex;
		};

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
		auto LoadMetaData(std::string_view VirtualShaderPath, FShaderMetaData& OutMetaData) const -> bool;
		auto IsMetaDataCurrent(const FShaderMetaData& CurrentMetaData, const FShaderMetaData& CachedMetaData) const -> bool;
		auto TryLoadShaderCache(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) const -> bool;
		auto SaveMetaData(const FShaderMetaData& MetaData) const -> bool;
		auto SaveCompiledShaderCache(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) const -> bool;

		auto CompileInternal(
			slang::ISession* InSession,
			const char8* InShaderFilePath,
			const std::span<const char8* const>& InEntryPoints,
			Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
			Slang::ComPtr<slang::IBlob>& OutDiagnostics
		) const -> Slang::Result;

		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;
	};
} // namespace Durin
