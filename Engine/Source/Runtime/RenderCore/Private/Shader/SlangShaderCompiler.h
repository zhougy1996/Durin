#pragma once

#include "ShaderCompiler.h"

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
		auto GetEnvironmentIdentity() const -> std::string;

	private:
		auto CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool;

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
