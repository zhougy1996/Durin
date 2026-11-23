#include "MaterialGraph.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionCall.h"

namespace Durin::MaterialEditor
{
	using N = EMaterialGraphNodeChange;
	using G = EMaterialGraphChange;

	auto DMaterialGraph::BeginDestroy() -> void
	{
		for (const auto& Node : Nodes)
			if (Node.IsValid()) Node->ClearPinLinks();
		UnsubscribeDependencies();
		if (auto* Asset = Owner.Get()) GetMaterialGraphChangeSource(*Asset).Unsubscribe(OwnerHandle);
		Super::BeginDestroy();
	}
	auto DMaterialGraph::UnsubscribeDependencies() -> void
	{
		for (const auto& Subscription : Dependencies)
			if (auto* Owner = Subscription.Owner.Get()) GetMaterialGraphChangeSource(*Owner).Unsubscribe(Subscription.Handle);
		Dependencies.clear();
	}
	auto DMaterialGraph::ObserveDependencies(DObject& Asset) -> void
	{
		UnsubscribeDependencies();
		const auto* Material = Cast<DMaterial>(&Asset);
		const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions
			: Cast<DMaterialFunction>(&Asset)->GetExpressionCollection().Expressions;
		std::unordered_map<DMaterialFunction*, std::vector<FGuid>> Calls;
		for (const auto& Expression : Expressions)
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
				if (auto* Function = Cast<DMaterialFunction>(Call->Function.Get())) Calls[Function].push_back(Call->Id);
		for (auto& [Function, Ids] : Calls)
		{
			const auto Handle = Function->GetGraphChanges().Subscribe(*Function,
				[WeakGraph = TWeakObjectPtr<DMaterialGraph>(this), Nodes = std::move(Ids), Callee = TWeakObjectPtr<DMaterialFunction>(Function),
				 Signature = std::make_shared<FMaterialFunctionSignature>(Function->GetFunctionSignature())](const FMaterialGraphChangeSet& Change) mutable {
					// A callee's body/layout is not part of the caller's inspection.
					// Compilation independently observes semantic dependency changes.
					if (!Change.Has(G::Reset) && std::ranges::none_of(Change.Nodes, [](const auto& Node) {
						return (Node.Flags & (N::Added | N::Removed | N::Interface)) != N::None;
					})) return;
					const auto* Current = Callee.Get();
					if (!Current) return;
					const auto& Updated = Current->GetFunctionSignature();
					if (!Change.Has(G::Reset) && *Signature == Updated) return;
					*Signature = Updated;
					if (auto* Graph = WeakGraph.Get())
					{
						FMaterialGraphChangeSet Changes;
						for (const auto Id : Nodes) Changes.MarkNode(Id, N::Content | N::Interface);
						Graph->ApplyChanges(std::move(Changes), false);
					}
				});
			Dependencies.push_back({TWeakObjectPtr<DObject>(Function), Handle});
		}
	}

	auto DMaterialGraph::ApplyChanges(FMaterialGraphChangeSet Changes, bool CheckDependencies) -> void
	{
		auto* Asset = Owner.Get();
		if (!Asset) return;
		if (Changes.Has(G::Reset))
		{
			RefreshNodes();
			ObserveDependencies(*Asset);
			Synchronized.Broadcast(Changes);
			return;
		}

		if (Changes.Has(G::RootInputs)) Changes.MarkNode(RootViewId, N::Content | N::Interface | N::Inputs);
		if (Changes.Has(G::RootPosition)) Changes.MarkNode(RootViewId, N::Position);
		// Rebind only when a call site was added, replaced, removed or retargeted.
		std::unordered_set<FGuid> ChangedOutputs;
		for (const auto& Change : Changes.Nodes)
			if ((Change.Flags & (N::Added | N::Removed | N::Interface)) != N::None)
				ChangedOutputs.insert(Change.NodeId);
		if (CheckDependencies && !ChangedOutputs.empty())
		{
			const auto* Material = Cast<DMaterial>(Asset);
			const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions
				: Cast<DMaterialFunction>(Asset)->GetExpressionCollection().Expressions;
			const bool HadCall = std::ranges::any_of(Nodes, [&](const auto& Node) {
				const auto& Info = Node->GetView().Node;
				return ChangedOutputs.contains(Info.Id) && Info.Opcode == EMaterialProgramOpcode::FunctionCall;
			});
			if (HadCall || std::ranges::any_of(Expressions, [&](const auto& Expression) {
				return ChangedOutputs.contains(Expression->Id) && Cast<DMaterialExpressionFunctionCall>(Expression.Get());
			})) ObserveDependencies(*Asset);
		}
		// Consumers also cache the resolved type of their source pin.
		if (!ChangedOutputs.empty())
		{
			for (const auto& Source : Nodes)
			{
				if (!ChangedOutputs.contains(Source->GetView().Node.Id)) continue;
				for (const auto& Pin : Source->GetPins())
					if (Pin.Direction == EMaterialGraphPinDirection::Output)
						for (const auto* Consumer : Pin.LinkedTo)
							Changes.MarkNode(Consumer->GetOwningNode()->GetView().Node.Id, N::Content);
			}
		}

		std::vector<FGuid> Reconstruct;
		for (const auto& Change : Changes.Nodes)
		{
			if ((Change.Flags & N::Removed) != N::None)
				std::erase_if(Nodes, [&](const auto& Node) {
					if (Node->GetView().Node.Id != Change.NodeId) return false;
					Node->ClearPinLinks();
					return true;
				});
			else if (Change.Flags != N::Position)
				Reconstruct.push_back(Change.NodeId);
			else if (auto* Node = FindNode(Change.NodeId))
			{
				const auto* Material = Cast<DMaterial>(Asset);
				if (Material && Node->View.Node.bMaterialOutput)
				{
					const auto [X, Y] = Material->GetRootPosition();
					Node->View.Presentation.X = X;
					Node->View.Presentation.Y = Y;
					continue;
				}
				const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions
					: Cast<DMaterialFunction>(Asset)->GetExpressionCollection().Expressions;
				const auto Expression = std::ranges::find_if(Expressions, [&](const auto& Entry) { return Entry && Entry->Id == Change.NodeId; });
				if (Expression == Expressions.end()) continue;
				const auto& Layout = (*Expression)->EditorLayout;
				Node->View.Presentation = Layout.bAuthored
					? FMaterialGraphNodePresentation{Change.NodeId, Layout.X, Layout.Y, Layout.DisplayName}
					: DefaultNodePresentation(Node->View.Node, std::distance(Expressions.begin(), Expression));
			}
		}
		if (!Reconstruct.empty()) RefreshNodes(Reconstruct);
		else if (!ChangedOutputs.empty()) RestorePinLinks();
		Synchronized.Broadcast(Changes);
	}
}
