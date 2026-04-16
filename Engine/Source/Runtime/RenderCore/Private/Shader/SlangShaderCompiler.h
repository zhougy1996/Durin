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

		auto Compile(const char8* InShaderFilename, const char8* InEntryPoint, std::vector<uint32>& OutCode) -> bool override;

		auto Compile(const char8* InShaderFilename, const std::span<const char8*>& InEntryPoints, std::vector<std::vector<uint32>>& OutCodes) -> bool override;

	private:
		auto CompileInternal(const char8* InShaderFilePath, const std::span<const char8*>& InEntryPoints, std::vector<Slang::ComPtr<slang::IBlob>>& OutCodes, Slang::ComPtr<slang::IBlob>& OutDiagnostics) const -> Slang::Result;

		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;

		Slang::ComPtr<slang::ISession> Session;
	};
} // namespace Doge
