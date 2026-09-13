#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;
	namespace
	{
		struct FEditableInput
		{
			FMaterialProgramLink* Source = nullptr;
			FMaterialInputDefault* Default = nullptr;
		};

		auto FindInput(FMaterialGraphDocumentState& State, FGuid NodeId, uint32 Index, FGuid PortId) -> FEditableInput
		{
			auto* Node = FindNode(State.Program, NodeId);
			if (!Node) return {};
			if (PortId.IsValid())
			{
				auto Call = std::ranges::find(State.Calls, NodeId, &FMaterialFunctionCall::NodeId);
				if (Call == State.Calls.end() || !Call->Function.IsValid()) return {};
				const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
				const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end() || Port->Type > EMaterialProgramValueType::Float4) return {};
				auto Input = std::ranges::find(Call->Inputs, PortId, &FMaterialFunctionInputBinding::InputId);
				if (Input == Call->Inputs.end())
				{
					Call->Inputs.push_back({PortId, Port->Type});
					Input = std::prev(Call->Inputs.end());
				}
				return {&Input->Source, &Input->Default};
			}
			if (Index >= Node->Inputs.size() || IsMaterialSampleUVInput(*Node, Index)) return {};
			if (Node->Opcode == EMaterialProgramOpcode::TextureCoordinates)
				return {&Node->Inputs[Index], &const_cast<FMaterialInputDefault&>(GetMaterialUVSetting(Node->UVSettings, Index))};
			Node->InputDefaults.resize(Node->Inputs.size());
			return {&Node->Inputs[Index], &Node->InputDefaults[Index]};
		}

		auto HasConsumer(const FMaterialGraphDocumentState& State, FGuid Id) -> bool
		{
			for (const auto& Node : State.Program.Nodes)
			{
				for (const auto& Input : Node.Inputs) if (Input.SourceNodeId == Id) return true;
				for (const auto& Input : Node.SurfaceAttributes) if (Input.Source.SourceNodeId == Id) return true;
			}
			for (const auto& Call : State.Calls)
				for (const auto& Input : Call.Inputs) if (Input.Source.SourceNodeId == Id) return true;
			if (State.Program.Outputs.Surface.SourceNodeId == Id) return true;
			for (uint32 Index = 0; Index < 8; ++Index)
				if (GetMaterialSurfaceOutputLink(State.Program.Outputs, static_cast<EMaterialSurfaceOutput>(Index)).SourceNodeId == Id) return true;
			return false;
		}
	}

	auto FMaterialGraphDocument::SetInputDefault(const FGuid& NodeId, uint32 InputIndex,
		FMaterialInputDefault Value, FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.Default) return MakeRejected("This input does not support an inline numeric value.");
		*Input.Default = Value;
		return Commit(std::move(State), "Edit Input Default", Transactions);
	}

	auto FMaterialGraphDocument::ExtractInputDefault(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.Default || Input.Source->SourceNodeId.IsValid() || Input.Default->Kind == EMaterialInputDefaultKind::None)
			return MakeRejected("Only an unconnected explicit numeric binding can be extracted.");
		const auto Value = *Input.Default;
		FMaterialProgramNode Node{.Id = FGuid::NewGuid(), .Opcode = Value.Kind == EMaterialInputDefaultKind::Parameter
			? EMaterialProgramOpcode::Parameter : EMaterialProgramOpcode::Constant,
			.ResultType = Value.Type, .Literal = Value.Literal, .ParameterId = Value.ParameterId};
		*Input.Source = {Node.Id};
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return MakeRejected("The input owner has no authored position.");
		State.Presentation.Nodes.push_back({Node.Id, Position->X - 320, Position->Y});
		const auto Id = Node.Id;
		State.Program.Nodes.push_back(std::move(Node));
		auto Result = Commit(std::move(State), "Extract Input Node", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::InlineInputNode(const FGuid& NodeId, uint32 InputIndex,
		FGuid PortId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Input = FindInput(State, NodeId, InputIndex, PortId);
		if (!Input.Source || Input.Source->SourceOutputIndex != 0 || Input.Source->SourceOutputId.IsValid())
			return MakeRejected("This source cannot be represented by an inline binding.");
		const auto* Source = FindNode(State.Program, Input.Source->SourceNodeId);
		if (!Source || (Source->Opcode != EMaterialProgramOpcode::Constant && Source->Opcode != EMaterialProgramOpcode::Parameter))
			return MakeRejected("Only numeric constants and parameter references can be inlined.");
		const auto SourceId = Source->Id;
		*Input.Default = {.Kind = Source->Opcode == EMaterialProgramOpcode::Parameter ? EMaterialInputDefaultKind::Parameter
			: EMaterialInputDefaultKind::Literal, .Type = Source->ResultType, .Literal = Source->Literal, .ParameterId = Source->ParameterId};
		*Input.Source = {};
		if (!HasConsumer(State, SourceId)) std::erase_if(State.Program.Nodes, [&](const auto& Node) { return Node.Id == SourceId; });
		return Commit(std::move(State), "Inline Input Node", Transactions);
	}

	auto FMaterialGraphDocument::ExtractUVSettings(const FGuid& NodeId, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Sample = FindNode(State.Program, NodeId);
		if (!Sample || !IsMaterialSamplingNode(Sample->Opcode)) return MakeRejected("Select a texture sampling node.");
		const uint32 Index = Sample->Opcode == EMaterialProgramOpcode::TextureSample2D ? 1 : 0;
		if (Sample->Inputs.size() <= Index || Sample->Inputs[Index].SourceNodeId.IsValid())
			return MakeRejected("The sample already uses an explicit UV expression.");
		FMaterialProgramNode Coordinates{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::TextureCoordinates,
			.ResultType = EMaterialProgramValueType::Float2, .Inputs = {{}, {}, {}, {}}, .UVSettings = Sample->UVSettings};
		Sample->Inputs[Index] = {Coordinates.Id};
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return MakeRejected("The sample has no authored position.");
		State.Presentation.Nodes.push_back({Coordinates.Id, Position->X - 320, Position->Y});
		const auto Id = Coordinates.Id;
		State.Program.Nodes.push_back(std::move(Coordinates));
		auto Result = Commit(std::move(State), "Extract Texture Coordinates", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}
}
