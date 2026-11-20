#pragma once

#include "CoreMinimal.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "MaterialGraph.h"
#include "MaterialGraphReadModel.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/Material.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Testing
{
	inline auto CaptureGraphView(MaterialEditor::DMaterialGraph& Graph) -> MaterialEditor::FMaterialGraphView
	{
		MaterialEditor::FMaterialGraphReadModel Reader(&Graph);
		Reader.Refresh(*Graph.GetMaterialOrFunction());
		return Reader.GetView();
	}

	template<class TTerminal>
	auto FindFunctionTerminal(MaterialEditor::DMaterialGraph& Graph, const FGuid& PortId) -> MaterialEditor::DMaterialGraphNode*
	{
		for (const auto& Node : Graph.GetNodes())
			if (auto* Terminal = Cast<TTerminal>(Node->GetMaterialExpression()); Terminal && Terminal->Port.Id == PortId)
				return Node.Get();
		return nullptr;
	}

	inline auto OutputPosition(const DMaterial& Material, FMaterialGraphPresentation& Presentation) -> FMaterialGraphNodePresentation&
	{
		const auto Id = Material.GetOutputNode()->Id;
		const auto It = std::ranges::find(Presentation.Nodes, Id, &FMaterialGraphNodePresentation::NodeId);
		if (It != Presentation.Nodes.end()) return *It;
		return Presentation.Nodes.emplace_back(FMaterialGraphNodePresentation{Id, 96, 0});
	}
	inline auto OutputPosition(const DMaterial& Material, const FMaterialGraphPresentation& Presentation) -> FMaterialGraphNodePresentation
	{
		const auto Id = Material.GetOutputNode()->Id;
		const auto It = std::ranges::find(Presentation.Nodes, Id, &FMaterialGraphNodePresentation::NodeId);
		return It != Presentation.Nodes.end() ? *It : FMaterialGraphNodePresentation{Id, 96, 0};
	}
	template<class T> auto MakeGraphExpression(FGuid Id = FGuid::NewGuid()) -> TStrongObjectPtr<T>
	{
		TStrongObjectPtr<T> Expression(NewObject<T>(nullptr, NAME_None));
		Expression->Id = Id;
		return Expression;
	}
	inline auto CreateGraphConstant(MaterialEditor::DMaterialGraph& Graph, float Value = 0.f,
		int32 X = 0, int32 Y = 0, ETransactionMode Mode = ETransactionMode::NotTransactable) -> MaterialEditor::FMaterialGraphCommandResult
	{
		auto Constant = MakeGraphExpression<DMaterialExpressionScalarConstant>();
		Constant->Value = Value;
		return Graph.CreateExpression(*Constant.Get(), X, Y, Mode);
	}
	inline auto CreateGraphCatalogNode(MaterialEditor::DMaterialGraph& Graph, const MaterialEditor::FMaterialGraphCatalogEntry& Entry,
		int32 X, int32 Y, FMaterialProgramLink Input, ETransactionMode Mode = ETransactionMode::Record)
		-> MaterialEditor::FMaterialGraphCommandResult
	{
		std::optional<MaterialEditor::FMaterialGraphPinAddress> Source;
		if (Input.SourceNodeId.IsValid()) Source = MaterialEditor::FMaterialGraphPinAddress::Output(Input);
		return MakeCreationAction(Entry).PerformAction(Graph, X, Y, Source, Mode);
	}
	inline auto CreateGraphCatalogNode(MaterialEditor::DMaterialGraph& Graph,
		EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type = EMaterialProgramValueType::Float,
		FMaterialProgramLink Input = {}, int32 X = 0, int32 Y = 0, ETransactionMode Mode = ETransactionMode::NotTransactable)
		-> MaterialEditor::FMaterialGraphCommandResult
	{
		const auto Catalog = MaterialEditor::EnumerateCatalog();
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& Value) { return Value.Opcode == Opcode && Value.ResultType == Type; });
		if (Entry == Catalog.end()) return {.Status = MaterialEditor::EMaterialGraphCommandStatus::Rejected,
			.Message = "The catalog expression shape is stale."};
		return CreateGraphCatalogNode(Graph, *Entry, X, Y, Input, Mode);
	}
}
