#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraphEditInternals.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/Material.h"
#include "MaterialGraph.h"
#include "Materials/MaterialExpressionEditing.h"
#include "MaterialGraphNodeDisplay.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{
		auto ApplyPositions(const DMaterialGraph& Graph, std::span<const FMaterialGraphNodePresentation> Positions,
			FMaterialGraphPresentation& Presentation, std::string* OutError) -> bool
		{
			for (const auto& Position : Positions)
			{
				if (!Graph.FindNode(Position.NodeId))
					return FailWithError(OutError, "A moved material graph node does not exist.");
				if (Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
					|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
					return FailWithError(OutError, "A material graph position is outside the supported coordinate range.");
				const auto It = std::ranges::find(Presentation.Nodes, Position.NodeId, &FMaterialGraphNodePresentation::NodeId);
				if (It == Presentation.Nodes.end()) Presentation.Nodes.push_back(Position);
				else { It->X = Position.X; It->Y = Position.Y; }
			}
			return true;
		}

	}

	auto FMaterialGraphGeometry::GetMetrics()
		-> const FMaterialGraphCanvasMetrics&
	{
		static constexpr FMaterialGraphCanvasMetrics Metrics;
		return Metrics;
	}

	auto FMaterialGraphGeometry::GetNodeHeight(uint32 InputCount) -> float
	{
		const FMaterialGraphCanvasMetrics& Metrics = GetMetrics();
		return Metrics.HeaderHeight + Metrics.SecondaryHeight
			+ Metrics.BodyPadding * 2.0f
			+ Metrics.PinRowHeight * std::max(1u, InputCount);
	}

	auto FMaterialGraphGeometry::SelectDetailLevel(
		float Zoom,
		EMaterialGraphDetailLevel Previous) -> EMaterialGraphDetailLevel
	{
		if (Previous == EMaterialGraphDetailLevel::Overview)
			return Zoom > 0.48f ? EMaterialGraphDetailLevel::Editing : Previous;
		return Zoom < 0.42f ? EMaterialGraphDetailLevel::Overview : EMaterialGraphDetailLevel::Editing;
	}

	auto DMaterialGraph::MoveSelection(std::span<const FMaterialGraphNodePosition> Positions,
		bool bShouldTransact, std::string* OutError) const -> bool
	{
		std::vector<FMaterialGraphNodePresentation> Resolved;
		Resolved.reserve(Positions.size());
		for (const auto& Position : Positions)
		{
			const auto* Node = ResolveNode(Position.Node);
			if (!Node) return FailWithError(OutError, "A moved material graph node does not exist.");
			Resolved.push_back({Node->GetView().Node.Id, Position.X, Position.Y});
		}
		return MoveNodes(Resolved, bShouldTransact, OutError);
	}

	auto DMaterialGraph::MoveNodes(
		std::span<const FMaterialGraphNodePresentation> Positions,
		bool bShouldTransact, std::string* OutError) const -> bool
	{
		auto Admission = ResolveEditorTransactor(bShouldTransact);
		if (!Admission) return FailWithError(OutError, Admission.error());
		auto* Transactions = *Admission;
		return MoveNodesInternal(Positions, Transactions, OutError);
	}

	auto DMaterialGraph::MoveNodesInternal(
		std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Owner.IsValid()) return FailWithError(OutError, "The material graph owner is no longer available.");
		if (Positions.empty()) return true;
		if (Positions.size() > MaterialProgramMaxNodeCount + (GetRootNode() ? 1u : 0u))
			return FailWithError(OutError, "The material graph move request exceeds the node bound.");
		return CommitPositions(Positions, Transactions, false, "Move Material Nodes", OutError);
	}

	auto DMaterialGraph::CommitPositions(std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions, bool bAuthorDefaultPositions, std::string_view Description, std::string* OutError) const -> bool
	{
		auto& Asset = *Owner.Get();
		auto* Material = Cast<DMaterial>(&Asset);
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(Asset);
		std::unordered_set<FGuid> Requested;
		std::vector<std::pair<DMaterialExpression*, const FMaterialGraphNodePresentation*>> Changes;
		const FMaterialGraphNodePresentation* RootPosition = nullptr;
		for (const auto& Position : Positions)
		{
			if (!Requested.insert(Position.NodeId).second)
				return FailWithError(OutError, "A material graph move request contains a duplicate node GUID.");
			const auto* Node = FindNode(Position.NodeId);
			if (!Node) return FailWithError(OutError, "A moved material graph node does not exist.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
				return FailWithError(OutError, "A material graph position is outside the supported coordinate range.");
			if (Material && Position.NodeId == RootViewId)
			{
				if (Material->GetRootPosition() != std::pair{Position.X, Position.Y}) RootPosition = &Position;
				continue;
			}
			auto* Expression = FindMaterialExpression(Expressions, Position.NodeId);
			if (!Expression || Expression != Node->GetMaterialExpression())
				return FailWithError(OutError, "A moved material graph node no longer belongs to the asset.");
			const auto& Before = Node->GetView().Presentation;
			if (Before.X != Position.X || Before.Y != Position.Y || (bAuthorDefaultPositions && !Expression->EditorLayout.bAuthored))
				Changes.emplace_back(Expression, &Position);
		}
		if (!RootPosition && Changes.empty()) return true;
		if (Transactions && ResolveEditorTransactor(true).value_or(nullptr) != Transactions)
			return FailWithError(OutError, "The editor transactor is busy.");

		FScopedMaterialGraphChange Batch(Asset);
		const bool bNested = Transactions && Transactions->IsActive();
		FScopedTransaction Recording("MaterialGraph", Description, &Asset, Transactions && !bNested);
		if (Transactions)
		{
			if (!bNested && !Recording.IsOutstanding()) return FailWithError(OutError, "Unable to start the layout transaction.");
			FScopedPackageDirtySuppression Suppress;
			auto Record = [](DObject& Object) {
				Object.SetFlags(EObjectFlags::Transactional);
				return Object.Modify(false);
			};
			if ((RootPosition && !Record(*Material))
				|| !std::ranges::all_of(Changes, [&](const auto& Change) { return Record(*Change.first); }))
			{
				if (!bNested) Recording.Cancel();
				return FailWithError(OutError, "Unable to record the graph layout.");
			}
		}
		for (const auto& [Expression, Position] : Changes)
		{
			auto& Layout = Expression->EditorLayout;
			Layout.bAuthored = true;
			Layout.X = Position->X;
			Layout.Y = Position->Y;
		}
		if (RootPosition) Material->SetRootPosition(RootPosition->X, RootPosition->Y);
		else Asset.MarkPackageDirty();
		GetMaterialGraphChangeSource(Asset).PublishPresentation(Asset);
		return true;
	}

	auto DMaterialGraph::CalculateLayout(
		std::span<const FGuid> NodeIds,
		FMaterialGraphPresentation& OutPresentation, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Owner.IsValid()) return FailWithError(OutError, "The material graph owner is no longer available.");
		OutPresentation = ReadGraphPresentation(*Owner.Get());
		std::vector<FMaterialGraphNodePresentation> Positions;
		if (!CalculateLayoutPositions(NodeIds, OutPresentation, Positions, OutError)) return false;
		return ApplyPositions(*this, Positions, OutPresentation, OutError);
	}

	auto DMaterialGraph::CalculateLayoutPositions(std::span<const FGuid> NodeIds, const FMaterialGraphPresentation& Presentation,
		std::vector<FMaterialGraphNodePresentation>& Positions, std::string* OutError) const -> bool
	{
		struct FLayoutNode { FGuid Id; std::vector<FGuid> Inputs; float Width = 224.0f; float Height = 94.0f; };
		std::vector<FLayoutNode> Nodes;
		for (const auto& GraphNode : GetNodes())
		{
			auto* Expression = GraphNode->MaterialExpression.Get();
			const auto& Visual = GraphNode->GetView();
			FLayoutNode Node{Visual.Node.Id, {}, GraphNodeWidth(Visual), GraphNodeHeight(Visual)};
			// Retained inputs still constrain layout when the current root mode hides their pins.
			const auto AddInput = [&](const FMaterialExpressionInput& Input) {
				if (const auto* Source = Input.Expression.Get()) Node.Inputs.push_back(Source->Id);
			};
			if (Expression) VisitStoredMaterialInputs(*Expression, AddInput);
			else if (auto* Material = Cast<DMaterial>(Owner.Get())) FMaterialExpressionEditing::VisitRootConnections(*Material, AddInput);
			Nodes.push_back(std::move(Node));
		}
		const auto FindLayoutNode = [&](const FGuid& Id) -> const FLayoutNode* {
			const auto It = std::ranges::find(Nodes, Id, &FLayoutNode::Id);
			return It == Nodes.end() ? nullptr : &*It;
		};
		if (NodeIds.size() > MaterialProgramMaxNodeCount + (GetRootNode() ? 1u : 0u))
			return FailWithError(OutError, "The material graph layout request exceeds the node bound.");
		std::unordered_set<FGuid> Requested(NodeIds.begin(), NodeIds.end());
		if (NodeIds.empty())
			for (const FLayoutNode& Node : Nodes)
				Requested.insert(Node.Id);
		if (Requested.size() != (NodeIds.empty() ? Nodes.size() : NodeIds.size()))
			return FailWithError(OutError, "The material graph layout request contains duplicate node GUIDs.");
		for (const FGuid& Id : Requested)
			if (!FindLayoutNode(Id))
				return FailWithError(OutError, "A material graph layout node does not exist.");

		std::unordered_map<FGuid, std::vector<FGuid>> Consumers;
		for (const FLayoutNode& Node : Nodes)
			for (const FGuid& Input : Node.Inputs)
				Consumers[Input].push_back(Node.Id);
		std::unordered_map<FGuid, uint32> DistanceToSink;
		std::function<uint32(const FGuid&)> Visit = [&](const FGuid& Id) -> uint32 {
			if (const auto It = DistanceToSink.find(Id); It != DistanceToSink.end())
				return It->second;
			uint32 Distance = 0;
			if (const auto It = Consumers.find(Id); It != Consumers.end())
				for (const FGuid& Consumer : It->second)
					Distance = std::max(Distance, Visit(Consumer) + 1);
			DistanceToSink.emplace(Id, Distance);
			return Distance;
		};
		uint32 MaximumDistance = 0;
		for (const FLayoutNode& Node : Nodes)
			MaximumDistance = std::max(MaximumDistance, Visit(Node.Id));

		std::map<uint32, std::vector<FGuid>> Columns;
		for (const FGuid& Id : Requested)
			Columns[MaximumDistance - DistanceToSink[Id]].push_back(Id);
		std::unordered_map<FGuid, uint32> NodeColumns;
		std::unordered_map<FGuid, size_t> Ranks;
		for (auto& [Column, Nodes] : Columns)
		{
			std::ranges::sort(Nodes, [&](const FGuid& A, const FGuid& B) {
				return std::pair(A == RootViewId, A) < std::pair(B == RootViewId, B);
			});
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				NodeColumns.emplace(Nodes[Index], Column);
				Ranks[Nodes[Index]] = Index;
			}
		}

		auto Median = [&](const FGuid& Id, uint32 NeighborColumn,
			bool bUseInputs) -> float {
			std::vector<size_t> NeighborRanks;
			if (bUseInputs)
			{
				const FLayoutNode* Node = FindLayoutNode(Id);
				if (Node)
					for (const FGuid& Input : Node->Inputs)
						if (NodeColumns.contains(Input)
							&& NodeColumns[Input] == NeighborColumn)
							NeighborRanks.push_back(Ranks[Input]);
			}
			else if (const auto It = Consumers.find(Id); It != Consumers.end())
			{
				for (const FGuid& Consumer : It->second)
					if (NodeColumns.contains(Consumer)
						&& NodeColumns[Consumer] == NeighborColumn)
						NeighborRanks.push_back(Ranks[Consumer]);
			}
			if (NeighborRanks.empty()) return static_cast<float>(Ranks[Id]);
			std::ranges::sort(NeighborRanks);
			const size_t Middle = NeighborRanks.size() / 2;
			if (NeighborRanks.size() % 2) return static_cast<float>(NeighborRanks[Middle]);
			return (static_cast<float>(NeighborRanks[Middle - 1])
				+ static_cast<float>(NeighborRanks[Middle])) * 0.5f;
		};
		auto SortColumn = [&](uint32 Column, uint32 NeighborColumn,
			bool bUseInputs) {
			auto It = Columns.find(Column);
			if (It == Columns.end()) return;
			auto& Nodes = It->second;
			const auto PreviousRanks = Ranks;
			std::ranges::stable_sort(Nodes, [&](const FGuid& A, const FGuid& B) {
				const float MedianA = Median(A, NeighborColumn, bUseInputs);
				const float MedianB = Median(B, NeighborColumn, bUseInputs);
				if (MedianA != MedianB) return MedianA < MedianB;
				if (PreviousRanks.at(A) != PreviousRanks.at(B))
					return PreviousRanks.at(A) < PreviousRanks.at(B);
				return A < B;
			});
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				Ranks[Nodes[Index]] = Index;
		};
		if (!Columns.empty())
			for (uint32 Sweep = 0; Sweep < 4; ++Sweep)
			{
				for (auto It = std::next(Columns.begin()); It != Columns.end(); ++It)
					SortColumn(It->first, std::prev(It)->first, true);
				for (auto It = Columns.rbegin(); It != Columns.rend(); ++It)
				{
					const auto Next = std::next(It);
					if (Next != Columns.rend()) SortColumn(Next->first, It->first, false);
				}
			}

		const FMaterialGraphCanvasMetrics& Metrics = FMaterialGraphGeometry::GetMetrics();
		struct FRect { float MinX; float MinY; float MaxX; float MaxY; };
		std::vector<FRect> Occupied;
		if (!NodeIds.empty() && !Requested.contains(RootViewId))
			if (const auto* Root = GetRootNode())
			{
				const auto& View = Root->GetView();
				const auto& P = View.Presentation;
				Occupied.push_back({static_cast<float>(P.X), static_cast<float>(P.Y), P.X + GraphNodeWidth(View), P.Y + GraphNodeHeight(View)});
			}
		if (!NodeIds.empty())
			for (const FMaterialGraphNodePresentation& Existing : Presentation.Nodes)
				if (!Requested.contains(Existing.NodeId))
				{
					const FLayoutNode* Node = FindLayoutNode(Existing.NodeId);
					if (!Node) continue;
					Occupied.push_back({static_cast<float>(Existing.X), static_cast<float>(Existing.Y),
						Existing.X + Node->Width,
						Existing.Y + Node->Height});
				}
		for (auto& [Column, Nodes] : Columns)
		{
			float Y = 0.0f;
			for (const FGuid& Id : Nodes)
			{
				const FLayoutNode* Node = FindLayoutNode(Id);
				const float Height = Node ? Node->Height : Metrics.HeaderHeight;
				const float X = Column * (Metrics.NodeWidth + Metrics.ColumnGap);
				for (uint32 Attempt = 0; Attempt <= MaterialProgramMaxNodeCount; ++Attempt)
				{
					const FRect Candidate{X, Y, X + (Node ? Node->Width : Metrics.NodeWidth), Y + Height};
					const auto Collision = std::ranges::find_if(Occupied,
						[&](const FRect& Rect) {
							return Candidate.MinX < Rect.MaxX && Candidate.MaxX > Rect.MinX
								&& Candidate.MinY < Rect.MaxY && Candidate.MaxY > Rect.MinY;
						});
					if (Collision == Occupied.end()) break;
					Y = Collision->MaxY + Metrics.RowGap;
					if (Attempt == MaterialProgramMaxNodeCount)
						return FailWithError(OutError, "The selected material graph layout has no collision-free placement.");
				}
				Positions.push_back({Id, static_cast<int32>(std::round(X)),
					static_cast<int32>(std::round(Y))});
				Occupied.push_back({X, Y, X + (Node ? Node->Width : Metrics.NodeWidth), Y + Height});
				Y += Height + Metrics.RowGap;
			}
		}
		return true;
	}

	auto DMaterialGraph::Layout(std::span<const FGuid> NodeIds,
		bool bShouldTransact, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		auto Admission = ResolveEditorTransactor(bShouldTransact);
		if (!Admission) return FailWithError(OutError, Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return FailWithError(OutError, "The material graph owner is no longer available.");
		auto Presentation = ReadGraphPresentation(*Owner.Get());
		std::vector<FMaterialGraphNodePresentation> Positions;
		if (!CalculateLayoutPositions(NodeIds, Presentation, Positions, OutError)) return false;
		return CommitPositions(Positions, Transactions, true, "Layout Graph Nodes", OutError);
	}
}
