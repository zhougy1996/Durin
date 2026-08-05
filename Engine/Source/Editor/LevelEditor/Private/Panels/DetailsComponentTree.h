#pragma once

#include "DObject/ObjectPtr.h"
#include "Widgets/EditorRenameDialog.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DSceneComponent;
	struct FLevelEditorContext;

	// Owns component-tree selection, mutation workflows, and immediate-mode component presentation.
	class FDetailsComponentTree final
	{
	public:
		auto Draw(FLevelEditorContext& Context, AActor* Actor) -> void;
		auto ResetRenameState() -> void;

	private:
		std::array<char, 128> ComponentTypeSearchText{};
		TObjectPtr<DActorComponent> PendingDeleteComponent;
		TObjectPtr<DSceneComponent> AddComponentParent;
		TObjectPtr<DSceneComponent> PendingExpandComponent;
		TObjectPtr<DActorComponent> RenamingComponent;
		FEditorRenameDialog RenameDialog;
		bool bAddComponentAsChild = false;
	};
} // namespace Durin
