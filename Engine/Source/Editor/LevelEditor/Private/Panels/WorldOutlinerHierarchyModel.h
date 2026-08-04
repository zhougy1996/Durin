#pragma once

namespace Durin
{
	// Builds a deterministic actor hierarchy projection without owning world or UI state.
	class FWorldOutlinerHierarchyModel
	{
	public:
		static constexpr uint32 InvalidNodeIndex = ~uint32{0};

		// Carries the stable identity and display metadata needed to build one hierarchy node.
		struct FInput
		{
			void* Key = nullptr;
			void* ParentKey = nullptr;
			std::string Name;
			std::string TypeLabel;
		};

		// Stores one flattened hierarchy row and indices into the same node array.
		struct FNode
		{
			void* Key = nullptr;
			std::string Name;
			std::string TypeLabel;
			std::vector<uint32> Children;
			uint32 Parent = InvalidNodeIndex;
			uint32 Depth = 0;
			uint32 TraversalBegin = 0;
			uint32 TraversalEnd = 0;
		};

		auto Reset() -> void;
		auto Rebuild(const void* Source, uint64 Revision, std::span<const FInput> Inputs) -> void;
		auto SetFilter(std::string_view Filter) -> void;
		auto NeedsRebuild(const void* Source, uint64 Revision) const -> bool;

		auto GetNodes() const -> std::span<const FNode> { return Nodes; }
		auto GetRootNodeIndices() const -> std::span<const uint32> { return RootNodeIndices; }
		auto GetFilter() const -> const std::string& { return CachedFilter; }
		auto Contains(const void* Key) const -> bool;
		auto IsNodeVisible(uint32 NodeIndex) const -> bool;
		auto IsDescendantOf(const void* Key, const void* CandidateAncestor) const -> bool;
		auto GetDepth(const void* Key) const -> uint32;

	private:
		auto FindNode(const void* Key) const -> uint32;

		const void* Source = nullptr;
		uint64 Revision = 0;
		std::vector<FNode> Nodes;
		std::vector<uint32> RootNodeIndices;
		std::unordered_map<const void*, uint32> KeyToNode;
		std::vector<uint8> FilterVisibility;
		std::string CachedFilter;
	};
} // namespace Durin
