#pragma once

#include "CoreMinimal.h"

#include "Shader/ShaderCompilerCore.h"

#include "slang.h"
#include "slang-com-ptr.h"

namespace Durin
{
	inline auto FormatSlangDiagnostic(std::string_view Phase, std::string_view Text, std::optional<int64> Status = {}) -> std::string
	{
		if (!Text.empty()) return std::string(Text.substr(0, ShaderBuildDiagnosticMaxBytes));
		return std::format("Slang phase {} failed (status {}): {}", Phase,
			Status ? std::to_string(*Status) : "unavailable", Text.substr(0, ShaderBuildDiagnosticMaxBytes));
	}

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
			std::vector<FShaderMacroDefinition>& OutMacros) -> FShaderOperationResult;

		static auto CreateSession(
			slang::IGlobalSession& GlobalSession,
			const FShaderCompileOptions& Options,
			Slang::ComPtr<slang::ISession>& OutSession,
			std::string_view SearchPath = {}) -> FShaderOperationResult;
	};
} // namespace Durin
