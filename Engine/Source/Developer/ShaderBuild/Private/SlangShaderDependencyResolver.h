#pragma once

#include "Shader/ShaderCompilerCore.h"

#include "slang.h"
#include "slang-com-ptr.h"

#include <mutex>

namespace Durin
{
	// Resolves the complete physical source dependency set used for shader cache identity.
	class FSlangShaderDependencyResolver
	{
	public:
		FSlangShaderDependencyResolver();
		~FSlangShaderDependencyResolver();

		auto Resolve(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> bool;
		auto ResolveSource(std::string_view ModuleName,
			std::string_view SourcePathHint, std::string_view Source,
			const FShaderCompileOptions& Options,
			std::vector<std::string>& OutDependencyPaths,
			std::string& OutDiagnostics) const -> bool;

	private:
		auto InitGlobalSession() -> void;

		// Slang global sessions are non-reentrant; derived objects must also die under this lock.
		mutable std::mutex GlobalSessionMutex;
		Slang::ComPtr<slang::IGlobalSession> GlobalSession;
	};
}
