#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphEditSession.h"
#include "MaterialExpressionInputs.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;
	namespace
	{
		struct FEditableInput
		{
			FMaterialExpressionInput* Source = nullptr;
			std::vector<float>* Default = nullptr;
			FMaterialExpressionSurfaceOutputs* MaterialOutputs = nullptr;
			EMaterialOutputPin OutputPin = EMaterialOutputPin::Surface;

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
			auto* Expression = FindExpression(State, NodeId);
			if (!Expression) return {};
			State.Modify(*Expression);
			if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(Expression))
			{
				if (PortId.IsValid() || Index > static_cast<uint32>(EMaterialOutputPin::Surface)) return {};
				return {.Source = GetMaterialOutputInput(Output->Outputs, static_cast<EMaterialOutputPin>(Index)),
					.MaterialOutputs = &Output->Outputs, .OutputPin = static_cast<EMaterialOutputPin>(Index)};
			}
			if (PortId.IsValid())
			{
				auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
				if (!Call || !Call->Function.IsValid()) return {};
				const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
				const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end() || Port->Type > EMaterialProgramValueType::Float4) return {};
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
			if (!Result.Source || Cast<DMaterialExpressionFunctionCall>(Expression)) return {};
			// Concrete numeric families pair each connection with its named float-vector default.
			Expression->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (Property->GetValuePtr(Expression) != Result.Source) return;
				auto* Default = Expression->GetClass()->FindPropertyByName(FName(Property->NamePrivate.ToString() + "Default"));
				if (!Default || Default->GetKind() != DurinCodeGen::EPropertyGenFlags::Array) return;
				if (static_cast<FArrayProperty*>(Default)->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Float) return;
				Result.Default = static_cast<std::vector<float>*>(Default->GetValuePtr(Expression));
			});
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
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.SupportsDefault()) return MakeRejected("This input does not support an inline numeric value.");
		std::vector<float> Components;
		if (Value.Kind == EMaterialInputDefaultKind::Literal && Value.Type <= EMaterialProgramValueType::Float4)
		{
			const std::array Values{Value.Literal.X, Value.Literal.Y, Value.Literal.Z, Value.Literal.W};
			Components.assign(Values.begin(), Values.begin() + static_cast<uint32>(Value.Type) + 1);
		}
		else if (Value.Kind != EMaterialInputDefaultKind::None) return MakeRejected("The input default is not numeric.");
		if (Input.Read() == Components) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (!Input.Write(Components)) return MakeRejected("The input default has an incompatible width.");
		return CommitGraphEdit(*Owner.Get(), State, "Edit Input Default", Transactions);
	}

	auto FMaterialGraphDocument::ExtractInputDefault(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.SupportsDefault() || Input.Source->ExpressionId.IsValid() || Input.Read().empty())
			return MakeRejected("Only an unconnected explicit numeric binding can be extracted.");
		auto Constant = MakeConstant(Input.Read());
		if (!Constant) return MakeRejected("The input default has an unsupported width.");
		*Input.Source = {Constant->Id};
		if (Input.MaterialOutputs) Input.MaterialOutputs->Surface = {};
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return MakeRejected("The input owner has no authored position.");
		State.Presentation.Nodes.push_back({Constant->Id, Position->X - 320, Position->Y});
		const auto Id = Constant->Id;
		State.Expressions.emplace_back(Constant.Get());
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Extract Input Node", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::InlineInputNode(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.SupportsDefault() || Input.Source->OutputIndex != 0 || Input.Source->OutputId.IsValid())
			return MakeRejected("This source cannot be represented by an inline binding.");
		const auto Value = ReadConstant(FindExpression(State, Input.Source->ExpressionId));
		if (Value.empty()) return MakeRejected("Only numeric constants can be inlined; parameter exposure remains an explicit node.");
		const auto SourceId = Input.Source->ExpressionId;
		if (!Input.Write(Value)) return MakeRejected("The constant has an incompatible width.");
		*Input.Source = {};
		if (!HasConsumer(State, SourceId))
		{
			std::erase_if(State.Expressions, [&](const auto& Expression) { return Expression->Id == SourceId; });
			std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Position.NodeId == SourceId; });
		}
		return CommitGraphEdit(*Owner.Get(), State, "Inline Input Node", Transactions);
	}

}
