#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphClipboardReferences.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	auto FMaterialGraphOperations::CopySelection(
		const DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload)
		-> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(const_cast<DMaterial&>(Material)).CopySelection(NodeIds, OutPayload);
	}

	auto FMaterialGraphDocument::CopySelection(std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload) const -> FMaterialGraphCommandResult
	{
		OutPayload = {};
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph copy selection is empty or exceeds the node bound.");
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size())
			return MakeRejected("The material graph copy selection contains duplicate node GUIDs.");
		const FMaterialProgram& Program = State.Program;
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				State.Presentation, Program);
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			Positions.emplace(Position.NodeId, Position);
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit;
		int32 MinimumY = MaterialGraphPresentationCoordinateLimit;
		for (const FGuid& Id : Selected)
		{
			if (!FindNode(Program, Id))
				return MakeRejected("A copied material graph node does not exist.");
			const auto It = Positions.find(Id);
			if (It == Positions.end())
				return MakeRejected("A copied material graph node has no authored position.");
			MinimumX = std::min(MinimumX, It->second.X);
			MinimumY = std::min(MinimumY, It->second.Y);
		}
		std::vector<FGuid> Ordered(Selected.begin(), Selected.end());
		std::ranges::sort(Ordered);
		OutPayload.SourceRoot = Owner.Get();
		OutPayload.Nodes.reserve(Ordered.size());
		for (const FGuid& Id : Ordered)
		{
			FMaterialProgramNode Node = *FindNode(Program, Id);
			if (Node.Opcode == EMaterialProgramOpcode::FunctionInput || Node.Opcode == EMaterialProgramOpcode::FunctionOutput)
			{
				const bool bOutput = Node.Opcode == EMaterialProgramOpcode::FunctionOutput;
				const auto& Ports = bOutput ? State.Signature.Outputs : State.Signature.Inputs;
				const auto Port = std::ranges::find(Ports, Node.FunctionPortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end()) { OutPayload = {}; return MakeRejected("A copied function port is unavailable."); }
				(bOutput ? OutPayload.Signature.Outputs : OutPayload.Signature.Inputs).push_back(*Port);
			}
			const FMaterialGraphNodePresentation& Position = Positions.at(Id);
			OutPayload.Nodes.push_back({
				.Node = std::move(Node),
				.RelativeX = Position.X - MinimumX,
				.RelativeY = Position.Y - MinimumY,
			});
		}
		for (const auto& Call : State.Calls)
			if (Selected.contains(Call.NodeId))
			{
				OutPayload.Calls.push_back(Call);
			}
		if (Selected.contains(Program.Outputs.Surface.SourceNodeId))
		{
			OutPayload.bConnectAggregateSurface = true;
			OutPayload.AggregateSourceNodeId = Program.Outputs.Surface.SourceNodeId;
			OutPayload.AggregateSourceOutputIndex = Program.Outputs.Surface.SourceOutputIndex;
			OutPayload.AggregateSourceOutputId = Program.Outputs.Surface.SourceOutputId;
		}
		if (!OutPayload.Nodes.empty())
		{
			auto* References = NewObject<DMaterialGraphClipboardReferences>(DMaterialGraphClipboardReferences::StaticClass(), nullptr,
				NAME_None, EObjectFlags::Transient);
			for (const auto& Call : OutPayload.Calls) References->Functions.emplace_back(Call.Function.Get());
			for (const auto& Entry : OutPayload.Nodes) References->Textures.emplace_back(Entry.Node.Parameter.Value.TextureValue.Get());
			OutPayload.RetainedReferences = FStrongObjectPtr(References);
		}
		return {
			.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Ordered),
		};
	}

	auto FMaterialGraphOperations::Paste(
		DMaterial& Material,
		const FMaterialGraphClipboardPayload& Payload,
		int32 X,
		int32 Y,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).Paste(Payload, X, Y, Transactions);
	}

	auto FMaterialGraphDocument::Paste(const FMaterialGraphClipboardPayload& Payload,
		int32 X, int32 Y, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return MakeRejected("The material graph clipboard schema version is unsupported.");
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph clipboard node count is outside the supported bound.");
		auto& Candidate = State.Program;
		if (Candidate.Nodes.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("Pasting would exceed the material graph node limit.");

		const bool bSameRoot = Payload.SourceRoot.Get() == Owner.Get();
		const auto* References = Cast<DMaterialGraphClipboardReferences>(Payload.RetainedReferences.Get());
		if (References && (References->Functions.size() != Payload.Calls.size() || References->Textures.size() != Payload.Nodes.size()))
			return MakeRejected("Clipboard retained references do not match the payload.");
		if (!State.bFunction && (!Payload.Signature.Inputs.empty() || !Payload.Signature.Outputs.empty()))
			return MakeRejected("Function interface terminals can only be pasted into a function.");
		FMaterialProgram Copied;
		for (size_t Index = 0; Index < Payload.Nodes.size(); ++Index)
		{
			auto Node = Payload.Nodes[Index].Node;
			if (References) Node.Parameter.Value.TextureValue = Cast<DTexture2D>(References->Textures[Index].Get());
			Copied.Nodes.push_back(std::move(Node));
		}
		std::vector<FMaterialParameterDefinition> CopiedSchema;
		const auto Validation = DeriveMaterialParameterSchema(Copied, CopiedSchema);
		if (!Validation) return MakeRejected("Invalid clipboard parameter owners.", Validation.Diagnostics);
		if (State.bFunction && !CopiedSchema.empty()) return MakeRejected("Functions cannot own root parameters.");
		if (!References && std::ranges::any_of(Copied.Nodes,
			[](const auto& Node) { return Node.Parameter.Value.TextureValue.Get() != nullptr; }))
			return MakeRejected("Clipboard texture references are not retained.");
		std::unordered_map<FGuid, FGuid> Remap;
		Remap.reserve(Payload.Nodes.size());
		for (const FMaterialGraphClipboardNode& ClipboardNode : Payload.Nodes)
		{
			if (!ClipboardNode.Node.Id.IsValid()
				|| !Remap.emplace(ClipboardNode.Node.Id, FGuid{}).second)
				return MakeRejected("The material graph clipboard contains an invalid or duplicate node GUID.");
			if (ClipboardNode.RelativeX < 0 || ClipboardNode.RelativeY < 0
				|| ClipboardNode.RelativeX > MaterialGraphPresentationCoordinateLimit * 2
				|| ClipboardNode.RelativeY > MaterialGraphPresentationCoordinateLimit * 2)
				return MakeRejected("The material graph clipboard contains an invalid relative position.");
		}
		std::unordered_set<FGuid> UsedIds;
		UsedIds.reserve(Candidate.Nodes.size() + Payload.Nodes.size());
		for (const FMaterialProgramNode& Node : Candidate.Nodes) UsedIds.insert(Node.Id);
		for (const FMaterialGraphClipboardNode& ClipboardNode : Payload.Nodes)
		{
			FGuid& NewId = Remap.at(ClipboardNode.Node.Id);
			do NewId = FGuid::NewGuid(); while (UsedIds.contains(NewId));
			UsedIds.insert(NewId);
		}
		if (Payload.bConnectAggregateSurface
			&& (!Payload.AggregateSourceNodeId.IsValid()
				|| !Remap.contains(Payload.AggregateSourceNodeId)))
			return MakeRejected("The aggregate Surface clipboard source is missing from the selection.");

		std::unordered_map<FGuid, FGuid> PortRemap;
		for (bool bOutput : {false, true})
		{
			const auto& SourcePorts = bOutput ? Payload.Signature.Outputs : Payload.Signature.Inputs;
			auto& DestinationPorts = bOutput ? State.Signature.Outputs : State.Signature.Inputs;
			for (auto Port : SourcePorts)
			{
				const auto OldId = Port.Id;
				if (!OldId.IsValid() || PortRemap.contains(OldId)) return MakeRejected("The clipboard contains duplicate or invalid function ports.");
				Port.Id = FGuid::NewGuid();
				const auto BaseName = Port.Name;
				for (uint32 Suffix = 2; std::ranges::any_of(DestinationPorts, [&](const auto& Existing) { return Existing.Name == Port.Name; }); ++Suffix)
					Port.Name = std::format("{} {}", BaseName, Suffix);
				PortRemap.emplace(OldId, Port.Id);
				DestinationPorts.push_back(std::move(Port));
			}
		}
		for (auto& Port : State.Signature.Inputs)
			if (std::ranges::any_of(PortRemap, [&](const auto& Pair) { return Pair.second == Port.Id; })
				&& Port.Default.Kind == EMaterialFunctionDefaultKind::Input)
			{
				if (const auto It = PortRemap.find(Port.Default.InputId); It != PortRemap.end()) Port.Default.InputId = It->second;
				else if (!bSameRoot) return MakeRejected("A copied port default references an input outside the selection.");
			}
		const auto RemapLink = [&](FMaterialProgramLink& Input) {
			if (!Input.SourceNodeId.IsValid()) return true;
			const auto It = Remap.find(Input.SourceNodeId);
			if (It != Remap.end()) Input.SourceNodeId = It->second;
			else if (!bSameRoot || !FindNode(Candidate, Input.SourceNodeId)) return false;
			return true;
		};
		for (size_t CallIndex = 0; CallIndex < Payload.Calls.size(); ++CallIndex)
		{
			auto Call = Payload.Calls[CallIndex];
			if (References) Call.Function = Cast<DMaterialFunctionInterface>(References->Functions[CallIndex].Get());
			if (!Remap.contains(Call.NodeId)) return MakeRejected("A clipboard call has no selected node.");
			Call.NodeId = Remap.at(Call.NodeId);
			for (auto& Input : Call.Inputs)
				if (!RemapLink(Input.Source))
					return MakeRejected("A copied function call references an unavailable input or parameter.");
			State.Calls.push_back(std::move(Call));
		}
		auto& Presentation = State.Presentation;
		std::vector<FGuid> Generated;
		Generated.reserve(Payload.Nodes.size());
		for (size_t ClipboardIndex = 0; ClipboardIndex < Payload.Nodes.size(); ++ClipboardIndex)
		{
			const auto& ClipboardNode = Payload.Nodes[ClipboardIndex];
			FMaterialProgramNode Node = ClipboardNode.Node;
			if (References) Node.Parameter.Value.TextureValue = Cast<DTexture2D>(References->Textures[ClipboardIndex].Get());
			Node.Id = Remap.at(ClipboardNode.Node.Id);
			if (Node.FunctionPortId.IsValid())
			{
				if (!PortRemap.contains(Node.FunctionPortId)) return MakeRejected("A clipboard terminal has no port declaration.");
				Node.FunctionPortId = PortRemap.at(Node.FunctionPortId);
			}
			if (Node.Parameter.Id.IsValid())
			{
				do Node.Parameter.Id = FGuid::NewGuid();
				while (Node.Parameter.Id == Node.Id || std::ranges::any_of(Candidate.Nodes,
					[&](const auto& Existing) { return Existing.Parameter.Id == Node.Parameter.Id; }));
				const auto BaseName = Node.Parameter.Name.ToString();
				for (uint32 Suffix = 2; std::ranges::any_of(Candidate.Nodes,
					[&](const auto& Existing) { return Existing.Parameter.Id.IsValid() && Existing.Parameter.Name == Node.Parameter.Name; }); ++Suffix)
					Node.Parameter.Name = FName(std::format("{}{}", BaseName, Suffix));
				if (Node.Parameter.DisplayName == BaseName) Node.Parameter.DisplayName = Node.Parameter.Name.ToString();
			}
			for (FMaterialProgramLink& Input : Node.Inputs)
				if (!RemapLink(Input)) return MakeRejected("The material graph clipboard references an unavailable external input.");
			for (auto& Attribute : Node.SurfaceAttributes)
				if (!RemapLink(Attribute.Source)) return MakeRejected("A copied Surface attribute references an unavailable external input.");
			const int64 PositionX = static_cast<int64>(X) + ClipboardNode.RelativeX;
			const int64 PositionY = static_cast<int64>(Y) + ClipboardNode.RelativeY;
			if (PositionX < -MaterialGraphPresentationCoordinateLimit
				|| PositionX > MaterialGraphPresentationCoordinateLimit
				|| PositionY < -MaterialGraphPresentationCoordinateLimit
				|| PositionY > MaterialGraphPresentationCoordinateLimit)
				return MakeRejected("Pasting would place a material graph node outside the supported coordinate range.");
			Generated.push_back(Node.Id);
			Presentation.Nodes.push_back({Node.Id,
				static_cast<int32>(PositionX), static_cast<int32>(PositionY)});
			Candidate.Nodes.push_back(std::move(Node));
		}
		if (Payload.bConnectAggregateSurface && !State.bFunction)
		{
			for (EMaterialSurfaceOutput Output : {
				EMaterialSurfaceOutput::BaseColor, EMaterialSurfaceOutput::Normal,
				EMaterialSurfaceOutput::Metallic, EMaterialSurfaceOutput::Roughness,
				EMaterialSurfaceOutput::AmbientOcclusion, EMaterialSurfaceOutput::Emissive,
				EMaterialSurfaceOutput::Opacity, EMaterialSurfaceOutput::OpacityMask})
				GetMaterialSurfaceOutputLink(Candidate.Outputs, Output) = {};
			Candidate.Outputs.Surface = {
				.SourceNodeId = Remap.at(Payload.AggregateSourceNodeId),
				.SourceOutputIndex = Payload.AggregateSourceOutputIndex,
				.SourceOutputId = Payload.AggregateSourceOutputId};
		}
		auto Result = Commit(std::move(State), "Paste Graph Nodes", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = Generated;
			Result.GeneratedNodeIds = std::move(Generated);
		}
		return Result;
	}

	auto FMaterialGraphOperations::DuplicateNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		int32 OffsetX,
		int32 OffsetY,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphClipboardPayload Payload;
		FMaterialGraphCommandResult Copied = CopySelection(Material, NodeIds, Payload);
		if (!Copied) return Copied;
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				Material.GetMaterialGraphPresentation(), *Material.GetMaterialProgram());
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit;
		int32 MinimumY = MaterialGraphPresentationCoordinateLimit;
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			if (Selected.contains(Position.NodeId))
			{
				MinimumX = std::min(MinimumX, Position.X);
				MinimumY = std::min(MinimumY, Position.Y);
			}
		const int64 AnchorX = static_cast<int64>(MinimumX) + OffsetX;
		const int64 AnchorY = static_cast<int64>(MinimumY) + OffsetY;
		if (AnchorX < -MaterialGraphPresentationCoordinateLimit
			|| AnchorX > MaterialGraphPresentationCoordinateLimit
			|| AnchorY < -MaterialGraphPresentationCoordinateLimit
			|| AnchorY > MaterialGraphPresentationCoordinateLimit)
			return MakeRejected("Duplicating would place a material graph node outside the supported coordinate range.");
		return Paste(Material, Payload, static_cast<int32>(AnchorX),
			static_cast<int32>(AnchorY), Transactions);
	}

	auto FMaterialGraphOperations::CutSelection(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).CutSelection(NodeIds, OutPayload, Transactions);
	}

	auto FMaterialGraphDocument::CutSelection(std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphCommandResult Copied = CopySelection(NodeIds, OutPayload);
		if (!Copied) return Copied;
		FMaterialGraphCommandResult Removed = RemoveNodes(NodeIds, Transactions);
		if (!Removed) OutPayload = {};
		return Removed;
	}
}
