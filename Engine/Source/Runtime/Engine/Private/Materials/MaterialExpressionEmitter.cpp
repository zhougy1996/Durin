#include "MaterialExpressionGraphBuilder.h"

namespace Durin::MIR
{
	auto FEmitter::RegisterOutput(uint8 Index, FGuid Id, FValue Value) -> void
	{
		if (!Builder.Result.Diagnostics.empty()) return;
		if (const auto* Node = Value.GetIndex(); Node && *Node >= Builder.Result.IR.Nodes.size())
			return Fail(EMaterialExpressionError::BuildReturnedInvalidIRIndex, Id);
		if (!Builder.Values.emplace(FGraphBuilderImpl::FOutputKey{ExpressionId, Index, Id}, Value).second)
			Fail(EMaterialExpressionError::BuildOutputAlreadyRegistered, Id);
	}

	auto FEmitter::Output(uint8 Index, FValue Value) -> void
	{
		RegisterOutput(Index, {}, Value);
	}

	auto FEmitter::Output(FGuid Id, FValue Value) -> void
	{
		if (!Id.IsValid()) return Fail(EMaterialFunctionError::CallOutputRequiresUniqueValidTypedPort);
		RegisterOutput(0, Id, Value);
	}

	auto FEmitter::Resolve(const FMaterialExpressionInput& Input) -> FValue
	{
		return Builder.Resolve(Input);
	}

	auto FEmitter::ResolveIndex(const FMaterialExpressionInput& Input) -> uint32
	{
		return Builder.ResolveIndex(Input);
	}

	auto FEmitter::FunctionInput(FGuid PortId) -> FValue
	{
		return Builder.FunctionInput(PortId);
	}

	auto FEmitter::FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FValue
	{
		return Builder.FunctionOutput(PortId, Source);
	}

	auto FEmitter::FunctionCall(const DMaterialExpressionFunctionCall& Call) -> void
	{
		Builder.FunctionCall(Call, *this);
	}

	auto FEmitter::Emit(FNode Node) -> uint32
	{
		return Builder.Emit(std::move(Node));
	}

	auto FEmitter::Literal(std::span<const float> Components) -> uint32
	{
		return Builder.Literal(Components);
	}

	auto FEmitter::Parameter(FGuid Id, EMaterialParameterType Type) -> uint32
	{
		return Builder.Parameter(Id, Type);
	}

	auto FEmitter::Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialExpressionInput* const> Inputs,
		std::span<const std::vector<float>* const> Defaults, std::span<const uint8> Swizzle) -> uint32
	{
		return Builder.Numeric(Opcode, Type, Inputs, Defaults, Swizzle);
	}

	auto FEmitter::Coordinates() -> uint32
	{
		return Builder.Coordinates();
	}

	auto FEmitter::IsNormalTexture(FValue Value) const -> bool
	{
		if (const auto* Default = Value.GetTexture()) return Default->Fallback == EMaterialTextureFallback::FlatRGNormal;
		const auto Index = *Value.GetIndex();
		if (Index >= Builder.Result.IR.Nodes.size()) return false;
		const auto& Node = GetNode(Index);
		if (Node.Opcode != EMaterialProgramOpcode::TextureParameter) return false;
		const auto Usage = Builder.Shared->TextureUsages.find(Node.GetParameterId());
		return Usage != Builder.Shared->TextureUsages.end() && Usage->second == ETextureUsage::Normal;
	}

	auto FEmitter::Fail(FMaterialError Error, FGuid PortId, EMaterialProgramDiagnosticCategory Category) -> void
	{
		Builder.Fail(std::move(Error), PortId, Category);
	}

	auto FEmitter::GetNode(uint32 Index) const -> const FNode&
	{
		return Builder.GetNode(Index);
	}

}
