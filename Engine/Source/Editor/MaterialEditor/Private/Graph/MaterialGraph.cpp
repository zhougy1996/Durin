#include "MaterialGraph.h"
#include "MaterialGraphPinSource.h"
#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraphDocument.h"
#include "Materials/Material.h"
#include "DObject/ObjectLifecycle.h"
#include "Graph/MaterialGraphEditSession.h"
#include "Materials/MaterialExpressionInputs.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto IsOutput(const FMaterialGraphPinAddress& Pin) -> bool
		{
			return Pin.Kind == EMaterialGraphPinKind::Output || Pin.Kind == EMaterialGraphPinKind::FunctionOutput;
		}
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

	auto FMaterialGraphCreationAction::PerformAction(DMaterialGraph& Graph, int32 X, int32 Y,
		std::optional<FMaterialGraphPinAddress> Source, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		if (!Graph.GetMaterialOrFunction())
			return GraphEditInternals::RejectCommand("The graph owner is unavailable.", {}, EMaterialGraphCommandStatus::StaleOwner);
		return Graph.GetCommands().Create({*this, X, Y, Source}, Mode);
	}

	auto FMaterialGraphSchema::CanCreateConnection(const DMaterialGraph& Graph,
		const FMaterialGraphPinAddress& A, const FMaterialGraphPinAddress& B) const -> FMaterialGraphConnectionResponse
	{
		using R = EMaterialGraphConnectionResponse;
		if (!Graph.GetMaterialOrFunction()) return {R::Disallow, "The graph owner is no longer available."};
		if (IsOutput(A) == IsOutput(B)) return {R::Disallow, "Connect an output to an input."};
		const auto& Source = IsOutput(A) ? A : B;
		const auto& Target = IsOutput(A) ? B : A;
		if (!Source.NodeId.IsValid() || !IsSourceAddressValid(Source) || !IsSourceKeyValid(Source))
			return {R::Disallow, "The source pin key is invalid."};
		const FMaterialProgramLink Link{Source.NodeId, static_cast<uint8>(Source.Index), Source.PortId};
		const auto* Output = Graph.FindOutputPin(Link);
		const auto* Input = Graph.FindInputPin(Target);
		FMaterialGraphPinView AttributePin;
		if (Target.Kind == EMaterialGraphPinKind::AttributeOverride)
		{
			const auto* Node = Graph.FindNode(Target.NodeId);
			if (!Node || !Cast<DMaterialExpressionSetMaterialAttributes>(Node->GetMaterialExpression())
				|| Target.PortId.IsValid() || Target.Index >= FMaterialAttributeDefinitionMap::GetDefinitions().size())
				return {R::Disallow, "The attribute override is unavailable."};
			Input = Graph.FindInputPin(FMaterialGraphPinAddress::Input(Target.NodeId, Target.Index + 1));
			if (!Input)
			{
				const auto Type = GetMaterialSurfaceOutputType(static_cast<EMaterialProperty>(Target.Index));
				AttributePin.AcceptedTypes = {Type};
				if (Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4)
					AttributePin.AcceptedTypes.push_back(EMaterialProgramValueType::Float);
				Input = &AttributePin;
			}
		}
		if (!Input || !Output || Output->bMissing || Input->bMissing) return {R::Disallow, "The pin is unavailable."};
		if (Input->Link == Link) return {R::Make, {}};
		if (!IsConnectionTypeCompatible(*Input, Output->Type)) return {R::Disallow, "The pin types are incompatible."};
		std::vector<FGuid> Pending{Source.NodeId};
		std::unordered_set<FGuid> Visited;
		while (!Pending.empty())
		{
			const auto Id = Pending.back();
			Pending.pop_back();
			if (Id == Target.NodeId) return {R::Disallow, "The connection would create a cycle."};
			if (!Visited.insert(Id).second) continue;
			if (const auto* Node = Graph.FindNode(Id))
				for (const auto& Pin : Node->GetView().Inputs)
					if (Pin.Link.SourceNodeId.IsValid()) Pending.push_back(Pin.Link.SourceNodeId);
		}
		if (Input->Link.SourceNodeId.IsValid()) return {R::ReplaceInput, "The input already has a connection."};
		return {R::Make, {}};
	}

	auto FMaterialGraphSchema::TryCreateConnection(DMaterialGraph& Graph, const FMaterialGraphPinAddress& A,
		const FMaterialGraphPinAddress& B, bool Replace, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		if (!Graph.GetMaterialOrFunction()) return {.Status = EMaterialGraphCommandStatus::StaleOwner, .Message = "The graph owner is no longer available."};
		const auto Response = CanCreateConnection(Graph, A, B);
		if (!Response.Allows(Replace)) return {.Status = EMaterialGraphCommandStatus::Rejected, .Message = std::string(Response.Reason)};
		return Graph.LinkMaterialExpressionsFromGraph(IsOutput(A) ? B : A, IsOutput(A) ? A : B, Replace, Mode);
	}

	auto FMaterialGraphSchema::BreakPinLinks(DMaterialGraph& Graph, FMaterialGraphPinAddress Pin,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto* Owner = Graph.GetMaterialOrFunction();
		if (!Owner) return RejectCommand("The graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (!IsOutput(Pin))
		{
			if (Pin.Kind == EMaterialGraphPinKind::AttributeOverride)
			{
				// Choosing the base value clears both the connection and the inline override.
				return Graph.LinkMaterialExpressionsFromGraph(Pin, FMaterialGraphPinAddress::Output({}), true, Mode);
			}
			if (!Graph.FindInputPin(Pin)) return RejectCommand("The input pin is unavailable.");
			return Graph.LinkMaterialExpressionsFromGraph(Pin, FMaterialGraphPinAddress::Output({}), true, Mode);
		}
		const FMaterialProgramLink Link{Pin.NodeId, static_cast<uint8>(Pin.Index), Pin.PortId};
		if (!IsSourceAddressValid(Pin) || !IsSourceKeyValid(Pin) || !Graph.FindOutputPin(Link))
			return RejectCommand("The output pin is unavailable.");
		return Graph.BreakOutputLinks(Link, Mode);
	}

	auto DMaterialGraph::BreakOutputLinks(const FMaterialProgramLink& Link, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto* Owner = GetMaterialOrFunction();
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		const auto* Attributes = Cast<DMaterialExpressionGetMaterialAttributes>(FindNode(Link.SourceNodeId)->GetMaterialExpression());
		const auto* Attribute = Attributes ? FMaterialAttributeDefinitionMap::Find(Link.SourceOutputId) : nullptr;
		struct FConsumer
		{
			DMaterialExpression* Expression;
			FMaterialExpressionInput* Input;
			FGuid EmptyBinding;
		};
		std::vector<FConsumer> Consumers;
		std::vector<FGuid> Changed;
		for (const auto& Entry : FMaterialExpressionEditing::GetExpressions(*Owner))
		{
			auto& Expression = *Entry;
			const auto Before = Consumers.size();
			VisitMaterialExpressionInputs(Expression, [&](uint32 Index, FMaterialExpressionInput& Input) {
				if (Input.ExpressionId != Link.SourceNodeId) return;
				const bool SameOutput = Input.OutputId == Link.SourceOutputId && Input.OutputIndex == Link.SourceOutputIndex;
				const bool LegacyAttribute = Attribute && !Input.OutputId.IsValid() && Input.OutputIndex == static_cast<uint8>(Attribute->Property);
				if (!SameOutput && !LegacyAttribute) return;
				FGuid EmptyBinding;
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(&Expression); Call && Call->Inputs[Index].InputDefault.empty())
					EmptyBinding = Call->Inputs[Index].InputId;
				Consumers.push_back({&Expression, &Input, EmptyBinding});
			});
			if (Consumers.size() != Before) Changed.push_back(Expression.Id);
		}
		if (Consumers.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FMaterialGraphCommandResult Result;
		if (std::ranges::any_of(Consumers, [](const auto& Consumer) { return Consumer.EmptyBinding.IsValid(); }))
		{
			FGraphEditSession Edit(*Owner);
			for (const auto& Consumer : Consumers)
			{
				Edit.Modify(*Consumer.Expression);
				*Consumer.Input = {};
			}
			// Erase after all input pointers have been consumed.
			for (const auto& Consumer : Consumers)
				if (Consumer.EmptyBinding.IsValid())
					std::erase_if(Cast<DMaterialExpressionFunctionCall>(Consumer.Expression)->Inputs,
						[&](const auto& Binding) { return Binding.InputId == Consumer.EmptyBinding; });
			Result = Edit.Commit("Break Output Pin Links", *Admission);
		}
		else
		{
			FExpressionEdit Edit;
			for (const auto& Consumer : Consumers) Edit.SetLink(*Consumer.Expression, *Consumer.Input, {});
			Result = Edit.CommitInferred(*Owner, *Admission, "Break Output Pin Links");
		}
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded)
		{
			Changed.push_back(Link.SourceNodeId);
			Result.AffectedNodeIds = std::move(Changed);
		}
		return Result;
	}

	auto DMaterialGraphNode::GetMaterialExpression() const -> DMaterialExpression*
	{
		if (ActiveSource) return ActiveSource->Expression;
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Graph->FindNode(View.Node.Id) != this) return nullptr;
		return ResolveExpression(Graph->GetMaterialOrFunction(), View.Node.Id);
	}

	auto DMaterialGraphNode::ResolveInputGraph(const FMaterialGraphPinAddress& Pin) const -> DMaterialGraph*
	{
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Pin.NodeId != View.Node.Id || Graph->FindNode(Pin.NodeId) != this) return nullptr;
		// A material retains numeric property defaults while its root shows the Surface pin.
		if (View.Node.bMaterialOutput && Pin.Kind == EMaterialGraphPinKind::MaterialAttribute
			&& !Pin.PortId.IsValid() && Pin.Index < FMaterialAttributeDefinitionMap::GetDefinitions().size()) return Graph;
		return Graph->FindInputPin(Pin) ? Graph : nullptr;
	}

	auto DMaterialGraphNode::PinDefaultValueChanged(const FMaterialGraphPinAddress& Pin, FMaterialInputDefault Value,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto* Graph = ResolveInputGraph(Pin);
		if (!Graph) return GraphEditInternals::RejectCommand("The input pin is unavailable.");
		return GraphEditInternals::WriteInputDefault(Graph->GetMaterialOrFunction(),
			Pin.NodeId, Pin.Index, std::move(Value), Pin.PortId, Mode);
	}

	auto DMaterialGraphNode::SetPinUseConstant(const FMaterialGraphPinAddress& Pin, bool Enabled,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto* Graph = ResolveInputGraph(Pin);
		if (!Graph || Pin.PortId.IsValid()) return GraphEditInternals::RejectCommand("The numeric input pin is unavailable.");
		return GraphEditInternals::SetInputConstantEnabled(Graph->GetMaterialOrFunction(),
			Pin.NodeId, Pin.Index, Enabled, Mode);
	}

	auto DMaterialGraph::Create(DObject& MaterialOrFunction) -> DMaterialGraph*
	{
		if (!Cast<DMaterial>(&MaterialOrFunction) && !Cast<DMaterialFunction>(&MaterialOrFunction)) return nullptr;
		auto* Graph = NewObject<DMaterialGraph>(StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Graph->Owner = &MaterialOrFunction;
		Graph->Schema = &FMaterialGraphSchema::ForOwner(MaterialOrFunction);
		Graph->OwnerHandle = GetMaterialGraphChangeSource(MaterialOrFunction).Subscribe(MaterialOrFunction,
			[WeakGraph = TWeakObjectPtr<DMaterialGraph>(Graph)](const auto& Changes) {
				if (auto* Live = WeakGraph.Get()) Live->ApplyChanges(Changes);
			});
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

	auto DMaterialGraph::FindInputPin(const FMaterialGraphPinAddress& Address) const -> const FMaterialGraphPinView*
	{
		const auto* Node = FindNode(Address.NodeId);
		if (!Node) return nullptr;
		const auto& View = Node->GetView();
		for (const auto& Input : View.Inputs)
			if (View.InputAddress(Input) == Address) return &Input;
		return nullptr;
	}

	auto DMaterialGraph::LinkMaterialExpressionsFromGraph(const FMaterialGraphPinAddress& Input,
		const FMaterialGraphPinAddress& Output, bool Replace, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		return GraphEditInternals::WriteExpressionConnection(GetMaterialOrFunction(), Input, Output, Replace, Mode);
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
		if (ResolveExpression(GetMaterialOrFunction(), Expression.Id) != &Expression) return nullptr;
		if (auto* Node = FindNode(Expression.Id)) return Node;
		FMaterialGraphChangeSet Changes;
		Changes.MarkNode(Expression.Id, EMaterialGraphNodeChange::Added);
		ApplyChanges(std::move(Changes));
		return FindNode(Expression.Id);
	}

	auto DMaterialGraph::RebuildGraph() -> void
	{
		Nodes.clear();
		LinkGraphNodesFromMaterial();
	}

	auto DMaterialGraph::LinkGraphNodesFromMaterial() -> void
	{
		RefreshNodes(EnumerateCatalog());
		if (auto* Asset = Owner.Get()) ObserveDependencies(*Asset);
		FMaterialGraphChangeSet Changes;
		Changes.MarkGraph(EMaterialGraphChange::Reset);
		Synchronized.Broadcast(Changes);
	}

	auto DMaterialGraph::RefreshNodes(std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::optional<std::span<const FGuid>> Selected) -> void
	{
		std::unordered_map<FGuid, DMaterialGraphNode*> Existing;
		for (const auto& Node : Nodes) Existing.emplace(Node->GetView().Node.Id, Node.Get());
		std::vector<TObjectPtr<DMaterialGraphNode>> Updated;
		auto Summary = ReadMaterialGraph(GetMaterialOrFunction(), Catalog, Selected,
			[&](FMaterialGraphNodeView Metadata, const FMaterialGraphPinSource& Source) {
				const auto It = Existing.find(Metadata.Node.Id);
				const bool NewNode = It == Existing.end() || It->second->GetView().Node.bMaterialOutput != Metadata.Node.bMaterialOutput;
				auto* Type = Metadata.Node.bMaterialOutput ? DMaterialGraphNode_Root::StaticClass() : DMaterialGraphNode::StaticClass();
				auto* Node = NewNode ? NewObject<DMaterialGraphNode>(Type, this, NAME_None, EObjectFlags::Transient) : It->second;
				if (NewNode)
				{
					if (It != Existing.end()) std::erase_if(Nodes, [&](const auto& Value) { return Value.Get() == It->second; });
					Nodes.emplace_back(Node);
				}
				Node->Refresh(std::move(Metadata), Source, NewNode ? &DMaterialGraphNode::AllocateDefaultPins : &DMaterialGraphNode::ReconstructNode);
				if (!Selected) Updated.emplace_back(Node);
			});
		if (!Selected) Nodes = std::move(Updated);
		std::ranges::sort(Nodes, {}, [](const auto& Node) { return Node->GetView().Node.Id; });
		Outputs = std::move(Summary.Outputs);
	}
}
