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
#include <variant>

namespace Durin
{
	class DMaterialInterface;

	inline constexpr uint32 CurrentMaterialIRVersion = 4;
	inline constexpr uint32 CurrentMaterialGeneratorVersion = 7;
	inline constexpr uint32 CurrentMaterialCompilerEnvelopeVersion = 9;
	inline constexpr uint32 CurrentMaterialPassContractVersion = 3;

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

	struct FMaterialIRSwizzle
	{
		uint8 Length = 0;
		std::array<uint8, 4> Components{};
		auto operator==(const FMaterialIRSwizzle&) const -> bool = default;
	};

	// Only the selected immediate payload is present. Operations without immediates
	// use monostate; opcode/payload agreement is checked before encoding/generation.
	using FMaterialIRPayload = std::variant<std::monostate, FMaterialProgramLiteral, FGuid, FMaterialIRSwizzle>;

	struct FMaterialIRNode
	{
		EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		std::vector<uint32> Inputs;
		FMaterialIRPayload Payload;

		auto GetLiteral() const -> FMaterialProgramLiteral
		{
			const auto* Value = std::get_if<FMaterialProgramLiteral>(&Payload);
			return Value ? *Value : FMaterialProgramLiteral{};
		}
		auto GetParameterId() const -> FGuid
		{
			const auto* Value = std::get_if<FGuid>(&Payload);
			return Value ? *Value : FGuid{};
		}
		auto GetSwizzle() const -> FMaterialIRSwizzle
		{
			const auto* Value = std::get_if<FMaterialIRSwizzle>(&Payload);
			return Value ? *Value : FMaterialIRSwizzle{};
		}
		auto HasValidPayload() const -> bool
		{
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: return std::holds_alternative<FMaterialProgramLiteral>(Payload);
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter: return std::holds_alternative<FGuid>(Payload);
			case EMaterialProgramOpcode::Swizzle: return std::holds_alternative<FMaterialIRSwizzle>(Payload);
			default: return std::holds_alternative<std::monostate>(Payload);
			}
		}

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
		std::optional<uint32> InputIndex;
		std::optional<uint32> UVFieldIndex;
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

	// Final compiler input: detached typed IR and its binding/environment contract.
	// Authored Program nodes, function call tables, and object references are absent.
	struct FMaterialIRCompilerInput
	{
		FMaterialIR IR;
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerEnvironment Environment;
		std::vector<FMaterialExpressionSource> Sources;
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
		const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment,
		FMaterialIRCompilerInput& OutInput, std::vector<FMaterialFunctionOwnerStamp>* OutOwners = nullptr)
		-> FMaterialProgramValidationResult;
	ENGINE_API auto AreMaterialFunctionOwnersCurrent(std::span<const FMaterialFunctionOwnerStamp> Owners) -> bool;

	[[nodiscard]] ENGINE_API auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment) -> FMaterialOperationResult;

	ENGINE_API auto NormalizeMaterialIR(const FMaterialIRCompilerInput& Input) -> FMaterialNormalizationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialIR(const FMaterialIR& IR,
		const FMaterialRenderLayout& Layout) -> FMaterialProgramValidationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialIR(const FMaterialIR& IR,
		std::span<const FMaterialCompilerParameterDeclaration> Parameters) -> FMaterialProgramValidationResult;

	[[nodiscard]] ENGINE_API auto EncodeMaterialIRCanonical(
		const FMaterialIR& IR,
		FByteBuffer& OutBytes) -> FMaterialOperationResult;

	ENGINE_API auto BuildMaterialProgramIdentity(const FMaterialIRCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity;
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
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const FMaterialIR& IR) -> FMaterialSourceGenerationResult;
	ENGINE_API auto CompileMaterialIR(const FMaterialIRCompilerInput& Input,
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
