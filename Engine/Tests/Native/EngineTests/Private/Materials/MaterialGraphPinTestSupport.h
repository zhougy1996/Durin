#pragma once

#include "MaterialGraph.h"
#include "Graph/MaterialGraphCommands.h"
#include "Graph/MaterialEditorPreviewParameter.h"

namespace Durin::Testing
{
	using FTestExpressionInput = std::pair<FGuid, MaterialEditor::GraphEditInternals::FExpressionInputKey>;

	template<class TValue>
	auto WritePreviewParameter(DMaterial& Material, const FGuid& Id, const TValue& Value) -> bool
	{
		MaterialEditor::FMaterialEditorPreviewParameters Parameters(&Material);
		const auto Preview = Parameters.Find(Id);
		return Preview && PropertyEditor::FReflectedPropertyHandle(Preview->GetEditTarget()).SetValue(Value)
			== PropertyEditor::FPropertyAccess::Success;
	}

	inline auto ExpressionInput(FGuid Node, uint32 Index, FGuid Port = {}) -> FTestExpressionInput
	{
		if (Port.IsValid()) return {Node, Port};
		return {Node, Index};
	}

	inline auto ResolvePin(const MaterialEditor::DMaterialGraph& Graph, EMaterialProperty Property) -> const MaterialEditor::FMaterialGraphPin*
	{
		return Graph.FindRootPin(Property);
	}

	inline auto ResolvePin(const MaterialEditor::DMaterialGraph& Graph, const FMaterialProgramLink& Link) -> const MaterialEditor::FMaterialGraphPin*
	{
		return Graph.FindOutputGraphPin(Link);
	}

	inline auto ResolvePin(const MaterialEditor::DMaterialGraph& Graph, const FTestExpressionInput& Input) -> const MaterialEditor::FMaterialGraphPin*
	{
		const auto* Node = Graph.FindNode(Input.first);
		return Node ? std::visit([&](auto Key) { return Node->FindInputPin(Key); }, Input.second) : nullptr;
	}

	template<class T>
	auto ResolvePin(const MaterialEditor::DMaterialGraphNode& Node, const T& Key) -> const MaterialEditor::FMaterialGraphPin*
	{
		return ResolvePin(*Cast<MaterialEditor::DMaterialGraph>(Node.GetOuter()), Key);
	}

	template<class T>
	auto InputView(const MaterialEditor::DMaterialGraph& Graph, const T& Key) -> const MaterialEditor::FMaterialGraphPinView*
	{
		const auto* Pin = ResolvePin(Graph, Key);
		return Pin ? &Pin->GetOwningNode()->GetView().Inputs[Pin->DisplayIndex] : nullptr;
	}

	template<class A, class B>
	auto TryCreateConnection(MaterialEditor::DMaterialGraph& Graph, const A& First, const B& Second, bool Replace = false,
		bool Mode = true, std::string* Error = nullptr) -> bool
	{
		return Graph.GetSchema().TryCreateConnection(ResolvePin(Graph, First), ResolvePin(Graph, Second), Replace, Mode, Error);
	}

	inline auto FindExpression(DObject* Owner, FGuid Id) -> DMaterialExpression*
	{
		return Owner ? FindMaterialExpression(FMaterialExpressionEditing::GetExpressions(*Owner), Id) : nullptr;
	}

	inline auto WriteConnection(DObject* Owner, const FTestExpressionInput& Target, const FMaterialProgramLink& Source,
		bool Replace = false, bool Mode = true, std::string* Error = nullptr) -> bool
	{
		const auto Upstream = FindExpression(Owner, Source.SourceNodeId);
		return MaterialEditor::GraphEditInternals::WriteExpressionConnection(Owner, FindExpression(Owner, Target.first), Target.second,
			{Upstream, Source.SourceNodeId.IsValid() && !Upstream ? -1 : Source.SourceOutputIndex},
			Replace, Mode, Error, Source.SourceOutputId);
	}

	inline auto WriteConnection(DObject* Owner, EMaterialProperty Target, const FMaterialProgramLink& Source,
		bool Replace = false, bool Mode = true, std::string* Error = nullptr) -> bool
	{
		const auto Upstream = FindExpression(Owner, Source.SourceNodeId);
		return MaterialEditor::GraphEditInternals::WriteRootConnection(Cast<DMaterial>(Owner), Target,
			{Upstream, Source.SourceNodeId.IsValid() && !Upstream ? -1 : Source.SourceOutputIndex}, Replace, Mode, Error, Source.SourceOutputId);
	}

	inline auto WriteInputDefault(DObject* Owner, const FTestExpressionInput& Target, FMaterialInputDefault Value,
		bool Mode = true) -> MaterialEditor::FMaterialGraphCommandResult
	{
		return MaterialEditor::GraphEditInternals::WriteInputDefault(Owner, FindExpression(Owner, Target.first), Target.second, std::move(Value), Mode);
	}
}
