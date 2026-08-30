#pragma once

#include "Shader/ShaderCompilerCore.h"

#include "slang.h"
#include "slang-com-ptr.h"

namespace Durin
{
	// Defines the Slang compile environment shared by session creation and shader cache identity.
	class FSlangSessionEnvironment
	{
	public:
		static constexpr std::string_view BackendName = "slang";
		static constexpr SlangCompileTarget TargetFormat = SLANG_SPIRV;
		static constexpr std::string_view TargetFormatName = "SPIR-V";
		static constexpr std::string_view TargetIdentity = "spirv";
		static constexpr std::string_view TargetProfileName = "spirv_1_5";

		static auto NormalizeMacros(
			const FShaderCompileOptions& Options,
			std::vector<FShaderMacroDefinition>& OutMacros,
			std::string& OutErrorMessage
		) -> bool;

		static auto CreateSession(
			slang::IGlobalSession& GlobalSession,
			const FShaderCompileOptions& Options,
			Slang::ComPtr<slang::ISession>& OutSession,
			std::string& OutErrorMessage,
			std::string_view SearchPath = {}
		) -> bool;
	};
} // namespace Durin
