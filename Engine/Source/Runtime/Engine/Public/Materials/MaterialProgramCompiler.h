#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Shader/MaterialShaderIdentity.h"
#include "Shader/ShaderCompilerCore.h"

#include <optional>
#include <span>
#include <array>
#include <string>
#include <vector>
#include <variant>

namespace Durin
{
	class DMaterialInterface;

	namespace MIR
	{
		inline constexpr uint32 CurrentVersion = 4;
	}
	inline constexpr uint32 CurrentMaterialGeneratorVersion = 8;
	inline constexpr uint32 CurrentMaterialCompilerEnvelopeVersion = 9;
	inline constexpr uint32 CurrentMaterialPassContractVersion = 4;

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

	namespace MIR
	{
		struct FSwizzle
		{
			uint8 Length = 0;
			std::array<uint8, 4> Components{};
			auto operator==(const FSwizzle&) const -> bool = default;
		};

		// Only the selected immediate payload is present. Operations without immediates
		// use monostate; opcode/payload agreement is checked before encoding/generation.
		using FPayload = std::variant<std::monostate, FMaterialProgramLiteral, FGuid, FSwizzle>;

		struct FNode
		{
			EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;
			EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
			std::vector<uint32> Inputs;
			FPayload Payload;

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
			auto GetSwizzle() const -> FSwizzle
			{
				const auto* Value = std::get_if<FSwizzle>(&Payload);
				return Value ? *Value : FSwizzle{};
			}
			auto HasValidPayload() const -> bool
			{
				switch (Opcode)
				{
				case EMaterialProgramOpcode::Constant: return std::holds_alternative<FMaterialProgramLiteral>(Payload);
				case EMaterialProgramOpcode::Parameter:
				case EMaterialProgramOpcode::TextureParameter: return std::holds_alternative<FGuid>(Payload);
				case EMaterialProgramOpcode::Swizzle: return std::holds_alternative<FSwizzle>(Payload);
				default: return std::holds_alternative<std::monostate>(Payload);
				}
			}

			auto operator==(const FNode&) const -> bool = default;
		};

		struct FModule
		{
			uint32 Version = CurrentVersion;
			std::vector<FNode> Nodes;
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

			auto operator==(const FModule&) const -> bool = default;
		};

		// Per-input diagnostic metadata, excluded from shared artifact identity and cooked bytes.
		struct FSource
		{
			uint32 ExpressionIndex = 0;
			FGuid NodeId;
			FGuid PortId;
			std::string FunctionAssetPath;
			std::vector<FGuid> CallPath;
			std::optional<uint32> InputIndex;
			std::optional<uint32> UVFieldIndex;
		};

		struct FNormalizationResult
		{
			bool bSucceeded = false;
			FModule IR;
			std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
			FMaterialRenderLayout Layout;
			FByteBuffer CanonicalBytes;
			FMaterialProgramIdentity Identity;
			std::vector<FMaterialProgramDiagnostic> Diagnostics;
			std::vector<FSource> Sources;

			explicit operator bool() const { return bSucceeded; }
		};

		// Final compiler input: detached typed IR and its binding/environment contract.
		// Authored Program nodes, function call tables, and object references are absent.
		struct FCompilerInput
		{
			FModule IR;
			std::vector<FMaterialCompilerParameterDeclaration> Parameters;
			FMaterialStaticProperties StaticProperties;
			FMaterialCompilerEnvironment Environment;
			std::vector<FSource> Sources;
		};
	}

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
		MIR::FModule IR;
		// Sorted unique runtime binding contract, published with these shaders.
		// Values and resource references remain owned by material definitions/instances.
		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		FMaterialRenderLayout Layout;
		std::string GeneratedSource;
		std::vector<FMaterialCompilerDependency> Dependencies;
		std::vector<FCompiledShader> CompiledShaders;
		FMaterialCompileTimings Timings;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FMaterialCompilerSnapshot
	{
		MIR::FCompilerInput Input;
		std::vector<FMaterialFunctionOwnerStamp> FunctionOwners;
	};

	// Captured on the owning thread. A failed capture never publishes a payload.
	struct FMaterialCompilerSnapshotResult
	{
		std::optional<FMaterialCompilerSnapshot> Snapshot;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return Snapshot.has_value(); }
	};

	[[nodiscard]] ENGINE_API auto SnapshotMaterialCompilerInput(
		const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment)
		-> FMaterialCompilerSnapshotResult;
	ENGINE_API auto AreMaterialFunctionOwnersCurrent(std::span<const FMaterialFunctionOwnerStamp> Owners) -> bool;

	[[nodiscard]] ENGINE_API auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment) -> FMaterialOperationResult;

	namespace MIR
	{
		ENGINE_API auto Normalize(const FCompilerInput& Input) -> FNormalizationResult;
		[[nodiscard]] ENGINE_API auto Validate(const FModule& IR,
			const FMaterialRenderLayout& Layout) -> FMaterialProgramValidationResult;
		[[nodiscard]] ENGINE_API auto Validate(const FModule& IR,
			std::span<const FMaterialCompilerParameterDeclaration> Parameters) -> FMaterialProgramValidationResult;

		[[nodiscard]] ENGINE_API auto EncodeCanonical(
			const FModule& IR,
			FByteBuffer& OutBytes) -> FMaterialOperationResult;
	}

	ENGINE_API auto BuildMaterialProgramIdentity(const MIR::FCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity;
	struct FMaterialSourceGenerationResult
	{
		bool bSucceeded = false;
		std::string Source;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return bSucceeded; }
	};

	[[nodiscard]] ENGINE_API auto ValidateMaterialCompilerResult(
		const FMaterialCompilerResult& Result) -> FMaterialLayoutValidationResult;
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const MIR::FModule& IR, const FMaterialRenderLayout& Layout) -> FMaterialSourceGenerationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialCompiledStages(
		std::span<const FCompiledShader> Stages, const FMaterialRenderLayout& Layout,
		const FMaterialCompilerResourceLimits& Limits = {}) -> FMaterialLayoutValidationResult;
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const MIR::FModule& IR) -> FMaterialSourceGenerationResult;
	namespace MIR
	{
		ENGINE_API auto Compile(const FCompilerInput& Input,
			bool bForceRecompile = false) -> FMaterialCompilerResult;
	}
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
