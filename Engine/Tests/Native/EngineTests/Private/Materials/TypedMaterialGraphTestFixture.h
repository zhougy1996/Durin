#pragma once

#include "CoreMinimal.h"
#include "MaterialGraphDocument.h"
#include "Materials/Material.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Testing
{
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
	inline auto CreateGraphConstant(const MaterialEditor::FMaterialGraphDocument& Document, float Value = 0.f,
		int32 X = 0, int32 Y = 0, ETransactionMode Mode = ETransactionMode::NotTransactable) -> MaterialEditor::FMaterialGraphCommandResult
	{
		auto Constant = MakeGraphExpression<DMaterialExpressionScalarConstant>();
		Constant->Value = Value;
		return Document.CreateExpression(*Constant.Get(), X, Y, Mode);
	}
	inline auto CreateGraphCatalogNode(const MaterialEditor::FMaterialGraphDocument& Document,
		EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type = EMaterialProgramValueType::Float,
		FMaterialExpressionInput Input = {}, int32 X = 0, int32 Y = 0, ETransactionMode Mode = ETransactionMode::NotTransactable)
		-> MaterialEditor::FMaterialGraphCommandResult
	{
		const auto Catalog = MaterialEditor::FMaterialGraphOperations::EnumerateCatalog();
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& Value) { return Value.Opcode == Opcode && Value.ResultType == Type; });
		if (Entry == Catalog.end()) return {.Status = MaterialEditor::EMaterialGraphCommandStatus::Rejected,
			.Message = "The catalog expression shape is stale."};
		return Document.CreateCatalogNode(*Entry, X, Y, Input, Mode);
	}
}
