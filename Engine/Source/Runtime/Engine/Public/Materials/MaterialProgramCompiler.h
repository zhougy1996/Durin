#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Shader/MaterialShaderIdentity.h"
#include "Shader/ShaderCompilerCore.h"

#include <span>
#include <array>
#include <string>
#include <vector>

namespace Durin
{
	class DMaterialInterface;

	inline constexpr uint32 CurrentMaterialIRVersion = 4;
	inline constexpr uint32 CurrentMaterialGeneratorVersion = 5;
	inline constexpr uint32 CurrentMaterialCompilerEnvelopeVersion = 7;
	inline constexpr uint32 CurrentMaterialPassContractVersion = 2;

	struct FMaterialCompilerDependency
	{
		std::string VirtualPath;
		FXxHash128 ContentHash;

		auto operator==(const FMaterialCompilerDependency&) const
			-> bool = default;
	};

	struct FMaterialCompilerEnvironment
	{
		std::string CompilerIdentity;
		std::string Target = "vulkan-spirv-1.5";
		uint32 PassContractVersion = CurrentMaterialPassContractVersion;
		std::vector<FMaterialCompilerDependency> Dependencies;
		FMaterialCompilerResourceLimits ResourceLimits;

		auto operator==(const FMaterialCompilerEnvironment&) const
			-> bool = default;
	};

	// Detached, value-owned GameThread snapshot. Dynamic parameter/resource
	// values and every reflected/live owner are intentionally absent.
	struct FMaterialCompilerInput
	{
		FMaterialProgram Program;
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerEnvironment Environment;
		std::vector<FMaterialFunctionCallSnapshot> FunctionCalls;
		FMaterialFunctionClosure Functions;

		auto operator==(const FMaterialCompilerInput&) const -> bool = default;
	};

	struct FMaterialIRNode
	{
		EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		std::vector<uint32> Inputs;
		FMaterialProgramLiteral Literal;
		FGuid ParameterId;
		uint8 SwizzleLength = 0;
		uint8 SwizzleX = 0;
		uint8 SwizzleY = 0;
		uint8 SwizzleZ = 0;
		uint8 SwizzleW = 0;

		auto operator==(const FMaterialIRNode&) const -> bool = default;
	};

	struct FMaterialIR
	{
		uint32 Version = CurrentMaterialIRVersion;
		std::vector<FMaterialIRNode> Nodes;
		struct FSurfaceInput
		{
			bool bExpression = false;
			uint32 ExpressionIndex = 0;
			EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
			FMaterialProgramLiteral Literal;
			auto operator==(const FSurfaceInput&) const -> bool = default;
		};
		struct FSurfaceRoot
		{
			bool bAggregate = false;
			uint32 AggregateExpressionIndex = 0;
			std::array<FSurfaceInput, 8> Inputs;
			auto operator==(const FSurfaceRoot&) const -> bool = default;
		};
		FSurfaceRoot SurfaceRoot;

		auto operator==(const FMaterialIR&) const -> bool = default;
	};

	// Per-input diagnostic metadata, excluded from shared artifact identity and cooked bytes.
	struct FMaterialExpressionSource
	{
		uint32 ExpressionIndex = 0;
		FGuid NodeId;
		FGuid PortId;
		std::string FunctionAssetPath;
		std::vector<FGuid> CallPath;
	};

	struct FMaterialNormalizationResult
	{
		bool bSucceeded = false;
		FMaterialIR IR;
		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		FMaterialRenderLayout Layout;
		FByteBuffer CanonicalBytes;
		FMaterialProgramIdentity Identity;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		std::vector<FMaterialExpressionSource> Sources;

		operator bool() const { return bSucceeded; }
	};

	struct FMaterialCompileTimings
	{
		uint64 NormalizationMicroseconds = 0;
		uint64 GenerationMicroseconds = 0;
		uint64 CompilationMicroseconds = 0;
	};

	struct FMaterialCompilerResult
	{
		bool bSucceeded = false;
		FMaterialProgramIdentity Identity;
		std::string CompilerIdentity;
		std::string Target;
		uint32 PassContractVersion = CurrentMaterialPassContractVersion;
		FMaterialIR IR;
		// Sorted unique runtime binding contract, published with these shaders.
		// Values and resource references remain owned by material definitions/instances.
		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		FMaterialRenderLayout Layout;
		std::string GeneratedSource;
		std::vector<FMaterialCompilerDependency> Dependencies;
		std::vector<FCompiledShader> CompiledShaders;
		FMaterialCompileTimings Timings;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

		operator bool() const { return bSucceeded; }
	};

	[[nodiscard]] ENGINE_API auto SnapshotMaterialCompilerInput(
		const DMaterialInterface& Material,
		FMaterialCompilerEnvironment Environment,
		FMaterialCompilerInput& OutInput) -> FMaterialProgramValidationResult;

	ENGINE_API auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment,
		std::string& OutError) -> bool;

	ENGINE_API auto NormalizeMaterialProgram(
		const FMaterialCompilerInput& Input)
		-> FMaterialNormalizationResult;

	ENGINE_API auto EncodeMaterialIRCanonical(
		const FMaterialIR& IR,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool;

	ENGINE_API auto BuildMaterialProgramIdentity(
		const FMaterialCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout)
		-> FMaterialProgramIdentity;
	struct FMaterialSourceGenerationResult
	{
		std::string Source;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return Diagnostics.empty() && !Source.empty(); }
	};

	[[nodiscard]] ENGINE_API auto ValidateMaterialCompilerResult(
		const FMaterialCompilerResult& Result) -> FMaterialLayoutValidationResult;
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const FMaterialIR& IR, const FMaterialRenderLayout& Layout) -> FMaterialSourceGenerationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialCompiledStages(
		std::span<const FCompiledShader> Stages, const FMaterialRenderLayout& Layout,
		const FMaterialCompilerResourceLimits& Limits = {}) -> FMaterialLayoutValidationResult;
	ENGINE_API auto GenerateMaterialProgramSlang(
		const FMaterialIR& IR, std::string& OutSource,
		std::string& OutError) -> bool;
	ENGINE_API auto CompileMaterialProgram(
		const FMaterialCompilerInput& Input,
		bool bForceRecompile = false) -> FMaterialCompilerResult;
}

template<>
struct std::hash<Durin::FMaterialProgramIdentity>
{
	auto operator()(const Durin::FMaterialProgramIdentity& Identity) const
		noexcept -> size_t
	{
		return std::hash<Durin::FXxHash128>{}(Identity.Digest);
	}
};
