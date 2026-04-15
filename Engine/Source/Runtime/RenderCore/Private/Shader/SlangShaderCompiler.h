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

	private:
		Slang::ComPtr<slang::ISession> SlangSession;
	};
}
