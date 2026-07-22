#pragma once

#include "RHIDefinitions.h"
#include "RHIResources.h"

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
		std::optional<std::string> Value = std::nullopt;
	};

	struct FShaderCompileOptions
	{
		// Stable cache identity resolved by the caller. Leave empty to disable disk-backed shader cache reads and writes.
		std::string VirtualShaderPath;
		// Requested source-level entry points, such as `vertexMain` or `fragmentMain`.
		std::vector<const char8*> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
		// Backend/compiler build identity. The compile service fills this before cache lookup.
		std::string CompilerEnvironment;
		bool bForceRecompile = false;
	};

	struct FShaderResourceBinding
	{
		std::string Name;
		EShaderStageFlags StageFlags = EShaderStageFlags::None;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;

		auto operator==(const FShaderResourceBinding& Other) const -> bool
		{
			return Name == Other.Name
				&& StageFlags == Other.StageFlags
				&& SetIndex == Other.SetIndex
				&& BindingIndex == Other.BindingIndex
				&& Type == Other.Type
				&& ArraySize == Other.ArraySize;
		}
	};

	struct FShaderReflectionData
	{
		std::vector<FShaderResourceBinding> ResourceBindings;
		std::vector<FPushConstantRange> PushConstantRanges;
	};

	struct FCompiledShader
	{
		EShaderFrequency Frequency = EShaderFrequency::Vertex;
		// Source-level entry point requested by the caller, such as `vertexMain`.
		std::string SourceEntryPoint;
		// Backend-visible entry point exported by the compiled binary, such as Vulkan SPIR-V `main`.
		std::string BinaryEntryPoint = "main";
		std::string DebugName;
		std::shared_ptr<std::vector<std::byte>> Code;
		FXxHash128 Hash{};
		FShaderReflectionData Reflection;
	};

	struct FShaderCompilerOutput
	{
		bool bSucceeded = false;
		std::vector<FCompiledShader> CompiledShaders;
		std::string ErrorMessage;

		operator bool() const { return bSucceeded; }
	};
}
