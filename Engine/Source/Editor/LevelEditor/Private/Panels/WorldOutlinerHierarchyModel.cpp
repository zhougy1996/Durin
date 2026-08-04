#include "Panels/WorldOutlinerHierarchyModel.h"

#include "Misc/StringHelper.h"

namespace Durin
{
	auto FWorldOutlinerHierarchyModel::Reset() -> void
	{
		Source = nullptr;
		Revision = 0;
		Nodes.clear();
		RootNodeIndices.clear();
		KeyToNode.clear();
		FilterVisibility.clear();
		CachedFilter.clear();
	}

	auto FWorldOutlinerHierarchyModel::Rebuild(const void* InSource, uint64 InRevision, std::span<const FInput> Inputs) -> void
	{
		Source = InSource;
		Revision = InRevision;
		Nodes.clear();
		RootNodeIndices.clear();
		KeyToNode.clear();
		FilterVisibility.clear();
		CachedFilter.clear();

		std::vector<FInput> SortedInputs(Inputs.begin(), Inputs.end());
		std::unordered_set<void*> SeenKeys;
		std::erase_if(SortedInputs, [&SeenKeys](const FInput& Input) { return Input.Key == nullptr || !SeenKeys.insert(Input.Key).second; });
		std::ranges::stable_sort(SortedInputs, [](const FInput& Left, const FInput& Right) { return Left.Name < Right.Name; });

		Nodes.reserve(SortedInputs.size());
		KeyToNode.reserve(SortedInputs.size());
		for (const FInput& Input : SortedInputs)
		{
			const uint32 NodeIndex = static_cast<uint32>(Nodes.size());
			Nodes.push_back({.Key = Input.Key, .Name = Input.Name, .TypeLabel = Input.TypeLabel});
			KeyToNode.emplace(Input.Key, NodeIndex);
		}

		for (uint32 NodeIndex = 0; NodeIndex < Nodes.size(); ++NodeIndex)
		{
			const FInput& Input = SortedInputs[NodeIndex];
			if (const auto It = KeyToNode.find(Input.ParentKey); It != KeyToNode.end() && It->second != NodeIndex)
				Nodes[NodeIndex].Parent = It->second;
		}

		// Break malformed parent cycles at one edge so every node remains reachable from a root.
		std::vector<uint8> VisitState(Nodes.size(), 0);
		std::vector<uint32> Path;
		Path.reserve(Nodes.size());
		for (uint32 StartIndex = 0; StartIndex < Nodes.size(); ++StartIndex)
		{
			if (VisitState[StartIndex] != 0) continue;
			Path.clear();
			uint32 NodeIndex = StartIndex;
			while (NodeIndex != InvalidNodeIndex && VisitState[NodeIndex] == 0)
			{
				VisitState[NodeIndex] = 1;
				Path.push_back(NodeIndex);
				NodeIndex = Nodes[NodeIndex].Parent;
			}
			if (NodeIndex != InvalidNodeIndex && VisitState[NodeIndex] == 1)
				Nodes[Path.back()].Parent = InvalidNodeIndex;
			for (uint32 PathNode : Path) VisitState[PathNode] = 2;
		}

		RootNodeIndices.reserve(Nodes.size());
		for (uint32 NodeIndex = 0; NodeIndex < Nodes.size(); ++NodeIndex)
		{
			const uint32 ParentIndex = Nodes[NodeIndex].Parent;
			if (ParentIndex == InvalidNodeIndex)
				RootNodeIndices.push_back(NodeIndex);
			else
				Nodes[ParentIndex].Children.push_back(NodeIndex);
		}

		uint32 TraversalPosition = 0;
		auto CacheTraversal = [&](auto&& Self, uint32 NodeIndex, uint32 Depth) -> void {
			FNode& Node = Nodes[NodeIndex];
			Node.Depth = Depth;
			Node.TraversalBegin = TraversalPosition++;
			for (uint32 ChildIndex : Node.Children) Self(Self, ChildIndex, Depth + 1);
			Node.TraversalEnd = TraversalPosition;
		};
		for (uint32 RootIndex : RootNodeIndices) CacheTraversal(CacheTraversal, RootIndex, 0);

		SetFilter({});
	}

	auto FWorldOutlinerHierarchyModel::SetFilter(std::string_view Filter) -> void
	{
		CachedFilter = Filter;
		FilterVisibility.assign(Nodes.size(), Filter.empty());
		if (Filter.empty()) return;

		auto CacheVisibility = [&](auto&& Self, uint32 NodeIndex) -> bool {
			bool bVisible = StringUtils::ContainsInsensitive(Nodes[NodeIndex].Name, Filter)
				|| StringUtils::ContainsInsensitive(Nodes[NodeIndex].TypeLabel, Filter);
			for (uint32 ChildIndex : Nodes[NodeIndex].Children)
				bVisible |= Self(Self, ChildIndex);
			FilterVisibility[NodeIndex] = bVisible;
			return bVisible;
		};
		for (uint32 RootIndex : RootNodeIndices) CacheVisibility(CacheVisibility, RootIndex);
	}

	auto FWorldOutlinerHierarchyModel::NeedsRebuild(const void* InSource, uint64 InRevision) const -> bool
	{
		return Source != InSource || Revision != InRevision;
	}

	auto FWorldOutlinerHierarchyModel::IsNodeVisible(uint32 NodeIndex) const -> bool
	{
		return NodeIndex < FilterVisibility.size() && FilterVisibility[NodeIndex] != 0;
	}

	auto FWorldOutlinerHierarchyModel::Contains(const void* Key) const -> bool
	{
		return FindNode(Key) != InvalidNodeIndex;
	}

	auto FWorldOutlinerHierarchyModel::IsDescendantOf(const void* Key, const void* CandidateAncestor) const -> bool
	{
		if (!Key || !CandidateAncestor || Key == CandidateAncestor) return false;
		const uint32 NodeIndex = FindNode(Key);
		const uint32 AncestorIndex = FindNode(CandidateAncestor);
		if (NodeIndex == InvalidNodeIndex || AncestorIndex == InvalidNodeIndex) return false;
		const FNode& Node = Nodes[NodeIndex];
		const FNode& Ancestor = Nodes[AncestorIndex];
		return Ancestor.TraversalBegin <= Node.TraversalBegin && Node.TraversalEnd <= Ancestor.TraversalEnd;
	}

	auto FWorldOutlinerHierarchyModel::GetDepth(const void* Key) const -> uint32
	{
		const uint32 NodeIndex = FindNode(Key);
		return NodeIndex != InvalidNodeIndex ? Nodes[NodeIndex].Depth : 0;
	}

	auto FWorldOutlinerHierarchyModel::FindNode(const void* Key) const -> uint32
	{
		const auto It = KeyToNode.find(Key);
		return It != KeyToNode.end() ? It->second : InvalidNodeIndex;
	}
} // namespace Durin
