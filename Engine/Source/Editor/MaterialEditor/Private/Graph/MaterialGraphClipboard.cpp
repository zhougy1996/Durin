#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionBuild.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditSession.h"
#include "MaterialExpressionInputs.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	auto FormatMaterialGraphClipboardError(const FMaterialGraphClipboardError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EMaterialGraphClipboardError::None: return {};
		case EMaterialGraphClipboardError::Definition:
			return Error.ValidationCause ? std::string(GetMaterialParameterErrorText(Error.ValidationCause->Error)) : "The clipboard parameter definitions are invalid.";
		case EMaterialGraphClipboardError::CopyBounds: return "The material graph copy selection is empty or exceeds the node bound.";
		case EMaterialGraphClipboardError::CopyDuplicate: return "The material graph copy selection contains duplicate node GUIDs.";
		case EMaterialGraphClipboardError::CopyTerminal: return "The material output terminal cannot be copied.";
		case EMaterialGraphClipboardError::CopyMissing: return "A copied material graph node does not exist.";
		case EMaterialGraphClipboardError::CopyPosition: return "A copied material graph node has no authored position.";
		case EMaterialGraphClipboardError::CopyObject: return Error.DuplicationCause ? FormatObjectGraphError(*Error.DuplicationCause) : "Unable to copy the selected expression.";
		case EMaterialGraphClipboardError::Schema: return "The material graph clipboard schema version is unsupported.";
		case EMaterialGraphClipboardError::PasteBounds: return "Pasting would exceed the material graph node bounds.";
		case EMaterialGraphClipboardError::FunctionTerminal: return "Function interface terminals can only be pasted into a function.";
		case EMaterialGraphClipboardError::NodeIdentity: return "The material graph clipboard contains an invalid or duplicate node GUID.";
		case EMaterialGraphClipboardError::RelativePosition: return "The material graph clipboard contains an invalid relative position.";
		case EMaterialGraphClipboardError::SharedDefinition: return "Clipboard shared parameter definitions disagree.";
		case EMaterialGraphClipboardError::FunctionParameter: return "Functions cannot own root parameters.";
		case EMaterialGraphClipboardError::AggregateSource: return "The aggregate Surface clipboard source is missing from the selection.";
		case EMaterialGraphClipboardError::PortIdentity: return "The clipboard contains duplicate or invalid function ports.";
		case EMaterialGraphClipboardError::PortDefault: return "A copied port default references an input outside the selection.";
		case EMaterialGraphClipboardError::PasteObject: return Error.DuplicationCause ? FormatObjectGraphError(*Error.DuplicationCause) : "Unable to duplicate a clipboard expression.";
		case EMaterialGraphClipboardError::ExternalInput: return "The material graph clipboard references an unavailable external input.";
		case EMaterialGraphClipboardError::PastePosition: return "Pasting would place a material graph node outside the supported coordinate range.";
		case EMaterialGraphClipboardError::DuplicatePosition: return "Duplicating would place a material graph node outside the supported coordinate range.";
		}
		return {};
	}
	namespace
	{
		auto RejectClipboard(FMaterialGraphClipboardError Error) -> FMaterialGraphCommandResult
		{
			FMaterialGraphCommandResult Result;
			Result.ClipboardCause = std::move(Error);
			return Result;
		}
	}

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
		if (!Owner.IsValid()) return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner});
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(*Owner.Get());
		const auto& Presentation = Material ? Material->GetMaterialGraphPresentation().Nodes
			: Cast<DMaterialFunction>(Owner.Get())->GetFunctionPresentation().Nodes;
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return RejectClipboard({.Code = EMaterialGraphClipboardError::CopyBounds, .Count = NodeIds.size(), .Limit = MaterialProgramMaxNodeCount});
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size()) return RejectClipboard({.Code = EMaterialGraphClipboardError::CopyDuplicate, .Count = NodeIds.size(), .ExistingCount = Selected.size()});
		for (const auto& Expression : Expressions)
			if (Cast<DMaterialExpressionMaterialOutput>(Expression.Get())) Selected.erase(Expression->Id);
		if (Selected.empty()) return RejectClipboard({.Code = EMaterialGraphClipboardError::CopyTerminal});
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const auto& Position : Presentation) Positions.emplace(Position.NodeId, Position);
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit, MinimumY = MaterialGraphPresentationCoordinateLimit;
		for (const auto& Id : Selected)
		{
			if (std::ranges::none_of(Expressions, [&](const auto& Expression) { return Expression->Id == Id; }))
				return RejectClipboard({.Code = EMaterialGraphClipboardError::CopyMissing, .NodeId = Id});
			const auto It = Positions.find(Id);
			if (It == Positions.end()) return RejectClipboard({.Code = EMaterialGraphClipboardError::CopyPosition, .NodeId = Id});
			MinimumX = std::min(MinimumX, It->second.X); MinimumY = std::min(MinimumY, It->second.Y);
		}
		std::vector<FGuid> Ordered(Selected.begin(), Selected.end());
		std::ranges::sort(Ordered);
		OutPayload.SourceRoot = Owner.Get();
		for (const auto& Id : Ordered)
		{
			const auto It = std::ranges::find(Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			const auto& Position = Positions.at(Id);
			const auto Duplicated = DuplicateObject(It->Get(), nullptr, NAME_None);
			auto* Copy = Duplicated.Object;
			if (!Copy) return RejectClipboard({.Code = EMaterialGraphClipboardError::CopyObject, .NodeId = Id, .DuplicationCause = std::make_shared<FObjectGraphError>(Duplicated.Error)});
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
			.Disposition = EMaterialGraphCommandDisposition::Applied,
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
		if (!Owner.IsValid()) return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner});
		FGraphEditSession State(*Owner.Get());
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return RejectClipboard({.Code = EMaterialGraphClipboardError::Schema, .Count = Payload.SchemaVersion, .Limit = CurrentMaterialGraphClipboardSchemaVersion});
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount
			|| State.Expressions.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return RejectClipboard({.Code = EMaterialGraphClipboardError::PasteBounds, .Count = Payload.Nodes.size(), .ExistingCount = State.Expressions.size(), .Limit = MaterialProgramMaxNodeCount});
		const bool bSameRoot = Payload.SourceRoot.Get() == Owner.Get();
		if (!State.bFunction && std::ranges::any_of(Payload.Nodes, [](const auto& Node) {
			return Cast<DMaterialExpressionFunctionInput>(Node.Expression.Get()) || Cast<DMaterialExpressionFunctionOutput>(Node.Expression.Get());
		}))
			return RejectClipboard({.Code = EMaterialGraphClipboardError::FunctionTerminal});
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
				return RejectClipboard({.Code = EMaterialGraphClipboardError::NodeIdentity, .NodeId = Node.Expression ? Node.Expression->Id : FGuid{}});
			if (Node.RelativeX < 0 || Node.RelativeY < 0 || Node.RelativeX > MaterialGraphPresentationCoordinateLimit * 2
				|| Node.RelativeY > MaterialGraphPresentationCoordinateLimit * 2)
				return RejectClipboard({.Code = EMaterialGraphClipboardError::RelativePosition, .NodeId = Node.Expression->Id, .X = Node.RelativeX, .Y = Node.RelativeY, .CoordinateLimit = MaterialGraphPresentationCoordinateLimit * 2});
			FGuid Id;
			do Id = FGuid::NewGuid(); while (UsedIds.contains(Id));
			UsedIds.insert(Id); Remap.emplace(Node.Expression->Id, Id);
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Node.Expression.Get()))
				{
				const auto Definition = Parameter->GetParameterDefinition();
				const auto Existing = std::ranges::find(Definitions, Definition.Id, &FMaterialParameterDefinition::Id);
				if (Existing == Definitions.end()) Definitions.push_back(Definition);
				else if (*Existing != Definition) return RejectClipboard({.Code = EMaterialGraphClipboardError::SharedDefinition, .NodeId = Node.Expression->Id, .ParameterId = Definition.Id});
			}
		}
		const auto Validation = ValidateMaterialParameterDefinitions(Definitions);
		if (!Validation) return RejectClipboard({.Code = EMaterialGraphClipboardError::Definition, .ParameterId = Validation.ParameterId, .ValidationCause = Validation});
		if (State.bFunction && !Definitions.empty()) return RejectClipboard({.Code = EMaterialGraphClipboardError::FunctionParameter, .Count = Definitions.size()});
		if (Payload.bConnectAggregateSurface && !Remap.contains(Payload.AggregateSourceNodeId))
			return RejectClipboard({.Code = EMaterialGraphClipboardError::AggregateSource, .SourceId = Payload.AggregateSourceNodeId});
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
				if (!OldId.IsValid() || PortRemap.contains(OldId)) return RejectClipboard({.Code = EMaterialGraphClipboardError::PortIdentity, .PortId = OldId});
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
				else if (!bSameRoot) return RejectClipboard({.Code = EMaterialGraphClipboardError::PortDefault, .PortId = OldId, .SourceId = Port.Default.InputId});
			}
		const auto RemapLink = [&](FMaterialExpressionInput& Input) -> FMaterialGraphClipboardError {
			if (!Input.ExpressionId.IsValid()) return {};
			const auto It = Remap.find(Input.ExpressionId);
			if (It != Remap.end()) Input.ExpressionId = It->second;
			else if (!bSameRoot || std::ranges::none_of(State.Expressions, [&](const auto& E) { return E->Id == Input.ExpressionId; }))
				return {.Code = EMaterialGraphClipboardError::ExternalInput, .SourceId = Input.ExpressionId};
			return {};
		};
		std::vector<FGuid> Generated;
		for (const auto& Entry : Payload.Nodes)
		{
			const auto Duplicated = DuplicateObject(Entry.Expression.Get(), nullptr, NAME_None);
			TStrongObjectPtr<DMaterialExpression> Expression(Duplicated.Object);
			if (!Expression) return RejectClipboard({.Code = EMaterialGraphClipboardError::PasteObject, .NodeId = Entry.Expression->Id, .DuplicationCause = std::make_shared<FObjectGraphError>(Duplicated.Error)});
			Expression->Id = Remap.at(Entry.Expression->Id);
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get())) Input->Port = PortRemap.at(Input->Port.Id);
			if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) Output->Port = PortRemap.at(Output->Port.Id);
			if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			{
				Parameter->Metadata.Id = FGuid::NewGuid();
				if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return MakeParameterRejected(Error.Error);
			}
			FMaterialGraphClipboardError LinkError;
			VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) {
				auto Error = RemapLink(Input);
				if (Error.Code != EMaterialGraphClipboardError::None && LinkError.Code == EMaterialGraphClipboardError::None)
					LinkError = std::move(Error);
			});
			if (LinkError.Code != EMaterialGraphClipboardError::None)
			{
				LinkError.NodeId = Entry.Expression->Id;
				return RejectClipboard(std::move(LinkError));
			}
			const int64 PositionX = static_cast<int64>(X) + Entry.RelativeX, PositionY = static_cast<int64>(Y) + Entry.RelativeY;
			if (PositionX < -MaterialGraphPresentationCoordinateLimit || PositionX > MaterialGraphPresentationCoordinateLimit
				|| PositionY < -MaterialGraphPresentationCoordinateLimit || PositionY > MaterialGraphPresentationCoordinateLimit)
				return RejectClipboard({.Code = EMaterialGraphClipboardError::PastePosition, .NodeId = Entry.Expression->Id, .X = PositionX, .Y = PositionY, .CoordinateLimit = MaterialGraphPresentationCoordinateLimit});
			Generated.push_back(Expression->Id);
			State.Presentation.Nodes.push_back({Expression->Id, static_cast<int32>(PositionX), static_cast<int32>(PositionY), Entry.DisplayName});
			State.Expressions.emplace_back(Expression.Get());
		}
		if (Payload.bConnectAggregateSurface && !State.bFunction)
		{
			State.GetOutputs().Surface = {Remap.at(Payload.AggregateSourceNodeId), Payload.AggregateSourceOutputIndex, Payload.AggregateSourceOutputId};
		}
		auto Result = State.Commit("Paste Graph Nodes", Transactions);
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
		return FMaterialGraphDocument(Material).DuplicateNodes(NodeIds, OffsetX, OffsetY, Transactions);
	}

	auto FMaterialGraphDocument::DuplicateNodes(std::span<const FGuid> NodeIds,
		int32 OffsetX, int32 OffsetY, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphClipboardPayload Payload;
		const auto Copied = CopySelection(NodeIds, Payload);
		if (!Copied) return Copied;
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto& Presentation = Material ? Material->GetMaterialGraphPresentation().Nodes
			: Cast<DMaterialFunction>(Owner.Get())->GetFunctionPresentation().Nodes;
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit;
		int32 MinimumY = MaterialGraphPresentationCoordinateLimit;
		std::unordered_set<FGuid> Selected(Copied.AffectedNodeIds.begin(), Copied.AffectedNodeIds.end());
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
			return RejectClipboard({.Code = EMaterialGraphClipboardError::DuplicatePosition, .X = AnchorX, .Y = AnchorY, .CoordinateLimit = MaterialGraphPresentationCoordinateLimit});
		return Paste(Payload, static_cast<int32>(AnchorX),
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
