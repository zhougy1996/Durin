#pragma once

#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "RenderCoreAPI.h"

namespace Durin
{
	// Represents a shader preprocessor definition with an optional explicit value.
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

	// Carries source identity, entry points, variants, and cache policy into compilation.
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

	// Describes one resource binding reflected from a compiled shader stage.
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

	// Aggregates descriptor bindings and push-constant ranges for compiled shader code.
	struct FShaderReflectionData
	{
		std::vector<FShaderResourceBinding> ResourceBindings;
		std::vector<FPushConstantRange> PushConstantRanges;
	};

	// Owns one compiled stage binary and the reflection data needed to bind it.
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

	// Reports compilation status and either compiled stages or a diagnostic message.
	struct FShaderCompilerOutput
	{
		bool bSucceeded = false;
		std::vector<FCompiledShader> CompiledShaders;
		std::string ErrorMessage;

		operator bool() const { return bSucceeded; }
	};

	struct FShaderSourceDependencyFingerprint
	{
		std::string VirtualPath;
		FXxHash128 ContentHash;

		auto operator==(const FShaderSourceDependencyFingerprint&) const
			-> bool = default;
	};

	// Owns an in-memory generated root. RenderCore resolves imports, compiles,
	// reflects, and caches it without materializing authored source on disk.
	struct FGeneratedShaderCompileRequest
	{
		std::string VirtualPath;
		std::string Source;
		std::vector<std::string> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
		std::vector<std::string> AllowedImportVirtualPrefixes;
		bool bForceRecompile = false;
	};

	// Produces value-owned compiler and reachable-source identity without
	// exposing physical cache paths to higher-level compilers.
	RENDERCORE_API auto GetShaderCompilerEnvironmentIdentity() -> std::string;
	RENDERCORE_API auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool;
	RENDERCORE_API auto CompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput;
}
