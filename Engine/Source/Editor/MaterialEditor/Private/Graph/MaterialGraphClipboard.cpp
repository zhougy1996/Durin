#include "MaterialGraphEditInternals.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	auto FMaterialGraphOperations::CopySelection(
		const DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload)
		-> FMaterialGraphCommandResult
	{
		OutPayload = {};
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph copy selection is empty or exceeds the node bound.");
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size())
			return MakeRejected("The material graph copy selection contains duplicate node GUIDs.");
		const FMaterialProgram& Program = *Material.GetMaterialProgram();
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				Material.GetMaterialGraphPresentation(), Program);
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
		OutPayload.SourceRoot = const_cast<DMaterial*>(&Material);
		std::unordered_set<FGuid> Referenced;
		OutPayload.Nodes.reserve(Ordered.size());
		for (const FGuid& Id : Ordered)
		{
			FMaterialProgramNode Node = *FindNode(Program, Id);
			if (Node.ParameterId.IsValid()) Referenced.insert(Node.ParameterId);
			const FMaterialGraphNodePresentation& Position = Positions.at(Id);
			OutPayload.Nodes.push_back({
				.Node = std::move(Node),
				.RelativeX = Position.X - MinimumX,
				.RelativeY = Position.Y - MinimumY,
			});
		}
		for (const auto& Definition : Material.GetParameterDefinitions())
			if (Referenced.contains(Definition.Id))
			{
				OutPayload.Definitions.push_back(Definition);
				if (auto* Texture = Definition.Value.TextureValue.Get())
					OutPayload.RetainedTextures.emplace_back(Texture);
			}
		if (OutPayload.Definitions.size() != Referenced.size())
		{
			OutPayload = {};
			return MakeRejected("A copied parameter declaration is unavailable.");
		}
		if (Selected.contains(Program.Outputs.Surface.SourceNodeId))
		{
			OutPayload.bConnectAggregateSurface = true;
			OutPayload.AggregateSourceNodeId = Program.Outputs.Surface.SourceNodeId;
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
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return MakeRejected("The material graph clipboard schema version is unsupported.");
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph clipboard node count is outside the supported bound.");
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (Candidate.Nodes.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("Pasting would exceed the material graph node limit.");

		const bool bSameRoot = Payload.SourceRoot.Get() == &Material;
		const auto DeclarationValidation = ValidateMaterialParameterDefinitions(Payload.Definitions);
		if (!DeclarationValidation)
			return MakeRejected(std::format("Clipboard declaration {}: {}",
				DeclarationValidation.ParameterId.ToString(),
				GetMaterialParameterErrorText(DeclarationValidation.Error)));
		std::vector<FMaterialParameterDefinition> Definitions(
			Material.GetParameterDefinitions().begin(), Material.GetParameterDefinitions().end());
		std::unordered_map<FGuid, FGuid> ParameterRemap;
		for (const auto& Source : Payload.Definitions)
		{
			const auto Existing = std::ranges::find_if(Definitions, [&](const auto& Definition) {
				return bSameRoot ? Definition.Id == Source.Id : Definition.Name == Source.Name;
			});
			if (Existing != Definitions.end())
			{
				auto Comparable = Source;
				Comparable.Id = Existing->Id;
				if (Existing->Type != Source.Type || (!bSameRoot && *Existing != Comparable))
					return MakeRejected(std::format("Clipboard declaration {} conflicts with the destination name, type, default or metadata.", Source.Id.ToString()));
				ParameterRemap.emplace(Source.Id, Existing->Id);
			}
			else
			{
				if (bSameRoot)
					return MakeRejected(std::format("Clipboard declaration {} no longer exists in the source root.", Source.Id.ToString()));
				auto Local = Source;
				do Local.Id = FGuid::NewGuid(); while (std::ranges::any_of(Definitions,
					[&](const auto& Definition) { return Definition.Id == Local.Id; }));
				ParameterRemap.emplace(Source.Id, Local.Id);
				Definitions.push_back(std::move(Local));
			}
		}
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

		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::vector<FGuid> Generated;
		Generated.reserve(Payload.Nodes.size());
		for (const FMaterialGraphClipboardNode& ClipboardNode : Payload.Nodes)
		{
			FMaterialProgramNode Node = ClipboardNode.Node;
			Node.Id = Remap.at(ClipboardNode.Node.Id);
			if (Node.ParameterId.IsValid())
			{
				const auto Parameter = ParameterRemap.find(Node.ParameterId);
				if (Parameter == ParameterRemap.end())
					return MakeRejected("The clipboard is missing a referenced parameter declaration.");
				Node.ParameterId = Parameter->second;
				const auto Definition = std::ranges::find(Definitions, Node.ParameterId,
					&FMaterialParameterDefinition::Id);
				Node.DisplayName = Definition->DisplayName;
			}
			for (FMaterialProgramLink& Input : Node.Inputs)
			{
				const auto It = Remap.find(Input.SourceNodeId);
				if (It != Remap.end()) Input.SourceNodeId = It->second;
				else if (!bSameRoot || !FindNode(Candidate, Input.SourceNodeId))
					return MakeRejected(
						"The material graph clipboard references an unavailable external input.");
			}
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
		if (Payload.bConnectAggregateSurface)
		{
			for (EMaterialSurfaceOutput Output : {
				EMaterialSurfaceOutput::BaseColor, EMaterialSurfaceOutput::Normal,
				EMaterialSurfaceOutput::Metallic, EMaterialSurfaceOutput::Roughness,
				EMaterialSurfaceOutput::AmbientOcclusion, EMaterialSurfaceOutput::Emissive,
				EMaterialSurfaceOutput::Opacity, EMaterialSurfaceOutput::OpacityMask})
				GetMaterialSurfaceOutputLink(Candidate.Outputs, Output) = {};
			Candidate.Outputs.Surface = {
				.SourceNodeId = Remap.at(Payload.AggregateSourceNodeId)};
		}
		auto Result = ReplaceDefinitionsAndProgram(Material, std::move(Definitions),
			std::move(Candidate), std::move(Presentation), Transactions);
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
		FMaterialGraphCommandResult Copied = CopySelection(
			Material, NodeIds, OutPayload);
		if (!Copied) return Copied;
		FMaterialGraphCommandResult Removed = RemoveNodes(
			Material, NodeIds, Transactions);
		if (!Removed) OutPayload = {};
		return Removed;
	}
}
