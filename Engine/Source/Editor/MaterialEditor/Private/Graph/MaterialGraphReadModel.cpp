#include "MaterialGraphReadModel.h"
#include <unordered_map>

namespace Durin::Editor::Material
{
	using N = EMaterialGraphNodeChange;
	using G = EMaterialGraphChange;

	FMaterialGraphReadModel::~FMaterialGraphReadModel()
	{
		UnsubscribeDependencies();
		if (auto* Owner = Document.Owner.Get()) GetMaterialGraphChangeSource(*Owner).Unsubscribe(Document.Handle);
	}
	auto FMaterialGraphReadModel::UnsubscribeDependencies() -> void
	{
		for (const auto& Subscription : Dependencies)
			if (auto* Owner = Subscription.Owner.Get()) GetMaterialGraphChangeSource(*Owner).Unsubscribe(Subscription.Handle);
		Dependencies.clear();
	}
	auto FMaterialGraphReadModel::ObserveDependencies(DObject& Owner) -> void
	{
		UnsubscribeDependencies();
		const auto* Material = Cast<DMaterial>(&Owner);
		const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions
			: Cast<DMaterialFunction>(&Owner)->GetExpressionCollection().Expressions;
		std::unordered_map<DMaterialFunction*, std::vector<FGuid>> Calls;
		for (const auto& Expression : Expressions)
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
				if (auto* Function = Cast<DMaterialFunction>(Call->Function.Get())) Calls[Function].push_back(Call->Id);
		for (auto& [Function, Ids] : Calls)
		{
			const auto Handle = Function->GetGraphChanges().Subscribe(*Function,
				[this, Nodes = std::move(Ids), Callee = TWeakObjectPtr<DMaterialFunction>(Function),
				 Signature = Function->GetFunctionSignature()](const FMaterialGraphChangeSet& Change) mutable {
					// A callee's body/layout is not part of the caller's inspection.
					// Compilation independently observes semantic dependency changes.
					if (!Change.Has(G::Reset) && std::ranges::none_of(Change.Nodes, [](const auto& Node) {
						return (Node.Flags & (N::Added | N::Removed | N::Interface)) != N::None;
					})) return;
					const auto* Current = Callee.Get();
					if (!Current) return;
					const auto& Updated = Current->GetFunctionSignature();
					if (!Change.Has(G::Reset) && Signature == Updated) return;
					Signature = Updated;
					for (const auto Id : Nodes) Pending.MarkNode(Id, N::Content | N::Interface);
				});
			Dependencies.push_back({TWeakObjectPtr<DObject>(Function), Handle});
		}
	}
	auto FMaterialGraphReadModel::Refresh(DObject& Owner,
		std::span<const FMaterialGraphCatalogEntry> Catalog) -> FMaterialGraphChangeSet
	{
		if (Document.Owner.Get() != &Owner)
		{
			UnsubscribeDependencies();
			if (auto* Previous = Document.Owner.Get()) GetMaterialGraphChangeSource(*Previous).Unsubscribe(Document.Handle);
			Document.Owner = &Owner;
			Document.Handle = GetMaterialGraphChangeSource(Owner).Subscribe(Owner,
				[this](const FMaterialGraphChangeSet& Change) { Pending.Merge(Change); });
			Pending.MarkGraph(G::Reset);
		}
		auto Changes = std::exchange(Pending, {});
		if (Changes.IsEmpty()) return Changes;
		const bool bStructure = Changes.Has(G::Reset)
			|| std::ranges::any_of(Changes.Nodes, [](const auto& Node) {
				return (Node.Flags & (N::Added | N::Removed | N::Interface | N::Inputs)) != N::None;
			});
		FMaterialGraphDocument Graph(Owner);
		if (bStructure)
		{
			View = Graph.Inspect(Catalog);
			ObserveDependencies(Owner);
		}
		else
		{
			std::vector<FGuid> Contents;
			for (const auto& Node : Changes.Nodes)
				if ((Node.Flags & N::Content) != N::None) Contents.push_back(Node.NodeId);
			if (!Contents.empty())
			{
				auto Updated = Graph.InspectNodes(Contents, Catalog);
				for (auto& Node : Updated.Nodes)
					if (auto It = std::ranges::find(View.Nodes, Node.Node.Id,
						[](const auto& Value) { return Value.Node.Id; }); It != View.Nodes.end()) *It = std::move(Node);
				View.Outputs = std::move(Updated.Outputs);
			}
			const auto* Material = Cast<DMaterial>(&Owner);
			const auto& Positions = Material ? Material->GetMaterialGraphPresentation().Nodes
				: Cast<DMaterialFunction>(&Owner)->GetFunctionPresentation().Nodes;
			const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions
				: Cast<DMaterialFunction>(&Owner)->GetExpressionCollection().Expressions;
			for (const auto& Change : Changes.Nodes)
			{
				if ((Change.Flags & N::Position) == N::None) continue;
				auto Node = std::ranges::find(View.Nodes, Change.NodeId, [](const auto& Value) { return Value.Node.Id; });
				if (Node == View.Nodes.end()) continue;
				const auto Position = std::ranges::find(Positions, Change.NodeId, &FMaterialGraphNodePresentation::NodeId);
				if (Position != Positions.end()) Node->Presentation = *Position;
				else
				{
					const auto Expression = std::ranges::find_if(Expressions, [&](const auto& E) { return E->Id == Change.NodeId; });
					const auto Ordinal = static_cast<int32>(std::distance(Expressions.begin(), Expression));
					Node->Presentation = {.NodeId = Change.NodeId, .X = Node->Node.bMaterialOutput ? 1280 : Ordinal % 4 * 320,
						.Y = Node->Node.bMaterialOutput ? 0 : Ordinal / 4 * 240};
				}
			}


		}
		return Changes;
	}
}
