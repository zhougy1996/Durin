#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"
#include "Shader/ShaderCompilerCore.h"

#include <span>
#include <string>
#include <vector>

namespace Durin
{
	class DMaterialInterface;

	inline constexpr uint32 CurrentMaterialIRVersion = 1;
	inline constexpr uint32 CurrentMaterialGeneratorVersion = 1;
	inline constexpr uint32 CurrentMaterialCompilerEnvelopeVersion = 2;
	inline constexpr uint32 CurrentMaterialPassContractVersion = 1;

	struct FMaterialCompilerParameterDeclaration
	{
		FGuid Id;
		EMaterialParameterType Type = EMaterialParameterType::Scalar;

		auto operator==(const FMaterialCompilerParameterDeclaration&) const
			-> bool = default;
	};

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
		std::vector<uint32> SurfaceOutputs;

		auto operator==(const FMaterialIR&) const -> bool = default;
	};

	struct FMaterialProgramIdentity
	{
		FXxHash128 Digest;

		auto IsValid() const -> bool { return !Digest.IsZero(); }
		auto ToString() const -> std::string { return Digest.ToString(); }
		auto operator==(const FMaterialProgramIdentity&) const -> bool = default;
	};

	struct FMaterialNormalizationResult
	{
		bool bSucceeded = false;
		FMaterialIR IR;
		std::vector<std::byte> CanonicalBytes;
		FMaterialProgramIdentity Identity;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

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
		FMaterialIR IR;
		std::string GeneratedSource;
		std::vector<FMaterialCompilerDependency> Dependencies;
		std::vector<FCompiledShader> CompiledShaders;
		FMaterialCompileTimings Timings;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

		operator bool() const { return bSucceeded; }
	};

	ENGINE_API auto SnapshotMaterialCompilerInput(
		const DMaterialInterface& Material,
		FMaterialCompilerEnvironment Environment,
		FMaterialCompilerInput& OutInput,
		FMaterialProgramValidationResult& OutValidation) -> bool;

	ENGINE_API auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment,
		std::string& OutError) -> bool;

	ENGINE_API auto NormalizeMaterialProgram(
		const FMaterialCompilerInput& Input)
		-> FMaterialNormalizationResult;

	ENGINE_API auto EncodeMaterialIRCanonical(
		const FMaterialIR& IR,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;

	ENGINE_API auto BuildMaterialProgramIdentity(
		const FMaterialCompilerInput& Input,
		std::span<const std::byte> CanonicalIR)
		-> FMaterialProgramIdentity;
	ENGINE_API auto GenerateMaterialProgramSlang(
		const FMaterialIR& IR, std::string& OutSource,
		std::string& OutError) -> bool;
	ENGINE_API auto ValidateMaterialCompiledStages(
		std::span<const FCompiledShader> Stages,
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
