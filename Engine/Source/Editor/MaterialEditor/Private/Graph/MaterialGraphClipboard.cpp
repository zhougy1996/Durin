#include "Materials/MaterialExpressionBuild.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphExpressionState.h"
#include "MaterialExpressionInputs.h"

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
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph copy selection is empty or exceeds the node bound.");
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size()) return MakeRejected("The material graph copy selection contains duplicate node GUIDs.");
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const auto& Position : State.Presentation.Nodes) Positions.emplace(Position.NodeId, Position);
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit, MinimumY = MaterialGraphPresentationCoordinateLimit;
		for (const auto& Id : Selected)
		{
			if (std::ranges::none_of(State.Expressions, [&](const auto& Expression) { return Expression->Id == Id; }))
				return MakeRejected("A copied material graph node does not exist.");
			const auto It = Positions.find(Id);
			if (It == Positions.end()) return MakeRejected("A copied material graph node has no authored position.");
			MinimumX = std::min(MinimumX, It->second.X); MinimumY = std::min(MinimumY, It->second.Y);
		}
		std::vector<FGuid> Ordered(Selected.begin(), Selected.end());
		std::ranges::sort(Ordered);
		OutPayload.SourceRoot = Owner.Get();
		for (const auto& Id : Ordered)
		{
			const auto It = std::ranges::find(State.Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			const auto* Input = Cast<DMaterialExpressionFunctionInput>(It->Get());
			const auto* Output = Cast<DMaterialExpressionFunctionOutput>(It->Get());
			if (Input || Output)
			{
				const auto PortId = Input ? Input->PortId : Output->PortId;
				const auto& Ports = Input ? State.Signature.Inputs : State.Signature.Outputs;
				const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end()) { OutPayload = {}; return MakeRejected("A copied function port is unavailable."); }
				(Input ? OutPayload.Signature.Inputs : OutPayload.Signature.Outputs).push_back(*Port);
			}
			const auto& Position = Positions.at(Id);
			OutPayload.Nodes.push_back({*It, Position.DisplayName, Position.X - MinimumX, Position.Y - MinimumY});
		}
		if (Selected.contains(State.Outputs.Surface.ExpressionId))
		{
			OutPayload.bConnectAggregateSurface = true;
			OutPayload.AggregateSourceNodeId = State.Outputs.Surface.ExpressionId;
			OutPayload.AggregateSourceOutputIndex = State.Outputs.Surface.OutputIndex;
			OutPayload.AggregateSourceOutputId = State.Outputs.Surface.OutputId;
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
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return MakeRejected("The material graph clipboard schema version is unsupported.");
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount
			|| State.Expressions.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("Pasting would exceed the material graph node bounds.");
		const bool bSameRoot = Payload.SourceRoot.Get() == Owner.Get();
		if (!State.bFunction && (!Payload.Signature.Inputs.empty() || !Payload.Signature.Outputs.empty()))
			return MakeRejected("Function interface terminals can only be pasted into a function.");
		std::unordered_map<FGuid, FGuid> Remap;
		std::unordered_set<FGuid> UsedIds;
		for (const auto& Expression : State.Expressions)
		{
			UsedIds.insert(Expression->Id);
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get())) UsedIds.insert(Parameter->Metadata.Id);
		}
		std::vector<FMaterialParameterDefinition> Definitions;
		for (const auto& Node : Payload.Nodes)
		{
			if (!Node.Expression || !Node.Expression->Id.IsValid() || Remap.contains(Node.Expression->Id))
				return MakeRejected("The material graph clipboard contains an invalid or duplicate node GUID.");
			if (Node.RelativeX < 0 || Node.RelativeY < 0 || Node.RelativeX > MaterialGraphPresentationCoordinateLimit * 2
				|| Node.RelativeY > MaterialGraphPresentationCoordinateLimit * 2)
				return MakeRejected("The material graph clipboard contains an invalid relative position.");
			FGuid Id;
			do Id = FGuid::NewGuid(); while (UsedIds.contains(Id));
			UsedIds.insert(Id); Remap.emplace(Node.Expression->Id, Id);
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Node.Expression.Get()))
				Definitions.push_back(Parameter->GetParameterDefinition());
		}
		const auto Validation = ValidateMaterialParameterDefinitions(Definitions);
		if (!Validation) return MakeRejected(std::string(GetMaterialParameterErrorText(Validation.Error)));
		if (State.bFunction && !Definitions.empty()) return MakeRejected("Functions cannot own root parameters.");
		if (Payload.bConnectAggregateSurface && !Remap.contains(Payload.AggregateSourceNodeId))
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
		const auto RemapLink = [&](FMaterialExpressionInput& Input) {
			if (!Input.ExpressionId.IsValid()) return true;
			const auto It = Remap.find(Input.ExpressionId);
			if (It != Remap.end()) Input.ExpressionId = It->second;
			else if (!bSameRoot || std::ranges::none_of(State.Expressions, [&](const auto& E) { return E->Id == Input.ExpressionId; })) return false;
			return true;
		};
		std::vector<FGuid> Generated;
		for (const auto& Entry : Payload.Nodes)
		{
			TStrongObjectPtr<DMaterialExpression> Expression(DuplicateObject(Entry.Expression.Get(), nullptr, NAME_None));
			if (!Expression) return MakeRejected("Unable to duplicate a clipboard expression.");
			Expression->Id = Remap.at(Entry.Expression->Id);
			FGuid* PortId = nullptr;
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get())) PortId = &Input->PortId;
			if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) PortId = &Output->PortId;
			if (PortId)
			{
				if (!PortRemap.contains(*PortId)) return MakeRejected("A clipboard terminal has no port declaration.");
				*PortId = PortRemap.at(*PortId);
			}
			if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			{
				auto& Metadata = Parameter->Metadata;
				do Metadata.Id = FGuid::NewGuid(); while (UsedIds.contains(Metadata.Id));
				UsedIds.insert(Metadata.Id);
				const auto BaseName = Metadata.Name.ToString();
				for (uint32 Suffix = 2; std::ranges::any_of(State.Expressions, [&](const auto& E) {
					const auto* P = Cast<DMaterialExpressionParameter>(E.Get()); return P && P->Metadata.Name == Metadata.Name;
				}); ++Suffix) Metadata.Name = FName(std::format("{}{}", BaseName, Suffix));
				if (Metadata.DisplayName == BaseName) Metadata.DisplayName = Metadata.Name.ToString();
			}
			bool bLinksValid = true;
			VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) { bLinksValid &= RemapLink(Input); });
			if (!bLinksValid) return MakeRejected("The material graph clipboard references an unavailable external input.");
			const int64 PositionX = static_cast<int64>(X) + Entry.RelativeX, PositionY = static_cast<int64>(Y) + Entry.RelativeY;
			if (PositionX < -MaterialGraphPresentationCoordinateLimit || PositionX > MaterialGraphPresentationCoordinateLimit
				|| PositionY < -MaterialGraphPresentationCoordinateLimit || PositionY > MaterialGraphPresentationCoordinateLimit)
				return MakeRejected("Pasting would place a material graph node outside the supported coordinate range.");
			Generated.push_back(Expression->Id);
			State.Presentation.Nodes.push_back({Expression->Id, static_cast<int32>(PositionX), static_cast<int32>(PositionY), Entry.DisplayName});
			State.Expressions.push_back(std::move(Expression));
		}
		if (State.bFunction)
		{
			std::vector<DMaterialFunctionInterface*> Roots;
			for (const auto& Expression : State.Expressions)
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Roots.push_back(Call->Function.Get());
			std::vector<FMaterialFunctionOwnerStamp> Closure;
			const auto Result = ValidateMaterialFunctionDependencies(Roots, Closure);
			if (!Result) return MakeRejected("The function dependencies are invalid.", Result.Diagnostics);
			if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
				return MakeRejected("This change would introduce recursive function dependencies.");
		}
		if (Payload.bConnectAggregateSurface && !State.bFunction)
		{
			for (auto* Output : {&State.Outputs.BaseColor, &State.Outputs.Normal, &State.Outputs.Metallic, &State.Outputs.Roughness,
				&State.Outputs.AmbientOcclusion, &State.Outputs.Emissive, &State.Outputs.Opacity, &State.Outputs.OpacityMask}) *Output = {};
			State.Outputs.Surface = {Remap.at(Payload.AggregateSourceNodeId), Payload.AggregateSourceOutputIndex, Payload.AggregateSourceOutputId};
		}
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Paste Graph Nodes", Transactions);
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
		const auto& Presentation = Material.GetMaterialGraphPresentation();
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
