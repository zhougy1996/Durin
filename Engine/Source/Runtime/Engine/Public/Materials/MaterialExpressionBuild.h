#pragma once

#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialProgramCompiler.h"

#include <map>
#include <set>
#include <tuple>
#include <functional>

namespace Durin
{
	inline constexpr uint32 InvalidMaterialExpressionIndex = 0xffffffffu;
	struct FMaterialExpressionTextureDefault
	{
		FMaterialSamplerState Sampler;
		EMaterialTextureFallback Fallback = EMaterialTextureFallback::White;
	};

	// Function texture defaults are values, not fake resource nodes or IR indices.
	struct FMaterialExpressionBuildValue
	{
		std::variant<uint32, FMaterialExpressionTextureDefault> Value = InvalidMaterialExpressionIndex;
		FMaterialExpressionBuildValue() = default;
		FMaterialExpressionBuildValue(uint32 Index) : Value(Index) {}
		FMaterialExpressionBuildValue(FMaterialExpressionTextureDefault Texture) : Value(std::move(Texture)) {}
		auto GetIndex() const -> const uint32* { return std::get_if<uint32>(&Value); }
		auto GetTexture() const -> const FMaterialExpressionTextureDefault* { return std::get_if<FMaterialExpressionTextureDefault>(&Value); }
	};

	// Supplied by the owning function collection. The provider may not load assets.
	struct FMaterialExpressionFunctionBody
	{
		FMaterialFunctionSignature Signature;
		std::vector<DMaterialExpression*> Expressions;
		std::string AssetPath;
		uint64 Revision = 0;
	};
	struct FMaterialExpressionBuildEnvironment
	{
		std::function<std::optional<FMaterialExpressionFunctionBody>(const DMaterialFunctionInterface&)> FindFunction;
	};
	struct FMaterialExpressionFunctionDependency
	{
		std::string AssetPath;
		uint64 Revision = 0;
	};

	// Owning-thread admission of editable dependencies; publishes stamps only on success.
	ENGINE_API auto ValidateMaterialFunctionDependencies(std::span<DMaterialFunctionInterface* const> Roots,
		std::vector<FMaterialFunctionOwnerStamp>& OutOwners) -> FMaterialProgramValidationResult;
	ENGINE_API auto ValidateMaterialFunctionCallSignature(const DMaterialExpressionFunctionCall& Call,
		const FMaterialFunctionSignature& Signature) -> FMaterialProgramValidationResult;

	// Detached result. No expression, texture, or callee object is retained.
	struct FMaterialExpressionBuildResult
	{
		FMaterialIR IR;
		std::vector<uint32> Roots;
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		std::vector<FMaterialExpressionSource> Sources;
		std::vector<FMaterialExpressionFunctionDependency> Dependencies;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return Diagnostics.empty(); }
	};

	// Owning-thread graph traversal. Expressions emit IR through this context directly.
	// Its lifetime must not cross a graph edit or an asynchronous dispatch.
	class FMaterialExpressionBuildContext
	{
	public:
		ENGINE_API explicit FMaterialExpressionBuildContext(std::span<DMaterialExpression* const> Expressions,
			FMaterialExpressionBuildEnvironment Environment = {});
		FMaterialExpressionBuildContext(const FMaterialExpressionBuildContext&) = delete;
		auto operator=(const FMaterialExpressionBuildContext&) -> FMaterialExpressionBuildContext& = delete;
		// Local authoring validation checks typed links without requiring available callee bodies.
		// Its opaque function values stay private and can never become compiler snapshots.
		ENGINE_API static auto ValidateSurface(std::span<DMaterialExpression* const> Expressions,
			const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		ENGINE_API static auto ValidateFunction(std::span<DMaterialExpression* const> Expressions,
			const FMaterialFunctionSignature& Signature) -> FMaterialProgramValidationResult;
		ENGINE_API auto Resolve(const FMaterialExpressionInput& Input) -> FMaterialExpressionBuildValue;
		ENGINE_API auto ResolveIndex(const FMaterialExpressionInput& Input) -> uint32;
		ENGINE_API auto FunctionInput(FGuid PortId) -> FMaterialExpressionBuildValue;
		ENGINE_API auto FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FMaterialExpressionBuildValue;
		ENGINE_API auto FunctionCall(const DMaterialExpressionFunctionCall& Call, FGuid OutputId) -> FMaterialExpressionBuildValue;
		ENGINE_API auto Emit(FMaterialIRNode Node) -> uint32;
		ENGINE_API auto Literal(std::span<const float> Components) -> uint32;
		ENGINE_API auto Parameter(FGuid Id, EMaterialParameterType Type) -> uint32;
		ENGINE_API auto Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::span<const FMaterialExpressionInput* const> Inputs,
			std::span<const std::vector<float>* const> Defaults,
			std::span<const uint8> Swizzle = {}) -> uint32;
		ENGINE_API auto Coordinates() -> uint32;
		ENGINE_API auto SampleOutput(const DMaterialExpression& Expression, uint8 OutputIndex) -> uint32;
		ENGINE_API auto Fail(std::string Message, FGuid PortId = {},
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> uint32;
		auto GetNode(uint32 Index) const -> const FMaterialIRNode& { return Result.IR.Nodes.at(Index); }
		ENGINE_API auto Finish(std::span<const FMaterialExpressionInput> Roots) -> FMaterialExpressionBuildResult;
		ENGINE_API auto FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs) -> FMaterialExpressionBuildResult;
	private:
		struct FSharedState
		{
			FMaterialExpressionBuildResult Result;
			std::vector<uint32> Depths;
			uint32 LinkCount = 0;
			uint64 ClosureBytes = 0;
			std::vector<const DMaterialFunctionInterface*> ActiveFunctions;
			std::map<const DMaterialFunctionInterface*, FMaterialExpressionFunctionBody> Functions;
		};
		FMaterialExpressionBuildContext(FMaterialExpressionBuildContext& Parent,
			const FMaterialExpressionFunctionBody& Body, FGuid CallId);
		auto Admit(std::span<DMaterialExpression* const> InExpressions) -> void;
		bool bValidateAuthoring = false;
		FXxHash128Builder AuthoringCodeHash;
		auto OpaqueAuthoringValue(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<uint32> Inputs = {}) -> uint32;
		auto ValidateAuthoringCall(const DMaterialExpressionFunctionCall& Call, FGuid OutputId)
			-> FMaterialExpressionBuildValue;
		auto BuildAllExpressions() -> void;
		uint64 AuthoredLinks = 0;
		auto MatchesType(const FMaterialExpressionBuildValue& Value, EMaterialProgramValueType Type) const -> bool;
		auto BroadcastScalar(FMaterialExpressionBuildValue Value, EMaterialProgramValueType Type) -> FMaterialExpressionBuildValue;
		std::shared_ptr<FSharedState> Shared;
		FMaterialExpressionBuildResult& Result;
		std::vector<uint32>& Depths;
		uint32& LinkCount;
		FMaterialExpressionBuildEnvironment Environment;
		const FMaterialFunctionSignature* Signature = nullptr;
		std::string FunctionPath;
		std::vector<FGuid> CallPath;
		std::vector<FGuid> PortStack;
		std::map<FGuid, FMaterialExpressionBuildValue> BoundInputs;
		std::map<FGuid, std::map<FGuid, FMaterialExpressionBuildValue>> CallOutputs;
		using FOutputKey = std::tuple<FGuid, uint8, FGuid>;
		std::map<FGuid, DMaterialExpression*> Expressions;
		std::map<FOutputKey, FMaterialExpressionBuildValue> Values;
		std::set<FOutputKey> Active;
		std::vector<FGuid> SourceStack;
	};

	[[nodiscard]] ENGINE_API auto BuildMaterialExpressionGraph(
		std::span<DMaterialExpression* const> Expressions,
		std::span<const FMaterialExpressionInput> Roots,
		FMaterialExpressionBuildEnvironment Environment = {}) -> FMaterialExpressionBuildResult;
}
