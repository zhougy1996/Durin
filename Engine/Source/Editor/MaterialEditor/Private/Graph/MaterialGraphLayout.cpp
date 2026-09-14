#include "MaterialGraphEditInternals.h"
#include "MaterialExpressionInputs.h"

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

	auto FMaterialGraphGeometry::GetSurfacePinOffset(uint32 InputIndex) -> float
	{
		const FMaterialGraphCanvasMetrics& Metrics = GetMetrics();
		return Metrics.SurfaceHeaderHeight
			+ Metrics.PinRowHeight * (static_cast<float>(InputIndex) + 0.5f);
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

	auto FMaterialGraphOperations::MoveNodes(
		DMaterial& Material,
		std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Positions.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Positions.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph move request exceeds the node bound.");
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::vector<FGuid> Affected;
		std::unordered_set<FGuid> RequestedNodes;
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			if (std::ranges::none_of(Material.GetExpressionCollection().Expressions,
				[&](const auto& Expression) { return Expression->Id == Position.NodeId; }))
				return MakeRejected("A moved material graph node does not exist.");
			if (!RequestedNodes.insert(Position.NodeId).second)
				return MakeRejected("A material graph move request contains a duplicate node GUID.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit
				|| Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit
				|| Position.Y > MaterialGraphPresentationCoordinateLimit)
				return MakeRejected("A material graph position is outside the supported coordinate range.");
			auto It = std::ranges::find(Presentation.Nodes, Position.NodeId,
				&FMaterialGraphNodePresentation::NodeId);
			if (It == Presentation.Nodes.end()) Presentation.Nodes.push_back(Position);
			else *It = Position;
			Affected.push_back(Position.NodeId);
		}
		return CommitPresentationChange(Material, std::move(Presentation),
			"Move Material Nodes", std::move(Affected), Transactions);
	}

	auto FMaterialGraphOperations::MoveMaterialOutput(
		DMaterial& Material,
		int32 X,
		int32 Y,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (X < -MaterialGraphPresentationCoordinateLimit
			|| X > MaterialGraphPresentationCoordinateLimit
			|| Y < -MaterialGraphPresentationCoordinateLimit
			|| Y > MaterialGraphPresentationCoordinateLimit)
			return MakeRejected(
				"The Surface position is outside the supported coordinate range.");
		FMaterialGraphPresentation Presentation =
			Material.GetMaterialGraphPresentation();
		Presentation.bHasMaterialOutputPosition = true;
		Presentation.MaterialOutputX = X;
		Presentation.MaterialOutputY = Y;
		return CommitPresentationChange(Material, std::move(Presentation),
			"Move Surface", {}, Transactions);
	}

	auto FMaterialGraphOperations::CalculateLayout(
		const DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphPresentation& OutPresentation) -> FMaterialGraphCommandResult
	{
		OutPresentation = Material.GetMaterialGraphPresentation();
		struct FLayoutNode { FGuid Id; std::vector<FGuid> Inputs; uint32 InputCount = 0; };
		std::vector<FLayoutNode> Nodes;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
		{
			FLayoutNode Node{Expression->Id, {}, Expression->GetAuthoredInputCount()};
			VisitMaterialExpressionInputs(*Expression, [&](uint32, const FMaterialExpressionInput& Input) {
				if (Input.ExpressionId.IsValid()) Node.Inputs.push_back(Input.ExpressionId);
			});
			Nodes.push_back(std::move(Node));
		}
		const auto FindLayoutNode = [&](const FGuid& Id) -> const FLayoutNode* {
			const auto It = std::ranges::find(Nodes, Id, &FLayoutNode::Id);
			return It == Nodes.end() ? nullptr : &*It;
		};
		const auto& Outputs = Material.GetExpressionOutputs();
		if (NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph layout request exceeds the node bound.");
		std::unordered_set<FGuid> Requested(NodeIds.begin(), NodeIds.end());
		if (NodeIds.empty())
			for (const FLayoutNode& Node : Nodes)
				Requested.insert(Node.Id);
		if (Requested.size() != (NodeIds.empty() ? Nodes.size() : NodeIds.size()))
			return MakeRejected("The material graph layout request contains duplicate node GUIDs.");
		for (const FGuid& Id : Requested)
			if (!FindLayoutNode(Id))
				return MakeRejected("A material graph layout node does not exist.");

		std::unordered_map<FGuid, std::vector<FGuid>> Consumers;
		for (const FLayoutNode& Node : Nodes)
			for (const FGuid& Input : Node.Inputs)
				Consumers[Input].push_back(Node.Id);
		std::unordered_set<FGuid> SurfaceSources{
			Outputs.BaseColor.ExpressionId,
			Outputs.Normal.ExpressionId,
			Outputs.Metallic.ExpressionId,
			Outputs.Roughness.ExpressionId,
			Outputs.AmbientOcclusion.ExpressionId,
			Outputs.Emissive.ExpressionId,
			Outputs.Opacity.ExpressionId,
			Outputs.OpacityMask.ExpressionId,
			Outputs.Surface.ExpressionId,
		};
		std::unordered_map<FGuid, uint32> DistanceToSink;
		std::function<uint32(const FGuid&)> Visit = [&](const FGuid& Id) -> uint32 {
			if (const auto It = DistanceToSink.find(Id); It != DistanceToSink.end())
				return It->second;
			uint32 Distance = SurfaceSources.contains(Id) ? 1u : 0u;
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

		const std::array SurfaceOrder{
			Outputs.BaseColor.ExpressionId,
			Outputs.Normal.ExpressionId,
			Outputs.Metallic.ExpressionId,
			Outputs.Roughness.ExpressionId,
			Outputs.AmbientOcclusion.ExpressionId,
			Outputs.Emissive.ExpressionId,
			Outputs.Opacity.ExpressionId,
			Outputs.OpacityMask.ExpressionId,
			Outputs.Surface.ExpressionId,
		};
		std::unordered_map<FGuid, size_t> SurfaceRanks;
		for (size_t Index = 0; Index < SurfaceOrder.size(); ++Index)
			SurfaceRanks.try_emplace(SurfaceOrder[Index], Index);
		if (!Columns.empty())
		{
			auto& SinkNodes = Columns.rbegin()->second;
			std::ranges::stable_sort(SinkNodes, [&](const FGuid& A, const FGuid& B) {
				const size_t RankA = SurfaceRanks.contains(A)
					? SurfaceRanks[A] : SurfaceOrder.size();
				const size_t RankB = SurfaceRanks.contains(B)
					? SurfaceRanks[B] : SurfaceOrder.size();
				return RankA == RankB ? A < B : RankA < RankB;
			});
			for (size_t Index = 0; Index < SinkNodes.size(); ++Index)
				Ranks[SinkNodes[Index]] = Index;
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

		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
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
						Existing.X + Metrics.NodeWidth,
						Existing.Y + FMaterialGraphGeometry::GetNodeHeight(
							Node->InputCount)});
				}
		std::vector<FMaterialGraphNodePresentation> Positions;
		for (auto& [Column, Nodes] : Columns)
		{
			float Y = 0.0f;
			for (const FGuid& Id : Nodes)
			{
				const FLayoutNode* Node = FindLayoutNode(Id);
				const float Height = FMaterialGraphGeometry::GetNodeHeight(
					Node ? Node->InputCount : 0u);
				const float X = Column * (Metrics.NodeWidth + Metrics.ColumnGap);
				for (uint32 Attempt = 0; Attempt <= MaterialProgramMaxNodeCount; ++Attempt)
				{
					const FRect Candidate{X, Y, X + Metrics.NodeWidth, Y + Height};
					const auto Collision = std::ranges::find_if(Occupied,
						[&](const FRect& Rect) {
							return Candidate.MinX < Rect.MaxX && Candidate.MaxX > Rect.MinX
								&& Candidate.MinY < Rect.MaxY && Candidate.MaxY > Rect.MinY;
						});
					if (Collision == Occupied.end()) break;
					Y = Collision->MaxY + Metrics.RowGap;
					if (Attempt == MaterialProgramMaxNodeCount)
						return MakeRejected("The selected material graph layout has no collision-free placement.");
				}
				Positions.push_back({Id, static_cast<int32>(std::round(X)),
					static_cast<int32>(std::round(Y))});
				Occupied.push_back({X, Y, X + Metrics.NodeWidth, Y + Height});
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
		if (NodeIds.empty())
		{
			bool bFound = false;
			float MaximumX = 0.0f;
			float MinimumY = 0.0f;
			float MaximumY = 0.0f;
			for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			{
				const FLayoutNode* Node = FindLayoutNode(Position.NodeId);
				if (!Node) continue;
				const float Y = static_cast<float>(Position.Y);
				const float Height = FMaterialGraphGeometry::GetNodeHeight(
					Node->InputCount);
				MaximumX = std::max(MaximumX,
					static_cast<float>(Position.X) + Metrics.NodeWidth);
				if (!bFound)
				{
					MinimumY = Y;
					MaximumY = Y + Height;
					bFound = true;
				}
				else
				{
					MinimumY = std::min(MinimumY, Y);
					MaximumY = std::max(MaximumY, Y + Height);
				}
			}
			const float OutputHeight = Metrics.SurfaceHeaderHeight
				+ Metrics.PinRowHeight
					* (Outputs.Surface.ExpressionId.IsValid() ? 1.0f : 8.0f)
				+ Metrics.BodyPadding;
			Presentation.bHasMaterialOutputPosition = true;
			Presentation.MaterialOutputX = static_cast<int32>(std::round(
				MaximumX + Metrics.ColumnGap));
			Presentation.MaterialOutputY = static_cast<int32>(std::round(
				bFound ? (MinimumY + MaximumY - OutputHeight) * 0.5f : 0.0f));
		}
		std::vector<FGuid> Affected(Requested.begin(), Requested.end());
		OutPresentation = std::move(Presentation);
		std::ranges::sort(Affected);
		return {
			.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Affected),
		};
	}

	auto FMaterialGraphOperations::Layout(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphPresentation Presentation;
		FMaterialGraphCommandResult Calculated = CalculateLayout(
			Material, NodeIds, Presentation);
		if (!Calculated) return Calculated;
		return CommitPresentationChange(Material, std::move(Presentation),
			"Layout Material Graph", std::move(Calculated.AffectedNodeIds), Transactions);
	}
}
