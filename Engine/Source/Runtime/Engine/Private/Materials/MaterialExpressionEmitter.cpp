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

	auto FEmitter::Literal(std::span<const float> Components,
		EMaterialSpatialKind Kind, EMaterialCoordinateSpace Space) -> uint32
	{
		return Builder.Literal(Components, Kind, Space);
	}

	auto FEmitter::Parameter(FGuid Id, EMaterialParameterType Type) -> uint32
	{
		return Builder.Parameter(Id, Type);
	}

	auto FEmitter::CollectionParameter(
		const DMaterialParameterCollection& Collection, FGuid ParameterId) -> uint32
	{
		return Builder.CollectionParameter(Collection, ParameterId);
	}

	auto FEmitter::Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialNumericInput* const> Inputs,
		std::span<const uint8> Swizzle, EMaterialSpatialKind OutputKind,
		EMaterialCoordinateSpace OutputSpace) -> uint32
	{
		return Builder.Numeric(Opcode, Type, Inputs, Swizzle, OutputKind, OutputSpace);
	}

	auto FEmitter::Transform(EMaterialProgramOpcode Opcode,
		const FMaterialNumericInput& Input, EMaterialCoordinateSpace Source,
		EMaterialCoordinateSpace Destination) -> uint32
	{
		return Builder.Transform(Opcode, Input, Source, Destination);
	}

	auto FEmitter::StaticBool(FGuid DeclarationId, bool DefaultValue) -> FValue
	{
		return Builder.StaticBool(DeclarationId, DefaultValue);
	}

	auto FEmitter::StaticSwitch(const FMaterialExpressionInput& Condition,
		EMaterialProgramValueType Type, const FMaterialNumericInput& FalseValue,
		const FMaterialNumericInput& TrueValue) -> FValue
	{
		return Builder.StaticSwitch(Condition, Type, FalseValue, TrueValue);
	}

	auto FEmitter::QualitySwitch(EMaterialProgramValueType Type,
		const FMaterialNumericInput& DefaultValue, const FMaterialExpressionInput& Low,
		const FMaterialExpressionInput& High) -> FValue
	{
		return Builder.QualitySwitch(Type, DefaultValue, Low, High);
	}

	auto FEmitter::FeatureLevelSwitch(EMaterialProgramValueType Type,
		const FMaterialNumericInput& DefaultValue, const FMaterialExpressionInput& ES3_1,
		const FMaterialExpressionInput& SM5, const FMaterialExpressionInput& SM6) -> FValue
	{
		return Builder.FeatureLevelSwitch(Type, DefaultValue, ES3_1, SM5, SM6);
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
