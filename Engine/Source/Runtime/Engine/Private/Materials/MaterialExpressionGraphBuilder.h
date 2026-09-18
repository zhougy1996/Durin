#pragma once

#include "Materials/MaterialExpressionBuild.h"
#include <map>
#include <set>
#include <tuple>

namespace Durin
{
	// One graph invocation. Child invocations share IR storage, but own bindings and caches.
	class FMaterialExpressionGraphBuilderImpl
	{
	public:
		explicit FMaterialExpressionGraphBuilderImpl(std::span<DMaterialExpression* const> Expressions,
			FMaterialExpressionBuildEnvironment Environment = {});
		FMaterialExpressionGraphBuilderImpl(const FMaterialExpressionGraphBuilderImpl&) = delete;
		auto operator=(const FMaterialExpressionGraphBuilderImpl&) -> FMaterialExpressionGraphBuilderImpl& = delete;
		// Local authoring validation checks typed links without requiring available callee bodies.
		// Its opaque function values stay private and can never become compiler snapshots.
		static auto ValidateSurface(std::span<DMaterialExpression* const> Expressions,
			const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		static auto ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		auto Resolve(const FMaterialExpressionInput& Input) -> FMaterialExpressionBuildValue;
		auto ResolveIndex(const FMaterialExpressionInput& Input) -> uint32;
		auto FunctionInput(FGuid PortId) -> FMaterialExpressionBuildValue;
		auto FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FMaterialExpressionBuildValue;
		auto FunctionCall(const DMaterialExpressionFunctionCall& Call, FMaterialExpressionEmitter& Emitter) -> void;
		auto Emit(FMaterialIRNode Node) -> uint32;
		auto Literal(std::span<const float> Components) -> uint32;
		auto Parameter(FGuid Id, EMaterialParameterType Type) -> uint32;
		auto Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::span<const FMaterialExpressionInput* const> Inputs,
			std::span<const std::vector<float>* const> Defaults,
			std::span<const uint8> Swizzle = {}) -> uint32;
		auto Coordinates() -> uint32;
		auto Fail(FMaterialError Error, FGuid PortId = {},
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> uint32;
		auto GetNode(uint32 Index) const -> const FMaterialIRNode& { return Result.IR.Nodes.at(Index); }
		auto Finish(std::span<const FMaterialExpressionInput> Roots) -> FMaterialExpressionBuildResult;
		auto FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs) -> FMaterialExpressionBuildResult;
	private:
		friend class FMaterialExpressionEmitter;
		struct FSharedState
		{
			FMaterialExpressionBuildResult Result;
			std::vector<uint32> Depths;
			uint32 LinkCount = 0;
			uint64 ClosureBytes = 0;
			std::vector<const DMaterialFunctionInterface*> ActiveFunctions;
			std::map<const DMaterialFunctionInterface*, FMaterialExpressionFunctionBody> Functions;
			std::map<FGuid, ETextureUsage> TextureUsages;
		};
		FMaterialExpressionGraphBuilderImpl(FMaterialExpressionGraphBuilderImpl& Parent,
			const FMaterialExpressionFunctionBody& Body, FGuid CallId);
		auto Admit(std::span<DMaterialExpression* const> InExpressions) -> void;
		bool bValidateAuthoring = false;
		FXxHash128Builder AuthoringCodeHash;
		auto OpaqueAuthoringValue(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<uint32> Inputs = {}) -> uint32;
		auto ValidateAuthoringCall(const DMaterialExpressionFunctionCall& Call, FMaterialExpressionEmitter& Emitter) -> void;
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
		using FOutputKey = std::tuple<FGuid, uint8, FGuid>;
		std::map<FGuid, DMaterialExpression*> Expressions;
		std::map<FOutputKey, FMaterialExpressionBuildValue> Values;
		std::set<FGuid> Active;
		std::set<FGuid> Built;
		auto BuildExpression(const DMaterialExpression& Expression) -> void;
		auto ReportMissingOutput(const DMaterialExpression& Expression, const FMaterialExpressionInput& Input) -> void;
		std::vector<FGuid> SourceStack;
	};

}
