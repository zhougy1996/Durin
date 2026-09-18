#pragma once

#include "Materials/MaterialExpressionBuild.h"
#include <map>
#include <set>
#include <tuple>

namespace Durin::MIR
{
	// One graph invocation. Child invocations share IR storage, but own bindings and caches.
	class FGraphBuilderImpl
	{
	public:
		explicit FGraphBuilderImpl(std::span<DMaterialExpression* const> Expressions,
			FBuildEnvironment Environment = {});
		FGraphBuilderImpl(const FGraphBuilderImpl&) = delete;
		auto operator=(const FGraphBuilderImpl&) -> FGraphBuilderImpl& = delete;
		// Local authoring validation checks typed links without requiring available callee bodies.
		// Its opaque function values stay private and can never become compiler snapshots.
		static auto ValidateSurface(std::span<DMaterialExpression* const> Expressions,
			const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		static auto ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		auto Resolve(const FMaterialExpressionInput& Input) -> FValue;
		auto ResolveIndex(const FMaterialExpressionInput& Input) -> uint32;
		auto FunctionInput(FGuid PortId) -> FValue;
		auto FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FValue;
		auto FunctionCall(const DMaterialExpressionFunctionCall& Call, FEmitter& Emitter) -> void;
		auto Emit(FNode Node) -> uint32;
		auto Literal(std::span<const float> Components) -> uint32;
		auto Parameter(FGuid Id, EMaterialParameterType Type) -> uint32;
		auto Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::span<const FMaterialExpressionInput* const> Inputs,
			std::span<const std::vector<float>* const> Defaults,
			std::span<const uint8> Swizzle = {}) -> uint32;
		auto Coordinates() -> uint32;
		auto Fail(FMaterialError Error, FGuid PortId = {},
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> uint32;
		auto GetNode(uint32 Index) const -> const FNode& { return Result.IR.Nodes.at(Index); }
		auto Finish(std::span<const FMaterialExpressionInput> Roots) -> FBuildResult;
		auto FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs) -> FBuildResult;
	private:
		friend class FEmitter;
		struct FSharedState
		{
			FBuildResult Result;
			std::vector<uint32> Depths;
			uint32 LinkCount = 0;
			uint64 ClosureBytes = 0;
			std::vector<const DMaterialFunctionInterface*> ActiveFunctions;
			std::map<const DMaterialFunctionInterface*, FFunctionBody> Functions;
			std::map<FGuid, ETextureUsage> TextureUsages;
		};
		FGraphBuilderImpl(FGraphBuilderImpl& Parent,
			const FFunctionBody& Body, FGuid CallId);
		auto Admit(std::span<DMaterialExpression* const> InExpressions) -> void;
		bool bValidateAuthoring = false;
		FXxHash128Builder AuthoringCodeHash;
		auto OpaqueAuthoringValue(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<uint32> Inputs = {}) -> uint32;
		auto ValidateAuthoringCall(const DMaterialExpressionFunctionCall& Call, FEmitter& Emitter) -> void;
		auto BuildAllExpressions() -> void;
		uint64 AuthoredLinks = 0;
		auto MatchesType(const FValue& Value, EMaterialProgramValueType Type) const -> bool;
		auto BroadcastScalar(FValue Value, EMaterialProgramValueType Type) -> FValue;
		std::shared_ptr<FSharedState> Shared;
		FBuildResult& Result;
		std::vector<uint32>& Depths;
		uint32& LinkCount;
		FBuildEnvironment Environment;
		const FMaterialFunctionSignature* Signature = nullptr;
		std::string FunctionPath;
		std::vector<FGuid> CallPath;
		std::vector<FGuid> PortStack;
		std::map<FGuid, FValue> BoundInputs;
		using FOutputKey = std::tuple<FGuid, uint8, FGuid>;
		std::map<FGuid, DMaterialExpression*> Expressions;
		std::map<FOutputKey, FValue> Values;
		std::set<FGuid> Active;
		std::set<FGuid> Built;
		auto BuildExpression(const DMaterialExpression& Expression) -> void;
		auto ReportMissingOutput(const DMaterialExpression& Expression, const FMaterialExpressionInput& Input) -> void;
		std::vector<FGuid> SourceStack;
	};

}
