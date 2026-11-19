#include "MaterialGraph.h"
#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraphDocument.h"
#include "Materials/Material.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto ResolveExpression(DObject* Owner, const FGuid& Id) -> DMaterialExpression*
		{
			const auto* Material = Cast<DMaterial>(Owner);
			const auto* Function = Cast<DMaterialFunction>(Owner);
			if (!Material && !Function) return nullptr;
			const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions : Function->GetExpressionCollection().Expressions;
			for (const auto& Expression : Expressions)
				if (Expression.IsValid() && Expression->Id == Id) return Expression.Get();
			return nullptr;
		}
	}

	auto DMaterialGraphNode::GetMaterialExpression() const -> DMaterialExpression*
	{
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Graph->FindNode(View.Node.Id) != this) return nullptr;
		return ResolveExpression(Graph->GetMaterialOrFunction(), View.Node.Id);
	}

	auto DMaterialGraphNode::SetProjection(FMaterialGraphNodeView Projection) -> void
	{
		auto Inputs = std::move(Projection.Inputs);
		auto Outputs = std::move(Projection.Outputs);
		View = std::move(Projection);
		CreateInputPins(std::move(Inputs));
		CreateOutputPins(std::move(Outputs));
	}

	auto DMaterialGraphNode::AllocateDefaultPins() -> void
	{
		if (!GetMaterialExpression()) return;
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		auto Snapshot = FMaterialGraphDocument(*Graph->GetMaterialOrFunction()).InspectNodes(
			std::span(&View.Node.Id, 1), EnumerateCatalog());
		if (!Snapshot.Nodes.empty()) SetProjection(std::move(Snapshot.Nodes.front()));
	}

	auto DMaterialGraphNode::ReconstructNode() -> void { AllocateDefaultPins(); }

	auto DMaterialGraph::Create(DObject& MaterialOrFunction) -> DMaterialGraph*
	{
		if (!Cast<DMaterial>(&MaterialOrFunction) && !Cast<DMaterialFunction>(&MaterialOrFunction)) return nullptr;
		auto* Graph = NewObject<DMaterialGraph>(StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Graph->Owner = &MaterialOrFunction;
		Graph->Schema = &FMaterialGraphDocument(MaterialOrFunction).GetSchema();
		Graph->RebuildGraph();
		return Graph;
	}

	auto DMaterialGraph::GetSchema() const -> const FMaterialGraphSchema& { return *Schema; }

	auto DMaterialGraph::FindNode(const FGuid& Id) const -> DMaterialGraphNode*
	{
		for (const auto& Node : Nodes)
			if (Node->GetView().Node.Id == Id) return Node.Get();
		return nullptr;
	}

	auto DMaterialGraph::AppendNode(FMaterialGraphNodeView Projection) -> DMaterialGraphNode*
	{
		auto* Type = Projection.Node.bMaterialOutput ? DMaterialGraphNode_Root::StaticClass() : DMaterialGraphNode::StaticClass();
		auto* Node = NewObject<DMaterialGraphNode>(Type, this, NAME_None, EObjectFlags::Transient);
		Node->SetProjection(std::move(Projection));
		Nodes.emplace_back(Node);
		return Node;
	}

	auto DMaterialGraph::FindOutputPin(const FMaterialProgramLink& Link) const -> const FMaterialGraphOutputPinView*
	{
		const auto* Node = FindNode(Link.SourceNodeId);
		if (!Node) return nullptr;
		for (const auto& Pin : Node->GetView().Outputs)
			if (Pin.PortId == Link.SourceOutputId && (Pin.PortId.IsValid() || Pin.OutputIndex == Link.SourceOutputIndex))
				return &Pin;
		return nullptr;
	}

	auto DMaterialGraph::AddExpression(DMaterialExpression& Expression) -> DMaterialGraphNode*
	{
		if (ResolveExpression(Owner.Get(), Expression.Id) != &Expression) return nullptr;
		if (auto* Node = FindNode(Expression.Id)) return Node;
		auto Snapshot = FMaterialGraphDocument(*Owner.Get()).InspectNodes(std::span(&Expression.Id, 1), EnumerateCatalog());
		return Snapshot.Nodes.empty() ? nullptr : AppendNode(std::move(Snapshot.Nodes.front()));
	}

	auto DMaterialGraph::RebuildGraph() -> void
	{
		Nodes.clear();
		if (auto* Asset = Owner.Get())
			for (auto& Projection : FMaterialGraphDocument(*Asset).Inspect().Nodes)
				AppendNode(std::move(Projection));
	}

	auto DMaterialGraph::LinkGraphNodesFromMaterial() -> void
	{
		auto* Asset = Owner.Get();
		if (!Asset) { Nodes.clear(); return; }
		auto Snapshot = FMaterialGraphDocument(*Asset).Inspect();
		const bool SameNodes = std::ranges::equal(Nodes, Snapshot.Nodes, [](const auto& Existing, const auto& Projection)
		{
			const auto& Previous = Existing->GetView().Node;
			return Previous.Id == Projection.Node.Id && Previous.bMaterialOutput == Projection.Node.bMaterialOutput;
		});
		if (SameNodes)
		{
			for (size_t I = 0; I < Nodes.size(); ++I) Nodes[I]->SetProjection(std::move(Snapshot.Nodes[I]));
			return;
		}
		Nodes.clear();
		for (auto& Projection : Snapshot.Nodes) AppendNode(std::move(Projection));
	}
}
