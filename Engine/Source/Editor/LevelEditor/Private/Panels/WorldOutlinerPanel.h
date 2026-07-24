#pragma once

#include "DObject/ObjectPtr.h"
#include "Panels/LevelEditorPanel.h"
#include "Widgets/EditorRenameDialog.h"

namespace Durin
{
	class AActor;
	class DLevel;

	// Builds and draws the searchable actor hierarchy for the active level.
	class FWorldOutlinerPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "World Outliner"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		static constexpr uint32 InvalidNodeIndex = ~uint32{0};

		// Stores one flattened hierarchy row and indices into the same node array.
		struct FOutlinerNode
		{
			TObjectPtr<AActor> Actor;
			std::vector<uint32> Children;
			uint32 Parent = InvalidNodeIndex;
			uint32 Depth = 0;
			uint32 TraversalBegin = 0;
			uint32 TraversalEnd = 0;
		};

		auto ResetHierarchyCache() -> void;
		auto RebuildHierarchyCache(DLevel* Level) -> void;
		auto RebuildFilterCache(std::string_view Filter) -> void;
		auto IsNodeVisible(uint32 NodeIndex) const -> bool;
		auto IsDescendantOf(const AActor* Actor, const AActor* CandidateAncestor) const -> bool;
		auto GetActorDepth(const AActor* Actor) const -> uint32;

		std::array<char, 128> SearchText{};
		std::array<char, 128> ActorTypeSearchText{};
		FEditorRenameDialog RenameDialog;
		TObjectPtr<AActor> RenamingActor;
		TObjectPtr<DLevel> DisplayedLevel;
		std::vector<TObjectPtr<AActor>> PendingDeleteActors;
		std::vector<AActor*> VisibleActors;
		std::vector<AActor*> LastVisibleActors;
		std::unordered_map<AActor*, bool> ExpandedActors;
		std::vector<FOutlinerNode> HierarchyNodes;
		std::vector<uint32> RootNodeIndices;
		std::unordered_map<const AActor*, uint32> ActorToNode;
		std::vector<uint8> FilterVisibility;
		TObjectPtr<DLevel> CachedHierarchyLevel;
		std::string CachedFilter;
		uint64 CachedHierarchyRevision = 0;
		bool bFilterCacheValid = false;
		bool bWasSearching = false;
		bool bLevelSelected = false;
		bool bRenamingLevel = false;
		int ExpandRequest = 0;
	};
} // namespace Durin
