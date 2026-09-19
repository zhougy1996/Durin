#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "Materials/MaterialExpressionEditing.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphNodeDisplay.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

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

	auto FMaterialGraphOperations::MoveNodes(DMaterial& Material,
		std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).MoveNodes(Positions, Transactions);
	}

	auto FMaterialGraphDocument::MoveNodes(
		std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto& Expressions = FMaterialExpressionEditing::GetExpressions(*Owner.Get());
		if (Positions.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Positions.size() > MaterialProgramMaxNodeCount)
			return RejectCommand("The material graph move request exceeds the node bound.");
		FMaterialGraphPresentation Presentation = ReadGraphPresentation(*Owner.Get());
		std::vector<FGuid> Affected;
		std::unordered_set<FGuid> RequestedNodes;
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			if (std::ranges::none_of(Expressions,
				[&](const auto& Expression) { return Expression->Id == Position.NodeId; }))
				return RejectCommand("A moved material graph node does not exist.");
			if (!RequestedNodes.insert(Position.NodeId).second)
				return RejectCommand("A material graph move request contains a duplicate node GUID.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit
				|| Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit
				|| Position.Y > MaterialGraphPresentationCoordinateLimit)
				return RejectCommand("A material graph position is outside the supported coordinate range.");
			auto It = std::ranges::find(Presentation.Nodes, Position.NodeId,
				&FMaterialGraphNodePresentation::NodeId);
			if (It == Presentation.Nodes.end()) Presentation.Nodes.push_back({Position.NodeId, Position.X, Position.Y});
			else { It->X = Position.X; It->Y = Position.Y; }
			Affected.push_back(Position.NodeId);
		}
		return CommitPresentationChange(*Owner.Get(), std::move(Presentation),
			"Move Material Nodes", std::move(Affected), Transactions);
	}

	auto FMaterialGraphOperations::MoveMaterialOutput(
		DMaterial& Material,
		int32 X,
		int32 Y,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		const auto* Output = Material.GetOutputNode();
		if (!Output) return RejectCommand("The material output node is unavailable.");
		const FMaterialGraphNodePresentation Position{Output->Id, X, Y};
		return MoveNodes(Material, std::span(&Position, 1), Transactions);
	}

	auto FMaterialGraphDocument::CalculateLayout(
		std::span<const FGuid> NodeIds,
		FMaterialGraphPresentation& OutPresentation) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		OutPresentation = ReadGraphPresentation(*Owner.Get());
		struct FLayoutNode { FGuid Id; std::vector<FGuid> Inputs; float Width = 224.0f; float Height = 94.0f; };
		std::vector<FLayoutNode> Nodes;
		const auto View = Inspect();
		for (const auto& Expression : FMaterialExpressionEditing::GetExpressions(*Owner.Get()))
		{
			const auto Visual = std::ranges::find(View.Nodes, Expression->Id,
				[](const auto& Item) { return Item.Node.Id; });
			if (Visual == View.Nodes.end()) continue;
			FLayoutNode Node{Expression->Id, {}, GraphNodeWidth(*Visual), GraphNodeHeight(*Visual)};
			VisitMaterialExpressionInputs(*Expression, [&](uint32, const FMaterialExpressionInput& Input) {
				if (Input.ExpressionId.IsValid()) Node.Inputs.push_back(Input.ExpressionId);
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

		FMaterialGraphPresentation Presentation = OutPresentation;
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
		std::vector<FMaterialGraphNodePresentation> Positions;
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
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			auto It = std::ranges::find(Presentation.Nodes, Position.NodeId,
				&FMaterialGraphNodePresentation::NodeId);
			if (It == Presentation.Nodes.end()) Presentation.Nodes.push_back(Position);
			else { It->X = Position.X; It->Y = Position.Y; }
		}
		std::vector<FGuid> Affected(Requested.begin(), Requested.end());
		OutPresentation = std::move(Presentation);
		std::ranges::sort(Affected);
		return {
			.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Affected),
		};
	}

	auto FMaterialGraphOperations::CalculateLayout(const DMaterial& Material,
		std::span<const FGuid> NodeIds, FMaterialGraphPresentation& OutPresentation) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(const_cast<DMaterial&>(Material)).CalculateLayout(NodeIds, OutPresentation);
	}

	auto FMaterialGraphOperations::Layout(DMaterial& Material, std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).Layout(NodeIds, Transactions);
	}

	auto FMaterialGraphDocument::Layout(std::span<const FGuid> NodeIds,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphPresentation Presentation;
		auto Calculated = CalculateLayout(NodeIds, Presentation);
		if (!Calculated) return Calculated;
		return CommitPresentationChange(*Owner.Get(), std::move(Presentation),
			"Layout Graph Nodes", std::move(Calculated.AffectedNodeIds), Transactions);
	}
}
