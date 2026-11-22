#include "MaterialGraph.h"
#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/Material.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphEditSession.h"
#include "Materials/MaterialExpressionInputs.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	auto DMaterialGraph::CopySelection(std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		OutPayload = {};
		if (!Owner.IsValid()) return FailWithError(OutError, "The material graph owner is no longer available.");
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(*Owner.Get());
		const auto& Presentation = Material ? Material->GetMaterialGraphPresentation().Nodes
			: Cast<DMaterialFunction>(Owner.Get())->GetFunctionPresentation().Nodes;
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount + (Material ? 1u : 0u))
			return FailWithError(OutError, "The material graph copy selection is empty or exceeds the node bound.");
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size()) return FailWithError(OutError, "The material graph copy selection contains duplicate node GUIDs.");
		if (Material) Selected.erase(RootViewId);
		if (Selected.empty()) return FailWithError(OutError, "The material output terminal cannot be copied.");
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const auto& Position : Presentation) Positions.emplace(Position.NodeId, Position);
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit, MinimumY = MaterialGraphPresentationCoordinateLimit;
		for (const auto& Id : Selected)
		{
			if (std::ranges::none_of(Expressions, [&](const auto& Expression) { return Expression->Id == Id; }))
				return FailWithError(OutError, "A copied material graph node does not exist.");
			const auto It = Positions.find(Id);
			if (It == Positions.end()) return FailWithError(OutError, "A copied material graph node has no authored position.");
			MinimumX = std::min(MinimumX, It->second.X); MinimumY = std::min(MinimumY, It->second.Y);
		}
		std::vector<FGuid> Ordered(Selected.begin(), Selected.end());
		std::ranges::sort(Ordered);
		OutPayload.SourceRoot = Owner.Get();
		std::unordered_map<DMaterialExpression*, DMaterialExpression*> CopiedExpressions;
		for (const auto& Id : Ordered)
		{
			const auto It = std::ranges::find(Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			const auto& Position = Positions.at(Id);
			const auto Duplicated = DuplicateObject(It->Get(), nullptr, NAME_None);
			if (!Duplicated)
			{
				if (OutError) *OutError = ToString(Duplicated.error());
				return false;
			}
			auto* Copy = *Duplicated;
			CopiedExpressions.emplace(It->Get(), Copy);
			OutPayload.Nodes.push_back({TStrongObjectPtr<DMaterialExpression>(Copy), Position.DisplayName, Position.X - MinimumX, Position.Y - MinimumY});
		}
		for (const auto& Node : OutPayload.Nodes)
			VisitStoredMaterialInputs(*Node.Expression, [&](FMaterialExpressionInput& Input) {
				const auto It = CopiedExpressions.find(Input.Expression.Get());
				if (It != CopiedExpressions.end()) Input.Expression = It->second;
			});
		if (Material)
		{
			const auto& Surface = *Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes);
			if (const auto It = CopiedExpressions.find(Surface.Expression.Get()); It != CopiedExpressions.end())
				OutPayload.AggregateSurface.Connect(Surface.OutputIndex, It->second);
		}
		return true;
	}

	auto DMaterialGraph::Paste(const FMaterialGraphClipboardPayload& Payload,
		int32 X, int32 Y, ETransactionMode Mode) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return std::unexpected(RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner));
		FGraphEditSession State(*Owner.Get());
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return std::unexpected(RejectCommand("The material graph clipboard schema version is unsupported."));
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount
			|| State.Expressions.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return std::unexpected(RejectCommand("Pasting would exceed the material graph node bounds."));
		const bool bSameRoot = Payload.SourceRoot.Get() == Owner.Get();
		if (std::ranges::any_of(Payload.Nodes, [&](const auto& Node) {
			return Node.Expression.IsValid() && !GetSchema().CanCreateExpression(*Node.Expression.Get());
		}))
			return std::unexpected(RejectCommand("The clipboard contains expressions unsupported by this graph."));
		std::unordered_set<FGuid> ClipboardIds;
		std::unordered_set<DMaterialExpression*> ExternalSources;
		std::unordered_set<FGuid> UsedIds;
		for (const auto& Expression : State.Expressions)
		{
			if (bSameRoot) ExternalSources.insert(Expression.Get());
			UsedIds.insert(Expression->Id);
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get())) UsedIds.insert(Parameter->Metadata.Id);
		}
		std::vector<FMaterialParameterDefinition> Definitions;
		for (const auto& Node : Payload.Nodes)
		{
			if (!Node.Expression || !Node.Expression->Id.IsValid() || !ClipboardIds.insert(Node.Expression->Id).second)
				return std::unexpected(RejectCommand("The material graph clipboard contains an invalid or duplicate node GUID."));
			if (Node.RelativeX < 0 || Node.RelativeY < 0 || Node.RelativeX > MaterialGraphPresentationCoordinateLimit * 2
				|| Node.RelativeY > MaterialGraphPresentationCoordinateLimit * 2)
				return std::unexpected(RejectCommand("The material graph clipboard contains an invalid relative position."));
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Node.Expression.Get()))
			{
				const auto Definition = Parameter->GetParameterDefinition();
				const auto Existing = std::ranges::find(Definitions, Definition.Id, &FMaterialParameterDefinition::Id);
				if (Existing == Definitions.end()) Definitions.push_back(Definition);
				else if (*Existing != Definition) return std::unexpected(RejectCommand("Clipboard shared parameter definitions disagree."));
			}
		}
		const auto Validation = ValidateMaterialParameterDefinitions(Definitions);
		if (!Validation) return std::unexpected(RejectCommand(std::string(GetMaterialParameterErrorText((Validation).Error))));
		if (!GetSchema().CanOwnParameters() && !Definitions.empty()) return std::unexpected(RejectCommand("Functions cannot own root parameters."));
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
				if (!OldId.IsValid() || PortRemap.contains(OldId)) return std::unexpected(RejectCommand("The clipboard contains duplicate or invalid function ports."));
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
				else if (!bSameRoot) return std::unexpected(RejectCommand("A copied port default references an input outside the selection."));
			}
		std::unordered_map<DMaterialExpression*, DMaterialExpression*> Copies;
		UsedIds.insert(ClipboardIds.begin(), ClipboardIds.end());
		std::vector<FGuid> Generated;
		for (const auto& Entry : Payload.Nodes)
		{
			const auto Duplicated = DuplicateObject(Entry.Expression.Get(), nullptr, NAME_None);
			if (!Duplicated) return std::unexpected(RejectCommand(ToString(Duplicated.error())));
			TStrongObjectPtr<DMaterialExpression> Expression(*Duplicated);
			do Expression->Id = FGuid::NewGuid(); while (!UsedIds.insert(Expression->Id).second);
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get())) Input->Port = PortRemap.at(Input->Port.Id);
			if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) Output->Port = PortRemap.at(Output->Port.Id);
			if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			{
				Parameter->Metadata.Id = FGuid::NewGuid();
				if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return std::unexpected(Error);
			}
			Copies.emplace(Entry.Expression.Get(), Expression.Get());
			const int64 PositionX = static_cast<int64>(X) + Entry.RelativeX, PositionY = static_cast<int64>(Y) + Entry.RelativeY;
			if (PositionX < -MaterialGraphPresentationCoordinateLimit || PositionX > MaterialGraphPresentationCoordinateLimit
				|| PositionY < -MaterialGraphPresentationCoordinateLimit || PositionY > MaterialGraphPresentationCoordinateLimit)
				return std::unexpected(RejectCommand("Pasting would place a material graph node outside the supported coordinate range."));
			Generated.push_back(Expression->Id);
			State.Presentation.Nodes.push_back({Expression->Id, static_cast<int32>(PositionX), static_cast<int32>(PositionY), Entry.DisplayName});
			State.Expressions.emplace_back(Expression.Get());
		}
		for (const auto& [SourceExpression, Copy] : Copies)
		{
			bool Valid = true;
			VisitStoredMaterialInputs(*Copy, [&](FMaterialExpressionInput& Input) {
				if (!Input.IsConnected()) return;
				auto* Source = Input.Expression.Get();
				if (const auto It = Copies.find(Source); It != Copies.end()) Input.Expression = It->second;
				else if (!ExternalSources.contains(Source)) Valid = false;
			});
			if (!Valid) return std::unexpected(RejectCommand("The material graph clipboard references an unavailable external input."));
		}
		if (Payload.AggregateSurface.IsConnected())
		{
			const auto Source = Copies.find(Payload.AggregateSurface.Expression.Get());
			if (Source == Copies.end()) return std::unexpected(RejectCommand("The aggregate Surface clipboard source is missing from the selection."));
			if (!State.bFunction) State.EditRootInput(EMaterialProperty::MP_MaterialAttributes)->Connect(Payload.AggregateSurface.OutputIndex, Source->second);
		}
		auto Result = State.Commit("Paste Graph Nodes", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return GetCreatedNodes(Generated);
	}

	auto DMaterialGraph::DuplicateNodes(std::span<const FGuid> NodeIds,
		int32 OffsetX, int32 OffsetY, ETransactionMode Mode) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		FMaterialGraphClipboardPayload Payload;
		std::string Error;
		if (!CopySelection(NodeIds, Payload, &Error))
		{
			const auto Status = Owner.IsValid() ? EMaterialGraphCommandStatus::Rejected : EMaterialGraphCommandStatus::StaleOwner;
			return std::unexpected(RejectCommand(std::move(Error), {}, Status));
		}
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto& Presentation = Material ? Material->GetMaterialGraphPresentation().Nodes
			: Cast<DMaterialFunction>(Owner.Get())->GetFunctionPresentation().Nodes;
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit;
		int32 MinimumY = MaterialGraphPresentationCoordinateLimit;
		std::unordered_set<FGuid> Selected;
		for (const auto& Entry : Payload.Nodes) Selected.insert(Entry.Expression->Id);
		for (const FMaterialGraphNodePresentation& Position : Presentation)
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
			return std::unexpected(RejectCommand("Duplicating would place a material graph node outside the supported coordinate range."));
		return Paste(Payload, static_cast<int32>(AnchorX), static_cast<int32>(AnchorY), Mode);
	}

	auto DMaterialGraph::CutSelection(std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		std::string Error;
		if (!CopySelection(NodeIds, OutPayload, &Error))
		{
			const auto Status = Owner.IsValid() ? EMaterialGraphCommandStatus::Rejected : EMaterialGraphCommandStatus::StaleOwner;
			return RejectCommand(std::move(Error), {}, Status);
		}
		FMaterialGraphCommandResult Removed = RemoveNodes(NodeIds, Mode);
		if (!Removed) OutPayload = {};
		return Removed;
	}
}
