#pragma once

#include "Shader/ShaderCompiler.h"

#include "slang.h"
#include "slang-com-ptr.h"

namespace Doge
{
	class FSlangShaderCompiler : public FShaderCompiler
	{
	public:
		FSlangShaderCompiler();
		~FSlangShaderCompiler() override;

		auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions &Options) -> FShaderCompilerOutput override;

	private:
		auto CompileInternal(const char8* InShaderFilePath, const std::span<const char8* const>& InEntryPoints, std::vector<Slang::ComPtr<slang::IBlob>>& OutCodes, Slang::ComPtr<slang::IBlob>& OutDiagnostics) const -> Slang::Result;

		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;

		Slang::ComPtr<slang::ISession> Session;
	};
} // namespace Doge
