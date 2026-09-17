#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionBuild.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditSession.h"
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
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(*Owner.Get());
		const auto& Presentation = Material ? Material->GetMaterialGraphPresentation().Nodes
			: Cast<DMaterialFunction>(Owner.Get())->GetFunctionPresentation().Nodes;
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph copy selection is empty or exceeds the node bound.");
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size()) return MakeRejected("The material graph copy selection contains duplicate node GUIDs.");
		for (const auto& Expression : Expressions)
			if (Cast<DMaterialExpressionMaterialOutput>(Expression.Get())) Selected.erase(Expression->Id);
		if (Selected.empty()) return MakeRejected("The material output terminal cannot be copied.");
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const auto& Position : Presentation) Positions.emplace(Position.NodeId, Position);
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit, MinimumY = MaterialGraphPresentationCoordinateLimit;
		for (const auto& Id : Selected)
		{
			if (std::ranges::none_of(Expressions, [&](const auto& Expression) { return Expression->Id == Id; }))
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
			const auto It = std::ranges::find(Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			const auto& Position = Positions.at(Id);
			auto* Copy = DuplicateObject(It->Get(), nullptr, NAME_None);
			if (!Copy) return MakeRejected("Unable to copy the selected expression.");
			OutPayload.Nodes.push_back({TStrongObjectPtr<DMaterialExpression>(Copy), Position.DisplayName, Position.X - MinimumX, Position.Y - MinimumY});
		}
		if (Material && Selected.contains(Material->GetExpressionOutputs().Surface.ExpressionId))
		{
			OutPayload.bConnectAggregateSurface = true;
			OutPayload.AggregateSourceNodeId = Material->GetExpressionOutputs().Surface.ExpressionId;
			OutPayload.AggregateSourceOutputIndex = Material->GetExpressionOutputs().Surface.OutputIndex;
			OutPayload.AggregateSourceOutputId = Material->GetExpressionOutputs().Surface.OutputId;
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
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return MakeRejected("The material graph clipboard schema version is unsupported.");
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount
			|| State.Expressions.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("Pasting would exceed the material graph node bounds.");
		const bool bSameRoot = Payload.SourceRoot.Get() == Owner.Get();
		if (!State.bFunction && std::ranges::any_of(Payload.Nodes, [](const auto& Node) {
			return Cast<DMaterialExpressionFunctionInput>(Node.Expression.Get()) || Cast<DMaterialExpressionFunctionOutput>(Node.Expression.Get());
		}))
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
				{
				const auto Definition = Parameter->GetParameterDefinition();
				const auto Existing = std::ranges::find(Definitions, Definition.Id, &FMaterialParameterDefinition::Id);
				if (Existing == Definitions.end()) Definitions.push_back(Definition);
				else if (*Existing != Definition) return MakeRejected("Clipboard shared parameter definitions disagree.");
			}
		}
		const auto Validation = ValidateMaterialParameterDefinitions(Definitions);
		if (!Validation) return MakeRejected(std::string(GetMaterialParameterErrorText(Validation.Error)));
		if (State.bFunction && !Definitions.empty()) return MakeRejected("Functions cannot own root parameters.");
		if (Payload.bConnectAggregateSurface && !Remap.contains(Payload.AggregateSourceNodeId))
			return MakeRejected("The aggregate Surface clipboard source is missing from the selection.");
		std::unordered_map<FGuid, FMaterialFunctionPort> PortRemap;
		for (bool bOutput : {false, true})
		{
			const auto GetPort = [bOutput](DMaterialExpression* Expression) -> const FMaterialFunctionPort* {
				if (bOutput)
				{
					const auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression);
					return Terminal ? &Terminal->Port : nullptr;
				}
				const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(Expression);
				return Terminal ? &Terminal->Port : nullptr;
			};
			std::unordered_set<std::string> Names;
			for (const auto& Expression : State.Expressions)
				if (const auto* Port = GetPort(Expression.Get())) Names.insert(Port->Name);
			for (const auto& Node : Payload.Nodes)
			{
				const auto* Source = GetPort(Node.Expression.Get());
				if (!Source) continue;
				auto Port = *Source;
				const auto OldId = Port.Id;
				if (!OldId.IsValid() || PortRemap.contains(OldId)) return MakeRejected("The clipboard contains duplicate or invalid function ports.");
				Port.Id = FGuid::NewGuid();
				const auto BaseName = Port.Name;
				for (uint32 Suffix = 2; Names.contains(Port.Name); ++Suffix) Port.Name = std::format("{} {}", BaseName, Suffix);
				Names.insert(Port.Name);
				PortRemap.emplace(OldId, std::move(Port));
			}
		}
		for (auto& [OldId, Port] : PortRemap)
			if (Port.Default.Kind == EMaterialFunctionDefaultKind::Input)
			{
				if (const auto It = PortRemap.find(Port.Default.InputId); It != PortRemap.end()) Port.Default.InputId = It->second.Id;
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
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get())) Input->Port = PortRemap.at(Input->Port.Id);
			if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) Output->Port = PortRemap.at(Output->Port.Id);
			if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			{
				Parameter->Metadata.Id = FGuid::NewGuid();
				if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error.empty()) return MakeRejected(Error);
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
			State.Expressions.emplace_back(Expression.Get());
		}
		if (State.bFunction)
		{
			std::vector<DMaterialFunctionInterface*> Roots;
			for (const auto& Expression : State.Expressions)
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Roots.push_back(Call->Function.Get());
			std::vector<FMaterialFunctionOwnerStamp> Closure;
			const auto Result = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
			if (!Result) return MakeRejected("The function dependencies are invalid.", Result.Diagnostics);
			if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
				return MakeRejected("This change would introduce recursive function dependencies.");
		}
		if (Payload.bConnectAggregateSurface && !State.bFunction)
		{
			State.GetOutputs().Surface = {Remap.at(Payload.AggregateSourceNodeId), Payload.AggregateSourceOutputIndex, Payload.AggregateSourceOutputId};
		}
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Paste Graph Nodes", Transactions);
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
