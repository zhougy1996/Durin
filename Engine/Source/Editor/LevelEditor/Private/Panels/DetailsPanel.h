#pragma once

#include "DObject/ObjectPtr.h"
#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DSceneComponent;
	class DObject;
	class FProperty;
	class FEditorSessionSettings;

	class FDetailsPanel final : public ILevelEditorPanel
	{
	public:
		explicit FDetailsPanel(FEditorSessionSettings& InSessionSettings);
		auto GetWindowName() const -> const char* override { return "Details"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		auto DrawComponents(FLevelEditorContext& Context, AActor* Actor) -> void;
		auto DrawReflectedProperties(FLevelEditorContext& Context, DObject* Object) -> void;
		auto DrawStaticMeshMaterials(FLevelEditorContext& Context, class DStaticMeshComponent* Component) -> void;
		auto DrawProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex) -> void;
		auto AssignObjectProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex, DObject* Value) -> bool;

		std::array<char, 128> ComponentTypeSearchText{};
		std::array<char, 128> PropertySearchText{};
		TObjectPtr<DActorComponent> PendingRemoveComponent;
		TObjectPtr<DSceneComponent> AddComponentParent;
		TObjectPtr<DSceneComponent> PendingExpandComponent;
		TObjectPtr<AActor> PropertyActor;
		TObjectPtr<DActorComponent> SelectedComponent;
		TObjectPtr<DActorComponent> RenamingComponent;
		std::array<char, 128> ComponentRenameText{};
		bool bFocusComponentRename = false;
		std::array<char, 256> AssetSearchText{};
		FEditorSessionSettings& SessionSettings;
		float ComponentPaneRatio;
		bool bAddComponentAsChild = false;
	};
} // namespace Durin
