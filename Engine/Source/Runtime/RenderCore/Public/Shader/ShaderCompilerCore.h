#pragma once

#include "Shader/Shader.h"

namespace Durin
{
	struct FShaderMacroDefinition
	{
		FShaderMacroDefinition() = default;

		FShaderMacroDefinition(std::string_view InName)
			: Name(InName)
		{
		}

		FShaderMacroDefinition(std::string_view InName, std::string_view InValue)
			: Name(InName)
			, Value(InValue)
		{
		}

		auto HasValue() const -> bool
		{
			return Value.has_value();
		}

		std::string Name;
		std::optional<std::string> Value;
	};

	struct FShaderCompileOptions
	{
		// Stable cache identity resolved by the caller. Leave empty to disable disk-backed shader cache reads and writes.
		std::string VirtualShaderPath;
		// Requested source-level entry points, such as `vertexMain` or `fragmentMain`.
		std::vector<const char8*> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
		bool bForceRecompile = false;
	};

	struct FShaderCompilerOutput
	{
		bool bSucceeded = false;
		std::vector<FCompiledShader> CompiledShaders;
		std::string ErrorMessage;

		operator bool() const { return bSucceeded; }
	};
} // namespace Durin
