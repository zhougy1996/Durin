#pragma once

#include "CoreMinimal.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "MaterialGraph.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/Material.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Testing
{
	inline auto CaptureGraphView(MaterialEditor::DMaterialGraph& Graph) -> MaterialEditor::FMaterialGraphView
	{
		MaterialEditor::FMaterialGraphView View{.Outputs = Graph.GetOutputs(), .bFunction = Graph.GetSchema().HasFunctionPorts()};
		View.Nodes.reserve(Graph.GetNodes().size());
		for (const auto& Node : Graph.GetNodes()) View.Nodes.push_back(Node->GetView());
		return View;
	}

	template<class TTerminal>
	auto FindFunctionTerminal(MaterialEditor::DMaterialGraph& Graph, const FGuid& PortId) -> MaterialEditor::DMaterialGraphNode*
	{
		for (const auto& Node : Graph.GetNodes())
			if (auto* Terminal = Cast<TTerminal>(Node->GetMaterialExpression()); Terminal && Terminal->Port.Id == PortId)
				return Node.Get();
		return nullptr;
	}

	template<class T> auto MakeGraphExpression(FGuid Id = FGuid::NewGuid()) -> TStrongObjectPtr<T>
	{
		TStrongObjectPtr<T> Expression(NewObject<T>(nullptr, NAME_None));
		Expression->Id = Id;
		return Expression;
	}
	// Replay can rebuild graph nodes. Capture selection identities while the returned nodes are live.
	inline auto GraphNodeId(MaterialEditor::DMaterialGraphNode* Node) -> FGuid { return Node->GetView().Node.Id; }
	inline auto ExpressionNodeId(DMaterialExpression* Expression) -> FGuid { return Expression->Id; }
	inline auto NodeIds(const std::vector<MaterialEditor::DMaterialGraphNode*>& Nodes) -> std::vector<FGuid>
	{
		std::vector<FGuid> Ids;
		for (const auto* Node : Nodes) Ids.push_back(Node->GetView().Node.Id);
		return Ids;
	}
	inline auto CreateGraphConstant(MaterialEditor::DMaterialGraph& Graph, float Value = 0.f,
		int32 X = 0, int32 Y = 0, bool Mode = false) -> MaterialEditor::TMaterialGraphResult<FGuid>
	{
		auto Constant = MakeGraphExpression<DMaterialExpressionScalarConstant>();
		Constant->Value = Value;
		return Graph.CreateExpression(*Constant.Get(), X, Y, Mode).transform(GraphNodeId);
	}
	inline auto CreateGraphCatalogNode(MaterialEditor::DMaterialGraph& Graph, const MaterialEditor::FMaterialGraphCatalogEntry& Entry,
		int32 X, int32 Y, FMaterialProgramLink Input, bool Mode = true)
		-> MaterialEditor::TMaterialGraphResult<std::vector<FGuid>>
	{
		const auto* Source = Input.SourceNodeId.IsValid() ? Graph.FindOutputGraphPin(Input) : nullptr;
		if (Input.SourceNodeId.IsValid() && !Source)
			return std::unexpected(MaterialEditor::FMaterialGraphCommandResult{.Status = MaterialEditor::EMaterialGraphCommandStatus::Rejected,
				.Message = "The source output no longer exists."});
		std::vector<MaterialEditor::DMaterialGraphNode*> AuxiliaryNodes;
		auto Created = MakeCreationAction(Entry).PerformAction(Graph, X, Y, Source, Mode, &AuxiliaryNodes);
		if (!Created) return std::unexpected(std::move(Created.error()));
		std::vector<FGuid> Ids{GraphNodeId(*Created)};
		for (auto* Node : AuxiliaryNodes) Ids.push_back(GraphNodeId(Node));
		return Ids;
	}
	inline auto CreateGraphCatalogNode(MaterialEditor::DMaterialGraph& Graph,
		EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type = EMaterialProgramValueType::Float,
		FMaterialProgramLink Input = {}, int32 X = 0, int32 Y = 0, bool Mode = false)
		-> MaterialEditor::TMaterialGraphResult<std::vector<FGuid>>
	{
		const auto Catalog = MaterialEditor::EnumerateCatalog();
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& Value) { return Value.Opcode == Opcode && Value.ResultType == Type; });
		if (Entry == Catalog.end()) return std::unexpected(MaterialEditor::FMaterialGraphCommandResult{.Status = MaterialEditor::EMaterialGraphCommandStatus::Rejected,
			.Message = "The catalog expression shape is stale."});
		return CreateGraphCatalogNode(Graph, *Entry, X, Y, Input, Mode);
	}
}
