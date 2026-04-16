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

		auto CompileShader(const char8* InShaderFilename, const char8* InEntryPoint) -> bool override;

	private:
		auto CompileShaderInternal(const char8* InShaderFilePath, const char8* InEntryPoint, Slang::ComPtr<slang::IBlob>& OutCode) const -> Slang::Result;

		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;

		Slang::ComPtr<slang::ISession> Session;
	};
}
