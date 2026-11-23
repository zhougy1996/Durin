#include "MaterialExpressionGraphBuilder.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"

namespace Durin::MIR
{
	auto FEmitter::IsCompilingMaterial() const -> bool { return Builder.bCompileReachableOnly; }

	auto FEmitter::SetCustomOutputs(std::string_view Name, std::span<const std::optional<FValue>> Args,
		EMaterialEvaluationStage Frequency) -> void
	{
		if (!Builder.Result.Diagnostics.empty()) return;
		const auto Families = GetMaterialCustomOutputs();
		const auto Family = std::ranges::find(Families, Name, &FMaterialCustomOutputDefinition::FunctionName);
		if (Family == Families.end() || Family->Outputs.size() != Args.size())
			return Fail(std::format("Unknown custom output group or invalid arity for {}.", Name));
		auto& IR = Builder.Result.IR;
		for (size_t Index = 0; Index < Args.size(); ++Index)
		{
			const auto& Slot = Family->Outputs[Index];
			if (Slot.Semantics.Stages != Frequency)
				return Fail(std::format("{} has the wrong output frequency.", Name));
			if (std::ranges::contains(IR.CustomOutputRoots, Slot.Id, &FModule::FAttributeRoot::AttributeID))
				return Fail(std::format("Duplicate custom output {}.", Name));
			auto& Root = IR.GetCustomOutput(Slot.Id);
			if (!Args[Index]) continue;
			const auto* Value = Args[Index]->GetIndex();
			if (!Value || *Value >= IR.Nodes.size()) return Fail("Custom output requires an emitted numeric value.");
			const auto Actual = GetNode(*Value).GetSemantics();
			if (Actual.Type != Slot.Semantics.Type || Actual.Kind != Slot.Semantics.Kind || Actual.Space != Slot.Semantics.Space
				|| !MaterialStagesContain(Actual.Stages, Frequency))
				return Fail(std::format("{} requires {}.", Slot.Name, GetMaterialValueSemanticsName(Slot.Semantics)));
			Root.bExpression = true;
			Root.ExpressionIndex = *Value;
		}
	}

	auto FEmitter::Output(int32 Index, FValue Value) -> void
	{
		if (!Builder.Result.Diagnostics.empty()) return;
		if (Index < 0) return Fail("Expression Build registered a negative output index.");
		if (const auto* Node = Value.GetIndex(); Node && *Node >= Builder.Result.IR.Nodes.size())
			return Fail("Expression Build registered an invalid IR index.");
		if (!Builder.Values.emplace(FGraphBuilderImpl::FOutputKey{ExpressionId, Index}, Value).second)
			Fail("Expression Build registered an output more than once.");
	}

	auto FEmitter::Resolve(const FMaterialExpressionInput& Input) -> FValue
	{
		return Builder.Resolve(Input);
	}

	auto FEmitter::ResolveIndex(const FMaterialExpressionInput& Input) -> uint32
	{
		return Builder.ResolveIndex(Input);
	}

	auto FEmitter::ValidCast(uint32 Code, EMaterialProgramValueType DestType) -> uint32
	{
		return Builder.ValidCast(Code, DestType);
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

	auto FEmitter::Aggregate(uint32 Prototype, const FAttributeAssignment& Assignment) -> uint32
	{
		return Builder.Aggregate(Prototype, Assignment);
	}

	auto FEmitter::Attribute(uint32 Aggregate, EMaterialProperty Property) -> uint32
	{
		return Builder.Attribute(Aggregate, Property);
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

	auto FEmitter::Parameter(FGuid Id, EMaterialParameterType Type, EMaterialTextureDimension Dimension) -> uint32
	{
		return Builder.Parameter(Id, Type, Dimension);
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

	auto FEmitter::Fail(std::string Message, FGuid PortId) -> void
	{
		Builder.Fail(std::move(Message), PortId);
	}

	auto FEmitter::GetNode(uint32 Index) const -> const FNode&
	{
		return Builder.GetNode(Index);
	}

}
