#include "Panels/DetailsComponentTree.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Actor.h"
#include "Editor/EditorEngine.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::ClassDisplayName;
		using StringUtils::ContainsInsensitive;

		constexpr const char* ComponentDragPayload = "DURIN_DETAILS_SCENE_COMPONENT";
	} // namespace

	auto FDetailsComponentTree::ResetSelection() -> void
	{
		SelectedComponent = nullptr;
		ResetRenameState();
	}

	auto FDetailsComponentTree::ResetRenameState() -> void
	{
		RenamingComponent = nullptr;
		PendingExpandComponent = nullptr;
		RenameDialog.Cancel();
	}

	auto FDetailsComponentTree::Draw(FLevelEditorContext& Context, AActor* Actor) -> void
	{
		ImGui::TextDisabled("Components");
		ImGui::SameLine();
		bool bOpenAddPopup = false;
		bool bOpenDeletePopup = false;
		if (Context.bReadOnly) ImGui::BeginDisabled();
		if (ImGui::SmallButton("+ Add"))
		{
			AddComponentParent = nullptr;
			bAddComponentAsChild = false;
			ComponentTypeSearchText.fill(0);
			bOpenAddPopup = true;
		}
		if (Context.bReadOnly) ImGui::EndDisabled();

		std::unordered_set<DActorComponent*> OwnedComponents;
		std::vector<DActorComponent*> OwnedComponentOrder;
		std::unordered_map<DSceneComponent*, std::vector<DSceneComponent*>> SceneChildren;
		for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
		{
			if (DActorComponent* Component = ComponentPtr.Get(); Component && OwnedComponents.insert(Component).second)
				OwnedComponentOrder.push_back(Component);
		}
		for (DActorComponent* Component : OwnedComponentOrder)
		{
			if (auto* SceneComponent = Cast<DSceneComponent>(Component))
			{
				DSceneComponent* Parent = SceneComponent->GetAttachParent();
				if (Parent && OwnedComponents.contains(Parent)) SceneChildren[Parent].push_back(SceneComponent);
			}
		}

		auto QueueAddComponent = [&](DSceneComponent* Parent, bool bAsChild) {
			AddComponentParent = Parent;
			bAddComponentAsChild = bAsChild;
			ComponentTypeSearchText.fill(0);
			bOpenAddPopup = true;
		};
		auto BeginRenameComponent = [&](DActorComponent* Component) {
			if (!Component) return;
			SelectedComponent = Component;
			RenamingComponent = Component;
			RenameDialog.Open(Component->GetName());
		};
		auto DuplicateComponent = [&](DActorComponent* Source) {
			if (!Source || !Actor->IsInstanceComponent(Source)) return;
			DActorComponent* Duplicate = Actor->AddInstanceComponent(Source->GetClass(), Source->GetFName());
			if (!Duplicate)
			{
				Context.SetError(std::format("Failed to duplicate component '{}'.", Source->GetName()));
				return;
			}
			Duplicate->UnregisterComponent();
			std::string CopyError;
			const std::unordered_map<DObject*, DObject*> ReferenceMap{{Source, Duplicate}};
			if (!CopyEditableObjectProperties(Source, Duplicate, ReferenceMap, &CopyError))
			{
				Actor->DestroyInstanceComponent(Duplicate);
				Context.SetError(std::format("Failed to duplicate component '{}': {}", Source->GetName(), CopyError));
				return;
			}
			if (auto* SourceScene = Cast<DSceneComponent>(Source); SourceScene)
			{
				auto* DuplicateScene = Cast<DSceneComponent>(Duplicate);
				DSceneComponent* Parent = SourceScene->GetAttachParent();
				if (DuplicateScene && Parent && !DuplicateScene->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative))
				{
					Actor->DestroyInstanceComponent(Duplicate);
					Context.SetError(std::format("Failed to attach duplicated component '{}'.", Source->GetName()));
					return;
				}
				PendingExpandComponent = Parent;
			}
			Duplicate->RegisterComponent();
			SelectedComponent = Duplicate;
			Context.InvalidatePackageSavedState(Actor->GetPackage());
		};
		auto ReparentComponent = [&](DSceneComponent* Moving, DSceneComponent* Parent) {
			if (!Moving || !Parent || Moving == Actor->GetRootComponent() || Moving->GetOwner() != Actor || Parent->GetOwner() != Actor) return;
			if (!Moving->AttachToComponent(Parent, EAttachmentTransformRule::KeepWorld))
			{
				Context.SetError(std::format("Failed to attach '{}' to '{}'.", Moving->GetName(), Parent->GetName()));
				return;
			}
			PendingExpandComponent = Parent;
			Context.InvalidatePackageSavedState(Actor->GetPackage());
		};

		std::unordered_set<DActorComponent*> Visited;
		std::function<void(DActorComponent*)> DrawComponentNode;
		DrawComponentNode = [&](DActorComponent* Component) {
			if (!Component || !OwnedComponents.contains(Component) || !Visited.insert(Component).second) return;
			auto* SceneComponent = Cast<DSceneComponent>(Component);
			const auto ChildrenIt = SceneComponent ? SceneChildren.find(SceneComponent) : SceneChildren.end();
			const bool bHasChildren = ChildrenIt != SceneChildren.end() && !ChildrenIt->second.empty();
			const bool bIsRoot = Component == Actor->GetRootComponent();
			const bool bIsInstance = Actor->IsInstanceComponent(Component);
			ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
			if (!bHasChildren) Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (SelectedComponent.Get() == Component) Flags |= ImGuiTreeNodeFlags_Selected;

			ImGui::PushID(Component);
			if (PendingExpandComponent.Get() == Component)
			{
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				PendingExpandComponent = nullptr;
			}
			const std::string Status = bIsRoot ? std::format("Root, {}", bIsInstance ? "Instance" : "Default") : bIsInstance ? "Instance" : "Default";
			const std::string Label = std::format("{}  ({})  [{}]", Component->GetName(), ClassDisplayName(Component->GetClass()), Status);
			const bool bOpen = MonaImGui::CompactTreeNode("##Component", Flags, "%s", Label.c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) SelectedComponent = Component;
			if (ImGui::BeginPopupContextItem("ComponentContext"))
			{
				SelectedComponent = Component;
				if (Context.bReadOnly) ImGui::BeginDisabled();
				if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
				if (SceneComponent && ImGui::MenuItem("Add Child Component")) QueueAddComponent(SceneComponent, true);
				ImGui::Separator();
				if (ImGui::MenuItem("Rename", "F2")) BeginRenameComponent(Component);
				if (bIsInstance && !bIsRoot)
				{
					if (ImGui::MenuItem("Duplicate Component", "Ctrl+D")) DuplicateComponent(Component);
				}
				else
				{
					ImGui::BeginDisabled();
					ImGui::MenuItem(bIsRoot ? "Root component cannot be duplicated" : "Default component cannot be duplicated");
					ImGui::EndDisabled();
				}
				ImGui::Separator();
				if (bIsInstance)
				{
					if (ImGui::MenuItem("Delete Component", "Del"))
					{
						PendingDeleteComponent = Component;
						bOpenDeletePopup = true;
					}
				}
				else
				{
					ImGui::BeginDisabled();
					ImGui::MenuItem("Default component cannot be deleted");
					ImGui::EndDisabled();
				}
				if (Context.bReadOnly) ImGui::EndDisabled();
				ImGui::Separator();
				if (ImGui::MenuItem("Copy Component Name")) ImGui::SetClipboardText(Component->GetName().c_str());
				if (ImGui::MenuItem("Copy Component Type")) ImGui::SetClipboardText(Component->GetClass()->GetQualifiedName().ToString().c_str());
				ImGui::EndPopup();
			}

			if (!Context.bReadOnly && SceneComponent && !bIsRoot && ImGui::BeginDragDropSource())
			{
				DSceneComponent* PayloadComponent = SceneComponent;
				ImGui::SetDragDropPayload(ComponentDragPayload, &PayloadComponent, sizeof(PayloadComponent));
				ImGui::Text("Move %s", Component->GetName().c_str());
				ImGui::EndDragDropSource();
			}
			if (!Context.bReadOnly && SceneComponent && ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ComponentDragPayload))
				{
					if (Payload->DataSize == sizeof(DSceneComponent*))
					{
						auto* Moving = *static_cast<DSceneComponent* const*>(Payload->Data);
						ReparentComponent(Moving, SceneComponent);
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (bOpen && bHasChildren)
			{
				for (DSceneComponent* Child : ChildrenIt->second) DrawComponentNode(Child);
				ImGui::TreePop();
			}
			ImGui::PopID();
		};

		ImGuiTreeNodeFlags ActorFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
		if (!SelectedComponent) ActorFlags |= ImGuiTreeNodeFlags_Selected;
		if (OwnedComponents.empty()) ActorFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		ImGui::PushID(Actor);
		const bool bActorOpen = MonaImGui::CompactTreeNode("##Actor", ActorFlags, "%s  (%s)", Actor->GetName().c_str(), ClassDisplayName(Actor->GetClass()).c_str());
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) SelectedComponent = nullptr;
		if (ImGui::BeginPopupContextItem("ActorContext"))
		{
			if (Context.bReadOnly) ImGui::BeginDisabled();
			if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
			if (Context.bReadOnly) ImGui::EndDisabled();
			ImGui::EndPopup();
		}
		if (!Context.bReadOnly && ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ComponentDragPayload))
			{
				if (Payload->DataSize == sizeof(DSceneComponent*) && Actor->GetRootComponent())
				{
					auto* Moving = *static_cast<DSceneComponent* const*>(Payload->Data);
					ReparentComponent(Moving, Actor->GetRootComponent());
				}
			}
			ImGui::EndDragDropTarget();
		}
		if (bActorOpen && !OwnedComponents.empty())
		{
			DrawComponentNode(Actor->GetRootComponent());
			for (DActorComponent* Component : OwnedComponentOrder)
			{
				if (auto* SceneComponent = Cast<DSceneComponent>(Component))
				{
					DSceneComponent* Parent = SceneComponent->GetAttachParent();
					if (!Parent || !OwnedComponents.contains(Parent)) DrawComponentNode(Component);
				}
			}
			for (DActorComponent* Component : OwnedComponentOrder)
			{
				if (!Cast<DSceneComponent>(Component)) DrawComponentNode(Component);
			}
			ImGui::TreePop();
		}
		ImGui::PopID();

		if (ImGui::BeginPopupContextWindow("ComponentsContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			if (Context.bReadOnly) ImGui::BeginDisabled();
			if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
			if (Context.bReadOnly) ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		if (Context.bReadOnly)
		{
			bOpenAddPopup = false;
			bOpenDeletePopup = false;
			PendingDeleteComponent = nullptr;
			RenamingComponent = nullptr;
			RenameDialog.Cancel();
		}

		const ImGuiIO& IO = ImGui::GetIO();
		const bool bComponentsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (!Context.bReadOnly && bComponentsFocused && !IO.WantTextInput && SelectedComponent)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) BeginRenameComponent(SelectedComponent.Get());
			if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && Actor->IsInstanceComponent(SelectedComponent.Get()) && SelectedComponent.Get() != Actor->GetRootComponent()) DuplicateComponent(SelectedComponent.Get());
			if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && Actor->IsInstanceComponent(SelectedComponent.Get()))
			{
				PendingDeleteComponent = SelectedComponent;
				bOpenDeletePopup = true;
			}
		}
		DActorComponent* RenameTarget = RenamingComponent.Get();
		const EEditorRenameDialogResult RenameResult = RenameDialog.Draw("Rename Component", RenameTarget ? RenameTarget->GetName() : std::string_view(), [&](std::string_view NewName) -> std::string {
			if (!RenameTarget || !Actor->RenameComponent(RenameTarget, FName(NewName))) return "Failed to rename component.";
			Context.InvalidatePackageSavedState(Actor->GetPackage());
			return {};
		});
		if (RenameResult != EEditorRenameDialogResult::None) RenamingComponent = nullptr;
		if (bOpenAddPopup) ImGui::OpenPopup("Add Component");
		if (ImGui::BeginPopup("Add Component"))
		{
			ImGui::SetNextItemWidth(std::min(MonaImGui::ScaleUI(320.0f), ImGui::GetContentRegionAvail().x));
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##ComponentTypeSearch", "Search component types...", ComponentTypeSearchText.data(), ComponentTypeSearchText.size());
			DClass* RequiredBase = bAddComponentAsChild ? DSceneComponent::StaticClass() : DActorComponent::StaticClass();
			bool bFoundClass = false;
			for (DClass* Class : GetDerivedClasses(RequiredBase, true))
			{
				if (!CanConstructObjectOfClass(Class, RequiredBase)) continue;
				const std::string DisplayName = ClassDisplayName(Class);
				if (!ContainsInsensitive(DisplayName, ComponentTypeSearchText.data())) continue;
				bFoundClass = true;
				ImGui::PushID(Class);
				if (ImGui::Selectable(DisplayName.c_str()))
				{
					DActorComponent* NewComponent = Actor->AddInstanceComponent(Class, FName(Class->GetDefaultObjectName()));
					bool bSucceeded = NewComponent != nullptr;
					if (bSucceeded && bAddComponentAsChild)
					{
						auto* NewSceneComponent = Cast<DSceneComponent>(NewComponent);
						DSceneComponent* Parent = AddComponentParent.Get();
						bSucceeded = NewSceneComponent && Parent && NewSceneComponent->AttachToComponent(Parent, EAttachmentTransformRule::SnapToTarget);
						if (!bSucceeded)
							Actor->DestroyInstanceComponent(NewComponent);
						else
							PendingExpandComponent = Parent;
					}
					if (!bSucceeded)
					{
						Context.SetError(std::format("Failed to add component of class {}.", Class->GetQualifiedName().ToString()));
					}
					else
					{
						SelectedComponent = NewComponent;
						Context.InvalidatePackageSavedState(Actor->GetPackage());
						if (auto* NewSceneComponent = Cast<DSceneComponent>(NewComponent); !bAddComponentAsChild && NewSceneComponent)
							PendingExpandComponent = NewSceneComponent->GetAttachParent();
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::PopID();
			}
			if (!bFoundClass) ImGui::TextDisabled("No matching component types.");
			ImGui::EndPopup();
		}

		if (bOpenDeletePopup) ImGui::OpenPopup("Delete Component?");
		if (ImGui::BeginPopupModal("Delete Component?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::Text("Delete component '%s'?", PendingDeleteComponent ? PendingDeleteComponent->GetName().c_str() : "");
			ImGui::TextDisabled("Its scene children will remain on the actor and keep their world transforms.");
			ImGui::TextDisabled("This action cannot be undone.");
			if (ImGui::Button("Delete"))
			{
				DActorComponent* Component = PendingDeleteComponent.Get();
				if (Component && !Actor->DestroyInstanceComponent(Component))
					Context.SetError("Failed to delete component.");
				else if (Component)
					Context.InvalidatePackageSavedState(Actor->GetPackage());
				if (SelectedComponent.Get() == Component) SelectedComponent = nullptr;
				PendingDeleteComponent = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				PendingDeleteComponent = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
} // namespace Durin
