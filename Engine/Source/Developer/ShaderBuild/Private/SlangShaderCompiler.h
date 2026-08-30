#pragma once

#include "ShaderCompiler.h"

#include "slang.h"
#include "slang-com-ptr.h"

#include <mutex>

namespace Durin
{
	// Compiles registered shader sources to Vulkan-compatible binaries through Slang.
	class FSlangShaderCompiler : public FShaderCompiler
	{
	public:
		FSlangShaderCompiler();
		~FSlangShaderCompiler() override;

		auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput override;
		auto CompileSource(std::string_view ModuleName,
			std::string_view SourcePathHint, std::string_view Source,
			const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
		auto GetEnvironmentIdentity() const -> std::string;

	private:
		auto CompileInternal(
			slang::ISession* InSession,
			slang::IModule* InModule,
			const std::span<const char8* const>& InEntryPoints,
			Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
			Slang::ComPtr<slang::IBlob>& OutDiagnostics
		) const -> Slang::Result;
		auto CompileModule(slang::IModule* Module,
			const FShaderCompileOptions& Options) -> FShaderCompilerOutput;

		auto InitGlobalSession() -> void;

		// Slang global sessions are non-reentrant; derived objects must also die under this lock.
		mutable std::mutex GlobalSessionMutex;
		Slang::ComPtr<slang::IGlobalSession> GlobalSession;
	};
} // namespace Durin
