#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/ReflectedPropertyEditing.h"
#include "Panels/LevelEditorPanel.h"
#include "Widgets/EditorRenameDialog.h"

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
		auto DrawProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex, const std::string& Label) -> void;
		auto DrawPropertyValue(FLevelEditorContext& Context, DObject* Object, FProperty* Property, void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly, const FReflectedPropertyEditTarget& EditTarget, bool bUseTransaction = true) -> bool;
		auto DrawArrayProperty(FLevelEditorContext& Context, DObject* Object, class FArrayProperty* Property, void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly, const FReflectedPropertyEditTarget& EditTarget) -> bool;
		auto DrawMapProperty(FLevelEditorContext& Context, DObject* Object, class FMapProperty* Property, void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly, const FReflectedPropertyEditTarget& EditTarget) -> bool;
		auto SubmitPropertyEdit(FLevelEditorContext& Context, const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& ProposedValue, bool bContinuous) -> bool;
		auto FinishActivePropertyEdit(FLevelEditorContext* Context, bool bCancel) -> void;
		auto IsActivePropertyEdit(const FReflectedPropertyEditTarget& Target) const -> bool;

		std::array<char, 128> ComponentTypeSearchText{};
		std::array<char, 128> PropertySearchText{};
		TObjectPtr<DActorComponent> PendingRemoveComponent;
		TObjectPtr<DSceneComponent> AddComponentParent;
		TObjectPtr<DSceneComponent> PendingExpandComponent;
		TObjectPtr<AActor> PropertyActor;
		TObjectPtr<DActorComponent> SelectedComponent;
		TObjectPtr<DActorComponent> RenamingComponent;
		FEditorRenameDialog RenameDialog;
		std::array<char, 256> AssetSearchText{};
		FEditorSessionSettings& SessionSettings;
		FReflectedPropertyEditSession PropertyEditSession;
		DObject* ActiveEditObject = nullptr;
		float ComponentPaneRatio;
		bool bAddComponentAsChild = false;
	};
} // namespace Durin
