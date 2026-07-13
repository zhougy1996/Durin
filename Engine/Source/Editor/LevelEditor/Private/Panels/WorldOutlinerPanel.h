#pragma once

#include "DObject/ObjectPtr.h"
#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class AActor;
	class DLevel;
	class FWorldOutlinerPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "World Outliner"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		std::array<char, 128> SearchText{};
		std::array<char, 128> ActorTypeSearchText{};
		std::array<char, 128> RenameText{};
		std::array<char, 128> LevelRenameText{};
		TObjectPtr<AActor> RenamingActor;
		TObjectPtr<DLevel> DisplayedLevel;
		std::vector<TObjectPtr<AActor>> PendingDeleteActors;
		std::vector<AActor*> LastVisibleActors;
		std::unordered_map<AActor*, bool> ExpandedActors;
		bool bWasSearching = false;
		bool bLevelSelected = false;
		bool bRenamingLevel = false;
		int ExpandRequest = 0;
	};
} // namespace Durin
