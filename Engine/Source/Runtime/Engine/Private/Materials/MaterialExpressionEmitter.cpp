#include "MaterialExpressionGraphBuilder.h"

namespace Durin
{
	auto FMaterialExpressionEmitter::RegisterOutput(uint8 Index, FGuid Id, FMaterialExpressionBuildValue Value) -> void
	{
		if (!Builder.Result.Diagnostics.empty()) return;
		if (const auto* Node = Value.GetIndex(); Node && *Node >= Builder.Result.IR.Nodes.size())
			return Fail(EMaterialExpressionError::BuildReturnedInvalidIRIndex, Id);
		if (!Builder.Values.emplace(FMaterialExpressionGraphBuilderImpl::FOutputKey{ExpressionId, Index, Id}, Value).second)
			Fail(EMaterialExpressionError::BuildOutputAlreadyRegistered, Id);
	}

	auto FMaterialExpressionEmitter::Output(uint8 Index, FMaterialExpressionBuildValue Value) -> void
	{
		RegisterOutput(Index, {}, Value);
	}

	auto FMaterialExpressionEmitter::Output(FGuid Id, FMaterialExpressionBuildValue Value) -> void
	{
		if (!Id.IsValid()) return Fail(EMaterialFunctionError::CallOutputRequiresUniqueValidTypedPort);
		RegisterOutput(0, Id, Value);
	}

	auto FMaterialExpressionEmitter::Resolve(const FMaterialExpressionInput& Input) -> FMaterialExpressionBuildValue
	{
		return Builder.Resolve(Input);
	}

	auto FMaterialExpressionEmitter::ResolveIndex(const FMaterialExpressionInput& Input) -> uint32
	{
		return Builder.ResolveIndex(Input);
	}

	auto FMaterialExpressionEmitter::FunctionInput(FGuid PortId) -> FMaterialExpressionBuildValue
	{
		return Builder.FunctionInput(PortId);
	}

	auto FMaterialExpressionEmitter::FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FMaterialExpressionBuildValue
	{
		return Builder.FunctionOutput(PortId, Source);
	}

	auto FMaterialExpressionEmitter::FunctionCall(const DMaterialExpressionFunctionCall& Call) -> void
	{
		Builder.FunctionCall(Call, *this);
	}

	auto FMaterialExpressionEmitter::Emit(FMaterialIRNode Node) -> uint32
	{
		return Builder.Emit(std::move(Node));
	}

	auto FMaterialExpressionEmitter::Literal(std::span<const float> Components) -> uint32
	{
		return Builder.Literal(Components);
	}

	auto FMaterialExpressionEmitter::Parameter(FGuid Id, EMaterialParameterType Type) -> uint32
	{
		return Builder.Parameter(Id, Type);
	}

	auto FMaterialExpressionEmitter::Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialExpressionInput* const> Inputs,
		std::span<const std::vector<float>* const> Defaults, std::span<const uint8> Swizzle) -> uint32
	{
		return Builder.Numeric(Opcode, Type, Inputs, Defaults, Swizzle);
	}

	auto FMaterialExpressionEmitter::Coordinates() -> uint32
	{
		return Builder.Coordinates();
	}

	auto FMaterialExpressionEmitter::IsNormalTexture(FMaterialExpressionBuildValue Value) const -> bool
	{
		if (const auto* Default = Value.GetTexture()) return Default->Fallback == EMaterialTextureFallback::FlatRGNormal;
		const auto Index = *Value.GetIndex();
		if (Index >= Builder.Result.IR.Nodes.size()) return false;
		const auto& Node = GetNode(Index);
		if (Node.Opcode != EMaterialProgramOpcode::TextureParameter) return false;
		const auto Usage = Builder.Shared->TextureUsages.find(Node.GetParameterId());
		return Usage != Builder.Shared->TextureUsages.end() && Usage->second == ETextureUsage::Normal;
	}

	auto FMaterialExpressionEmitter::Fail(FMaterialError Error, FGuid PortId, EMaterialProgramDiagnosticCategory Category) -> void
	{
		Builder.Fail(std::move(Error), PortId, Category);
	}

	auto FMaterialExpressionEmitter::GetNode(uint32 Index) const -> const FMaterialIRNode&
	{
		return Builder.GetNode(Index);
	}

}
