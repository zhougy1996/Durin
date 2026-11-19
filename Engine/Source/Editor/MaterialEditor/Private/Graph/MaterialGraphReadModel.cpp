#include "MaterialGraphReadModel.h"

namespace Durin::MaterialEditor
{
	using N = EMaterialGraphNodeChange;
	using G = EMaterialGraphChange;

	FMaterialGraphReadModel::~FMaterialGraphReadModel()
	{
		if (BoundGraph) BoundGraph->Synchronized.Remove(Handle);
	}

	auto FMaterialGraphReadModel::Refresh(DObject& Owner) -> FMaterialGraphChangeSet
	{
		if (!Handle.IsValid() || !BoundGraph || BoundGraph->GetMaterialOrFunction() != &Owner)
		{
			if (BoundGraph) BoundGraph->Synchronized.Remove(Handle);
			if (!BoundGraph || BoundGraph->GetMaterialOrFunction() != &Owner) BoundGraph = DMaterialGraph::Create(Owner);
			Handle = BoundGraph->Synchronized.AddLambda([this](const auto& Changes) { Pending.Merge(Changes); });
			Pending.MarkGraph(G::Reset);
		}
		auto Changes = std::exchange(Pending, {});
		if (Changes.IsEmpty()) return Changes;
		View.Outputs = BoundGraph->Outputs;
		View.bFunction = BoundGraph->GetSchema().HasFunctionPorts();
		if (Changes.Has(G::Reset) || std::ranges::any_of(Changes.Nodes, [](const auto& Change) {
			return (Change.Flags & (N::Added | N::Removed)) != N::None;
		}))
		{
			View.Nodes.clear();
			for (const auto& Node : BoundGraph->GetNodes()) View.Nodes.push_back(Node->GetView());
		}
		else
		{
			for (const auto& Change : Changes.Nodes)
			{
				auto It = std::ranges::find(View.Nodes, Change.NodeId, [](const auto& Node) { return Node.Node.Id; });
				if (const auto* Node = BoundGraph->FindNode(Change.NodeId); Node && It != View.Nodes.end())
				{
					if (Change.Flags == N::Position) It->Presentation = Node->GetView().Presentation;
					else *It = Node->GetView();
				}
			}
		}
		return Changes;
	}
}
