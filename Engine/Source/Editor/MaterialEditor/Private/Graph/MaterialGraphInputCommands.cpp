#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphEditSession.h"
#include "MaterialExpressionInputs.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;
	auto FormatMaterialGraphInputError(const FMaterialGraphInputError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EMaterialGraphInputError::None: return {};
		case EMaterialGraphInputError::MissingNode: return "The input owner is unavailable.";
		case EMaterialGraphInputError::OutputAddress: return "The material output input address is invalid.";
		case EMaterialGraphInputError::FunctionUnavailable: return "The function input owner is unavailable.";
		case EMaterialGraphInputError::PortMissing: return "The function input port is unavailable.";
		case EMaterialGraphInputError::PortType: return "The function input port is not numeric.";
		case EMaterialGraphInputError::InputMissing: return "The expression input is unavailable.";
		case EMaterialGraphInputError::FunctionAddress: return "A function input requires a port identity.";
		case EMaterialGraphInputError::UnsupportedDefault: return "This input does not support an inline numeric value.";
		case EMaterialGraphInputError::NonNumeric: return "The input default is not numeric.";
		case EMaterialGraphInputError::DefaultWidth: return "The input default has an incompatible width.";
		case EMaterialGraphInputError::ExtractBinding: return "Only an unconnected explicit numeric binding can be extracted.";
		case EMaterialGraphInputError::ConstantWidth: return "The input default has an unsupported width.";
		case EMaterialGraphInputError::PositionMissing: return "The input owner has no authored position.";
		case EMaterialGraphInputError::InlineSource: return "This source cannot be represented by an inline binding.";
		case EMaterialGraphInputError::NonConstant: return "Only numeric constants can be inlined; parameter exposure remains an explicit node.";
		case EMaterialGraphInputError::InlineWidth: return "The constant has an incompatible width.";
		}
		return {};
	}

	namespace
	{
		auto RejectInput(FMaterialGraphInputError Error) -> FMaterialGraphCommandResult
		{
			FMaterialGraphCommandResult Result;
			Result.InputCause = std::move(Error);
			return Result;
		}
		struct FEditableInput
		{
			FMaterialExpressionInput* Source = nullptr;
			std::vector<float>* Default = nullptr;
			FMaterialExpressionSurfaceOutputs* MaterialOutputs = nullptr;
			EMaterialOutputPin OutputPin = EMaterialOutputPin::Surface;
			FMaterialGraphInputError Error;

			auto SupportsDefault() const -> bool { return Default != nullptr || (MaterialOutputs && OutputPin != EMaterialOutputPin::Surface); }
			auto Read() const -> std::vector<float> { return MaterialOutputs ? ReadMaterialOutputDefault(*MaterialOutputs, OutputPin) : Default ? *Default : std::vector<float>{}; }
			auto Write(const std::vector<float>& Value) const -> bool
			{
				if (MaterialOutputs) return WriteMaterialOutputDefault(*MaterialOutputs, OutputPin, Value);
				if (!Default) return false;
				*Default = Value;
				return true;
			}
		};

		auto FindExpression(FGraphEditSession& State, FGuid Id) -> DMaterialExpression*
		{
			const auto It = std::ranges::find(State.Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			return It == State.Expressions.end() ? nullptr : It->Get();
		}

		auto FindInput(FGraphEditSession& State, FGuid NodeId, uint32 Index, FGuid PortId) -> FEditableInput
		{
			const auto Fail = [&](EMaterialGraphInputError Code) -> FEditableInput {
				return {.Error = {.Code = Code, .NodeId = NodeId, .PortId = PortId, .InputIndex = Index}};
			};
			auto* Expression = FindExpression(State, NodeId);
			if (!Expression) return Fail(EMaterialGraphInputError::MissingNode);
			State.Modify(*Expression);
			if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(Expression))
			{
				if (PortId.IsValid() || Index > static_cast<uint32>(EMaterialOutputPin::Surface)) return Fail(EMaterialGraphInputError::OutputAddress);
				return {.Source = GetMaterialOutputInput(Output->Outputs, static_cast<EMaterialOutputPin>(Index)),
					.MaterialOutputs = &Output->Outputs, .OutputPin = static_cast<EMaterialOutputPin>(Index)};
			}
			if (PortId.IsValid())
			{
				auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
				if (!Call || !Call->Function.IsValid()) return Fail(EMaterialGraphInputError::FunctionUnavailable);
				const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
				const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end()) return Fail(EMaterialGraphInputError::PortMissing);
				if (Port->Type > EMaterialProgramValueType::Float4)
				{
					auto Result = Fail(EMaterialGraphInputError::PortType);
					Result.Error.Type = Port->Type;
					return Result;
				}
				auto Input = std::ranges::find(Call->Inputs, PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				if (Input == Call->Inputs.end())
				{
					Call->Inputs.push_back({PortId, Port->Type});
					Input = std::prev(Call->Inputs.end());
				}
				return {&Input->Input, &Input->InputDefault};
			}
			FEditableInput Result;
			VisitMaterialExpressionInputs(*Expression, [&](uint32 Pin, FMaterialExpressionInput& Input) {
				if (Pin == Index) Result.Source = &Input;
			});
			if (!Result.Source) return Fail(EMaterialGraphInputError::InputMissing);
			if (Cast<DMaterialExpressionFunctionCall>(Expression)) return Fail(EMaterialGraphInputError::FunctionAddress);
			Result.Default = FindMaterialExpressionInputDefault(*Expression, *Result.Source);
			return Result;
		}

		auto HasConsumer(FGraphEditSession& State, FGuid Id) -> bool
		{
			bool bFound = false;
			for (auto& Expression : State.Expressions)
				VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) { bFound |= Input.ExpressionId == Id; });
			return bFound;
		}

		auto MakeConstant(std::span<const float> Value) -> TStrongObjectPtr<DMaterialExpression>
		{
			TStrongObjectPtr<DMaterialExpression> Result;
			if (Value.size() == 1) { auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); E->Value = Value[0]; Result = E; }
			if (Value.size() == 2) { auto* E = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); E->Value = FVector2(Value[0], Value[1]); Result = E; }
			if (Value.size() == 3) { auto* E = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None); E->Value = FVector3(Value[0], Value[1], Value[2]); Result = E; }
			if (Value.size() == 4) { auto* E = NewObject<DMaterialExpressionVector4Constant>(nullptr, NAME_None); E->Value = FVector4(Value[0], Value[1], Value[2], Value[3]); Result = E; }
			if (Result) Result->Id = FGuid::NewGuid();
			return Result;
		}

		auto ReadConstant(const DMaterialExpression* Source) -> std::vector<float>
		{
			if (const auto* E = Cast<DMaterialExpressionScalarConstant>(Source)) return {E->Value};
			if (const auto* E = Cast<DMaterialExpressionVector2Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y)};
			if (const auto* E = Cast<DMaterialExpressionVector3Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z)};
			if (const auto* E = Cast<DMaterialExpressionVector4Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z), static_cast<float>(E->Value.w)};
			return {};
		}
	}

	auto FMaterialGraphDocument::SetInputDefault(const FGuid& NodeId, uint32 InputIndex,
		FMaterialInputDefault Value, FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner});
		FGraphEditSession State(*Owner.Get());
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (Input.Error.Code != EMaterialGraphInputError::None) return RejectInput(Input.Error);
		if (!Input.SupportsDefault()) return RejectInput({.Code = EMaterialGraphInputError::UnsupportedDefault, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex});
		std::vector<float> Components;
		if (Value.Kind == EMaterialInputDefaultKind::Literal && Value.Type <= EMaterialProgramValueType::Float4)
		{
			const std::array Values{Value.Literal.X, Value.Literal.Y, Value.Literal.Z, Value.Literal.W};
			Components.assign(Values.begin(), Values.begin() + static_cast<uint32>(Value.Type) + 1);
		}
		else if (Value.Kind != EMaterialInputDefaultKind::None) return RejectInput({.Code = EMaterialGraphInputError::NonNumeric, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex, .Kind = Value.Kind, .Type = Value.Type});
		if (Input.Read() == Components) return {.Disposition = EMaterialGraphCommandDisposition::NoChange};
		if (!Input.Write(Components)) return RejectInput({.Code = EMaterialGraphInputError::DefaultWidth, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex, .Width = Components.size()});
		return State.Commit("Edit Input Default", Transactions);
	}

	auto FMaterialGraphDocument::ExtractInputDefault(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner});
		FGraphEditSession State(*Owner.Get());
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (Input.Error.Code != EMaterialGraphInputError::None) return RejectInput(Input.Error);
		if (!Input.SupportsDefault() || Input.Source->ExpressionId.IsValid() || Input.Read().empty())
			return RejectInput({.Code = EMaterialGraphInputError::ExtractBinding, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex});
		auto Constant = MakeConstant(Input.Read());
		if (!Constant) return RejectInput({.Code = EMaterialGraphInputError::ConstantWidth, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex, .Width = Input.Read().size()});
		*Input.Source = {Constant->Id};
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return RejectInput({.Code = EMaterialGraphInputError::PositionMissing, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex});
		State.Presentation.Nodes.push_back({Constant->Id, Position->X - 320, Position->Y});
		const auto Id = Constant->Id;
		State.Expressions.emplace_back(Constant.Get());
		auto Result = State.Commit("Extract Input Node", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::InlineInputNode(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner});
		FGraphEditSession State(*Owner.Get());
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (Input.Error.Code != EMaterialGraphInputError::None) return RejectInput(Input.Error);
		if (!Input.SupportsDefault() || Input.Source->OutputIndex != 0 || Input.Source->OutputId.IsValid())
			return RejectInput({.Code = EMaterialGraphInputError::InlineSource, .NodeId = NodeId, .PortId = PortId, .InputIndex = InputIndex});
		const auto Value = ReadConstant(FindExpression(State, Input.Source->ExpressionId));
		if (Value.empty()) return RejectInput({.Code = EMaterialGraphInputError::NonConstant, .NodeId = NodeId, .PortId = PortId, .SourceId = Input.Source->ExpressionId, .InputIndex = InputIndex});
		const auto SourceId = Input.Source->ExpressionId;
		if (!Input.Write(Value)) return RejectInput({.Code = EMaterialGraphInputError::InlineWidth, .NodeId = NodeId, .PortId = PortId, .SourceId = SourceId, .InputIndex = InputIndex, .Width = Value.size()});
		*Input.Source = {};
		if (!HasConsumer(State, SourceId))
		{
			std::erase_if(State.Expressions, [&](const auto& Expression) { return Expression->Id == SourceId; });
			std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Position.NodeId == SourceId; });
		}
		return State.Commit("Inline Input Node", Transactions);
	}

}
