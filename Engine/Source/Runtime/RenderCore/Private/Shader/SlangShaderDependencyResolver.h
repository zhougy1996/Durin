#pragma once

#include "Shader/ShaderCompiler.h"

#include "slang.h"
#include "slang-com-ptr.h"

namespace Durin
{
	class FSlangShaderDependencyResolver
	{
	public:
		FSlangShaderDependencyResolver();
		~FSlangShaderDependencyResolver();

		auto Resolve(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> bool;

	private:
		auto CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool;
		auto InitGlobalSession() -> void;

		Slang::ComPtr<slang::IGlobalSession> GlobalSession;
	};
}
