#pragma once

#include "DObject/ObjectPtr.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/WorldOutlinerHierarchyModel.h"
#include "Widgets/EditorRenameDialog.h"

namespace Durin
{
	class AActor;
	class DLevel;
}

namespace Durin::Editor::Level
{

	// Builds and draws the searchable actor hierarchy for the active level.
	class FWorldOutlinerPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "World Outliner"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		auto ResetHierarchyCache() -> void;
		auto RebuildHierarchyCache(DLevel* Level) -> void;
		auto RebuildFilterCache(std::string_view Filter) -> void;
		auto IsNodeVisible(uint32 NodeIndex) const -> bool;
		auto IsDescendantOf(const AActor* Actor, const AActor* CandidateAncestor) const -> bool;
		auto GetActorDepth(const AActor* Actor) const -> uint32;
		auto SetActorVisibility(const std::vector<TObjectPtr<AActor>>& TargetActors, bool bHidden) -> void;
		auto ShowAllActors() -> void;
		auto AreAllActorsHidden(const std::vector<TObjectPtr<AActor>>& Actors) const -> bool;
		auto HasSelectedAncestor(const std::vector<TObjectPtr<AActor>>& Actors, AActor* Candidate) const -> bool;
		auto BeginActorRename(AActor* Actor) -> void;
		auto BeginLevelRename(std::string_view LevelName) -> void;
		auto DrawActorNode(FLevelEditorContext& Context, uint32 NodeIndex, std::string_view Filter, bool bRestoreExpansion, bool& bRequestDelete) -> void;
		auto DrawActorContextMenu(FLevelEditorContext& Context, AActor* Actor, bool bPrimaryCamera, bool& bRequestDelete) -> void;
		auto DrawActorDragDrop(FLevelEditorContext& Context, AActor* Actor) -> void;
		auto DrawLevelNode(FLevelEditorContext& Context, std::string_view LevelName, std::string_view Filter, bool bRestoreExpansion, bool& bRequestDelete) -> void;
		auto DrawLevelDragDrop(FLevelEditorContext& Context) -> void;
		auto DrawShortcuts(FLevelEditorContext& Context, std::string_view LevelName, bool& bRequestDelete) -> void;
		auto DrawRenameDialog(FLevelEditorContext& Context, std::string_view LevelName) -> void;
		auto DrawDeletePopup(FLevelEditorContext& Context) -> void;

		std::array<char, 128> SearchText{};
		std::array<char, 128> ActorTypeSearchText{};
		FEditorRenameDialog RenameDialog;
		TObjectPtr<AActor> RenamingActor;
		TObjectPtr<DLevel> DisplayedLevel;
		std::vector<TObjectPtr<AActor>> PendingDeleteActors;
		std::vector<AActor*> VisibleActors;
		std::vector<AActor*> LastVisibleActors;
		std::unordered_map<AActor*, bool> ExpandedActors;
		FWorldOutlinerHierarchyModel HierarchyModel;
		bool bWasSearching = false;
		bool bLevelSelected = false;
		bool bRenamingLevel = false;
		int ExpandRequest = 0;
	};
} // namespace Durin::Editor::Level