#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphExpressionState.h"
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
			FMaterialScalarExpressionDefault* Scalar = nullptr;
			FMaterialVector2ExpressionDefault* Vector2 = nullptr;

			auto SupportsDefault() const -> bool { return Default || Scalar || Vector2; }
			auto Read() const -> std::vector<float>
			{
				if (Default) return *Default;
				if (Scalar && Scalar->bPresent) return {Scalar->Value};
				if (Vector2 && Vector2->bPresent) return {static_cast<float>(Vector2->Value.x), static_cast<float>(Vector2->Value.y)};
				return {};
			}
			auto Write(const std::vector<float>& Value) const -> bool
			{
				if (Default) *Default = Value;
				else if (Scalar)
				{
					if (Value.size() > 1) return false;
					Scalar->bPresent = !Value.empty();
					Scalar->Value = Value.empty() ? 0.f : Value[0];
				}
				else if (Vector2)
				{
					if (!Value.empty() && Value.size() != 2) return false;
					Vector2->bPresent = !Value.empty();
					Vector2->Value = Value.empty() ? FVector2(0.0) : FVector2(Value[0], Value[1]);
				}
				else return false;
				return true;
			}
		};

		auto FindExpression(FOwnedGraphSnapshot& State, FGuid Id) -> DMaterialExpression*
		{
			const auto It = std::ranges::find(State.Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			return It == State.Expressions.end() ? nullptr : It->Get();
		}

		auto FindInput(FOwnedGraphSnapshot& State, FGuid NodeId, uint32 Index, FGuid PortId) -> FEditableInput
		{
			auto* Expression = FindExpression(State, NodeId);
			if (!Expression) return {};
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
			if (auto* Coordinates = Cast<DMaterialExpressionTextureCoordinates>(Expression))
			{
				if (Index == 0) Result.Scalar = &Coordinates->Defaults.Channel;
				if (Index == 1) Result.Vector2 = &Coordinates->Defaults.Scale;
				if (Index == 2) Result.Vector2 = &Coordinates->Defaults.Offset;
				if (Index == 3) Result.Scalar = &Coordinates->Defaults.Rotation;
				return Result;
			}
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

		auto HasConsumer(FOwnedGraphSnapshot& State, FGuid Id) -> bool
		{
			bool bFound = false;
			for (auto& Expression : State.Expressions)
				VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) { bFound |= Input.ExpressionId == Id; });
			for (const auto* Output : {&State.Outputs.Surface, &State.Outputs.BaseColor, &State.Outputs.Normal, &State.Outputs.Metallic,
				&State.Outputs.Roughness, &State.Outputs.AmbientOcclusion, &State.Outputs.Emissive, &State.Outputs.Opacity, &State.Outputs.OpacityMask})
				bFound |= Output->ExpressionId == Id;
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
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
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
		return CommitOwnedExpressions(*Owner.Get(), std::move(State), "Edit Input Default", Transactions);
	}

	auto FMaterialGraphDocument::ExtractInputDefault(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.SupportsDefault() || Input.Source->ExpressionId.IsValid() || Input.Read().empty())
			return MakeRejected("Only an unconnected explicit numeric binding can be extracted.");
		auto Constant = MakeConstant(Input.Read());
		if (!Constant) return MakeRejected("The input default has an unsupported width.");
		*Input.Source = {Constant->Id};
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return MakeRejected("The input owner has no authored position.");
		State.Presentation.Nodes.push_back({Constant->Id, Position->X - 320, Position->Y});
		const auto Id = Constant->Id;
		State.Expressions.push_back(std::move(Constant));
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Extract Input Node", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::InlineInputNode(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
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
		return CommitOwnedExpressions(*Owner.Get(), std::move(State), "Inline Input Node", Transactions);
	}

	auto FMaterialGraphDocument::ExtractUVSettings(const FGuid& NodeId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Sample = FindExpression(State, NodeId);
		FMaterialExpressionInput* UV = nullptr;
		FMaterialExpressionUVSettings* Settings = nullptr;
		if (auto* E = Cast<DMaterialExpressionTextureSample2D>(Sample)) { UV = &E->UV; Settings = &E->UVSettings; }
		if (auto* E = Cast<DMaterialExpressionTextureSampleParameter2D>(Sample)) { UV = &E->UV; Settings = &E->UVSettings; }
		if (!UV) return MakeRejected("Select a texture sampling node.");
		if (UV->ExpressionId.IsValid()) return MakeRejected("The sample already uses an explicit UV expression.");
		TStrongObjectPtr<DMaterialExpressionTextureCoordinates> Coordinates(NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None));
		Coordinates->Id = FGuid::NewGuid();
		Coordinates->Defaults = *Settings;
		*UV = {Coordinates->Id};
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return MakeRejected("The sample has no authored position.");
		State.Presentation.Nodes.push_back({Coordinates->Id, Position->X - 320, Position->Y});
		const auto Id = Coordinates->Id;
		State.Expressions.emplace_back(Coordinates.Get());
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Extract Texture Coordinates", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}
}
