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
		auto ResetSelection() -> void;
		auto ResetRenameState() -> void;
		auto SetSelectedComponent(DActorComponent* Component) -> void { SelectedComponent = Component; }
		auto GetSelectedComponent() const -> DActorComponent* { return SelectedComponent.Get(); }

	private:
		std::array<char, 128> ComponentTypeSearchText{};
		TObjectPtr<DActorComponent> PendingDeleteComponent;
		TObjectPtr<DSceneComponent> AddComponentParent;
		TObjectPtr<DSceneComponent> PendingExpandComponent;
		TObjectPtr<DActorComponent> SelectedComponent;
		TObjectPtr<DActorComponent> RenamingComponent;
		FEditorRenameDialog RenameDialog;
		bool bAddComponentAsChild = false;
	};
} // namespace Durin
