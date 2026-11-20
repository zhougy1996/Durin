#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraphEditInternals.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/Material.h"
#include "MaterialGraph.h"
#include "MaterialGraphNodeDisplay.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{
		// The asset accepts a complete presentation, but history only owns the coordinates we write.
		auto ApplyPositions(const DMaterialGraph& Graph, std::span<const FMaterialGraphNodePresentation> Positions,
			FMaterialGraphPresentation& Presentation, bool AuthorDefaultPositions, std::vector<FNodePositionChange>* Changes = nullptr)
			-> FMaterialGraphCommandResult
		{
			std::unordered_map<FGuid, size_t> Indices;
			for (size_t I = 0; I < Presentation.Nodes.size(); ++I) Indices.emplace(Presentation.Nodes[I].NodeId, I);
			std::unordered_map<FGuid, const DMaterialGraphNode*> Nodes;
			for (const auto& Node : Graph.GetNodes()) Nodes.emplace(Node->GetView().Node.Id, Node.Get());
			for (const auto& Position : Positions)
			{
				const auto Node = Nodes.find(Position.NodeId);
				if (Node == Nodes.end()) return RejectCommand("A moved material graph node does not exist.");
				if (Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
					|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
					return RejectCommand("A material graph position is outside the supported coordinate range.");
				const auto It = Indices.find(Position.NodeId);
				const bool Authored = It != Indices.end();
				const auto& Before = Authored ? Presentation.Nodes[It->second] : Node->second->GetView().Presentation;
				if (Before.X == Position.X && Before.Y == Position.Y && (Authored || !AuthorDefaultPositions)) continue;
				if (Changes) Changes->push_back({Position.NodeId, Before.X, Before.Y, Position.X, Position.Y, Authored});
				if (Authored)
				{
					auto& Existing = Presentation.Nodes[It->second];
					Existing.X = Position.X;
					Existing.Y = Position.Y;
				}
				else Presentation.Nodes.push_back({Position.NodeId, Position.X, Position.Y});
			}
			return {};
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
			return Zoom > 0.48f ? EMaterialGraphDetailLevel::Readable : Previous;
		if (Previous == EMaterialGraphDetailLevel::Editing)
			return Zoom < 0.74f ? EMaterialGraphDetailLevel::Readable : Previous;
		if (Zoom < 0.42f) return EMaterialGraphDetailLevel::Overview;
		if (Zoom > 0.82f) return EMaterialGraphDetailLevel::Editing;
		return EMaterialGraphDetailLevel::Readable;
	}

	auto DMaterialGraph::MoveNodes(
		std::span<const FMaterialGraphNodePresentation> Positions,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		return MoveNodesInternal(Positions, Transactions);
	}

	auto DMaterialGraph::MoveNodesInternal(
		std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (Positions.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Positions.size() > MaterialProgramMaxNodeCount)
			return RejectCommand("The material graph move request exceeds the node bound.");
		FMaterialGraphPresentation Presentation = ReadGraphPresentation(*Owner.Get());
		std::unordered_set<FGuid> Requested;
		for (const auto& Position : Positions)
			if (!Requested.insert(Position.NodeId).second)
				return RejectCommand("A material graph move request contains a duplicate node GUID.");
		std::vector<FNodePositionChange> Changes;
		if (auto Result = ApplyPositions(*this, Positions, Presentation, false, &Changes); !Result) return Result;
		return CommitPresentationChange(*Owner.Get(), std::move(Presentation),
			"Move Material Nodes", std::move(Changes), Transactions);
	}

	auto DMaterialGraph::MoveMaterialOutput(
		int32 X,
		int32 Y,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto* Output = Material ? Material->GetOutputNode() : nullptr;
		if (!Output) return RejectCommand("The material output node is unavailable.");
		const FMaterialGraphNodePresentation Position{Output->Id, X, Y};
		return MoveNodesInternal(std::span(&Position, 1), *Admission);
	}

	auto DMaterialGraph::CalculateLayout(
		std::span<const FGuid> NodeIds,
		FMaterialGraphPresentation& OutPresentation) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		OutPresentation = ReadGraphPresentation(*Owner.Get());
		std::vector<FMaterialGraphNodePresentation> Positions;
		auto Calculated = CalculateLayoutPositions(NodeIds, OutPresentation, Positions);
		if (!Calculated) return Calculated;
		if (auto Result = ApplyPositions(*this, Positions, OutPresentation, true); !Result) return Result;
		for (const auto& Position : Positions) Calculated.AffectedNodeIds.push_back(Position.NodeId);
		std::ranges::sort(Calculated.AffectedNodeIds);
		return Calculated;
	}

	auto DMaterialGraph::CalculateLayoutPositions(std::span<const FGuid> NodeIds, const FMaterialGraphPresentation& Presentation,
		std::vector<FMaterialGraphNodePresentation>& Positions) const -> FMaterialGraphCommandResult
	{
		struct FLayoutNode { FGuid Id; std::vector<FGuid> Inputs; float Width = 224.0f; float Height = 94.0f; };
		std::vector<FLayoutNode> Nodes;
		for (const auto& GraphNode : GetNodes())
		{
			auto* Expression = GraphNode->MaterialExpression.Get();
			if (!Expression) continue;
			const auto& Visual = GraphNode->GetView();
			FLayoutNode Node{Visual.Node.Id, {}, GraphNodeWidth(Visual), GraphNodeHeight(Visual)};
			// Retained inputs still constrain layout when the current root mode hides their pins.
			VisitStoredMaterialInputs(*Expression, [&](const FMaterialExpressionInput& Input) {
				if (const auto* Source = Input.Expression.Get()) Node.Inputs.push_back(Source->Id);
			});
			Nodes.push_back(std::move(Node));
		}
		const auto FindLayoutNode = [&](const FGuid& Id) -> const FLayoutNode* {
			const auto It = std::ranges::find(Nodes, Id, &FLayoutNode::Id);
			return It == Nodes.end() ? nullptr : &*It;
		};
		if (NodeIds.size() > MaterialProgramMaxNodeCount)
			return RejectCommand("The material graph layout request exceeds the node bound.");
		std::unordered_set<FGuid> Requested(NodeIds.begin(), NodeIds.end());
		if (NodeIds.empty())
			for (const FLayoutNode& Node : Nodes)
				Requested.insert(Node.Id);
		if (Requested.size() != (NodeIds.empty() ? Nodes.size() : NodeIds.size()))
			return RejectCommand("The material graph layout request contains duplicate node GUIDs.");
		for (const FGuid& Id : Requested)
			if (!FindLayoutNode(Id))
				return RejectCommand("A material graph layout node does not exist.");

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
			std::ranges::sort(Nodes);
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
						return RejectCommand("The selected material graph layout has no collision-free placement.");
				}
				Positions.push_back({Id, static_cast<int32>(std::round(X)),
					static_cast<int32>(std::round(Y))});
				Occupied.push_back({X, Y, X + (Node ? Node->Width : Metrics.NodeWidth), Y + Height});
				Y += Height + Metrics.RowGap;
			}
		}
		return {};
	}

	auto DMaterialGraph::Layout(std::span<const FGuid> NodeIds,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto Presentation = ReadGraphPresentation(*Owner.Get());
		std::vector<FMaterialGraphNodePresentation> Positions;
		if (auto Result = CalculateLayoutPositions(NodeIds, Presentation, Positions); !Result) return Result;
		std::vector<FNodePositionChange> Changes;
		if (auto Result = ApplyPositions(*this, Positions, Presentation, true, &Changes); !Result) return Result;
		return CommitPresentationChange(*Owner.Get(), std::move(Presentation),
			"Layout Graph Nodes", std::move(Changes), Transactions);
	}
}
