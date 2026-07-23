#include "Panels/DetailsPanel.h"
#include "Editor/ReflectedPropertyView.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "Engine/Actor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "LevelEditorCustomizations.h"
#include "Settings/EditorSessionSettings.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::ClassDisplayName;
		using StringUtils::ContainsInsensitive;

		constexpr const char* ComponentDragPayload = "DURIN_DETAILS_SCENE_COMPONENT";

	} // namespace

	FDetailsPanel::FDetailsPanel(FEditorSessionSettings& InSessionSettings)
		: SessionSettings(InSessionSettings)
		, ComponentPaneRatio(InSessionSettings.GetDetailsPaneRatio())
	{
	}

	FDetailsPanel::~FDetailsPanel()
	{
		PropertyView.FinishActiveEdit(nullptr, true);
	}

	auto FDetailsPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!EditorWorkspaceUI::BeginDockablePanel(LevelEditorWorkspace::Type, "Details", "Details", GetOpenPtr()))
		{
			if (!FinishActivePropertyEdit(&Context, true)) SetOpen(true);
			ImGui::End();
			return;
		}

		AActor* Actor = Context.GetPrimarySelectedActor();
		if (Actor == nullptr)
		{
			if (!FinishActivePropertyEdit(&Context, true))
			{
				ImGui::TextDisabled("The active property preview must be restored before changing selection.");
				ImGui::End();
				return;
			}
			PropertyActor = nullptr;
			SelectedComponent = nullptr;
			RenamingComponent = nullptr;
			RenameDialog.Cancel();
			ImGui::TextDisabled("Select an actor to inspect it.");
			ImGui::End();
			return;
		}

		if (PropertyActor.Get() != Actor)
		{
			if (!FinishActivePropertyEdit(&Context, true))
			{
				Actor = PropertyActor.Get();
			}
			else
			{
				PropertyActor = Actor;
				SelectedComponent = nullptr;
				RenamingComponent = nullptr;
				RenameDialog.Cancel();
				PendingExpandComponent = nullptr;
			}
		}
		if (!Actor)
		{
			ImGui::End();
			return;
		}
		if (SelectedComponent && std::ranges::none_of(Actor->GetOwnedComponents(), [this](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == SelectedComponent.Get(); }))
		{
			SelectedComponent = nullptr;
		}

		ImGui::TextUnformatted(Actor->GetName().c_str());
		ImGui::TextDisabled("%s", Actor->GetClass()->GetName().c_str());
		if (Context.bReadOnly) ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning), "Runtime values (read-only)");
		if (Context.GetSelectedActors().size() > 1)
		{
			ImGui::TextDisabled("%zu actors selected; editing primary actor only.", Context.GetSelectedActors().size());
		}
		const float SplitterThickness = MonaImGui::GetUIStyleMetrics().SplitterThickness;
		const float AvailableHeight = ImGui::GetContentRegionAvail().y;
		const float UsableHeight = FMath::Max(AvailableHeight - SplitterThickness, 0.0f);
		float ComponentHeight = UsableHeight * ComponentPaneRatio;
		if (UsableHeight >= MonaImGui::ScaleUI(220.0f))
		{
			ComponentHeight = std::clamp(ComponentHeight, MonaImGui::ScaleUI(90.0f), UsableHeight - MonaImGui::ScaleUI(120.0f));
		}

		if (ImGui::BeginChild("DetailsComponents", ImVec2(0.0f, ComponentHeight), ImGuiChildFlags_Borders))
		{
			DrawComponents(Context, Actor);
		}
		ImGui::EndChild();

		if (MonaImGui::DrawSplitter("DetailsSplitter", MonaImGui::EUISplitterAxis::Y, ImGui::GetContentRegionAvail().x, UsableHeight, MonaImGui::ScaleUI(90.0f), MonaImGui::ScaleUI(120.0f), ComponentPaneRatio))
			SessionSettings.SetDetailsPaneRatio(ComponentPaneRatio);

		if (ImGui::BeginChild("DetailsProperties", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
		{
			DObject* InspectedObject = SelectedComponent ? static_cast<DObject*>(SelectedComponent.Get()) : static_cast<DObject*>(Actor);
			if (Context.bReadOnly) ImGui::BeginDisabled();
			DrawReflectedProperties(Context, InspectedObject);
			if (Context.bReadOnly) ImGui::EndDisabled();
		}
		ImGui::EndChild();
		ImGui::End();
	}

	auto FDetailsPanel::DrawComponents(FLevelEditorContext& Context, AActor* Actor) -> void
	{
		ImGui::TextDisabled("Components");
		ImGui::SameLine();
		bool bOpenAddPopup = false;
		bool bOpenRemovePopup = false;
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
			{
				OwnedComponentOrder.push_back(Component);
			}
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
		};
		auto ReparentComponent = [&](DSceneComponent* Moving, DSceneComponent* Parent) {
			if (!Moving || !Parent || Moving == Actor->GetRootComponent() || Moving->GetOwner() != Actor || Parent->GetOwner() != Actor) return;
			if (!Moving->AttachToComponent(Parent, EAttachmentTransformRule::KeepWorld))
			{
				Context.SetError(std::format("Failed to attach '{}' to '{}'.", Moving->GetName(), Parent->GetName()));
				return;
			}
			PendingExpandComponent = Parent;
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
			const std::string Status = bIsRoot ? std::format("Root, {}", bIsInstance ? "Instance" : "Default") : bIsInstance ? "Instance" :
																															   "Default";
			const std::string Label = std::format("{}  ({})  [{}]", Component->GetName(), ClassDisplayName(Component->GetClass()), Status);
			const bool bOpen = MonaImGui::CompactTreeNode("##Component", Flags, "%s", Label.c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) SelectedComponent = Component;
			if (ImGui::BeginPopupContextItem("ComponentContext"))
			{
				if (Context.bReadOnly) ImGui::BeginDisabled();
				if (ImGui::MenuItem("Rename", "F2")) BeginRenameComponent(Component);
				if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
				if (SceneComponent && ImGui::MenuItem("Add Child Component")) QueueAddComponent(SceneComponent, true);
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
				if (bIsInstance && !bIsRoot)
				{
					if (ImGui::MenuItem("Remove Component"))
					{
						PendingRemoveComponent = Component;
						bOpenRemovePopup = true;
					}
				}
				else
				{
					ImGui::BeginDisabled();
					ImGui::MenuItem(bIsRoot ? "Root component cannot be removed" : "Default component cannot be removed");
					ImGui::EndDisabled();
				}
				if (Context.bReadOnly) ImGui::EndDisabled();
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
				for (DSceneComponent* Child : ChildrenIt->second)
					DrawComponentNode(Child);
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
			bOpenRemovePopup = false;
			PendingRemoveComponent = nullptr;
			RenamingComponent = nullptr;
			RenameDialog.Cancel();
		}

		const ImGuiIO& IO = ImGui::GetIO();
		const bool bComponentsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (!Context.bReadOnly && bComponentsFocused && !IO.WantTextInput && SelectedComponent)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) BeginRenameComponent(SelectedComponent.Get());
			if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && Actor->IsInstanceComponent(SelectedComponent.Get()) && SelectedComponent.Get() != Actor->GetRootComponent()) DuplicateComponent(SelectedComponent.Get());
			if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && Actor->IsInstanceComponent(SelectedComponent.Get()) && SelectedComponent.Get() != Actor->GetRootComponent())
			{
				PendingRemoveComponent = SelectedComponent;
				bOpenRemovePopup = true;
			}
		}
		DActorComponent* RenameTarget = RenamingComponent.Get();
		const EEditorRenameDialogResult RenameResult = RenameDialog.Draw("Rename Component", RenameTarget ? RenameTarget->GetName() : std::string_view(), [&](std::string_view NewName) -> std::string {
			return RenameTarget && Actor->RenameComponent(RenameTarget, FName(NewName)) ? std::string() : "Failed to rename component.";
		});
		if (RenameResult != EEditorRenameDialogResult::None)
		{
			RenamingComponent = nullptr;
		}
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
						if (auto* NewSceneComponent = Cast<DSceneComponent>(NewComponent); !bAddComponentAsChild && NewSceneComponent)
						{
							PendingExpandComponent = NewSceneComponent->GetAttachParent();
						}
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::PopID();
			}
			if (!bFoundClass) ImGui::TextDisabled("No matching component types.");
			ImGui::EndPopup();
		}

		if (bOpenRemovePopup) ImGui::OpenPopup("Remove Component?");
		if (ImGui::BeginPopupModal("Remove Component?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::Text("Remove component '%s'?", PendingRemoveComponent ? PendingRemoveComponent->GetName().c_str() : "");
			ImGui::TextDisabled("Its scene children will remain on the actor and keep their world transforms.");
			ImGui::TextDisabled("This action cannot be undone.");
			if (ImGui::Button("Remove"))
			{
				DActorComponent* Component = PendingRemoveComponent.Get();
				if (Component && !Actor->DestroyInstanceComponent(Component)) Context.SetError("Failed to remove component.");
				if (SelectedComponent.Get() == Component) SelectedComponent = nullptr;
				PendingRemoveComponent = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				PendingRemoveComponent = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto FDetailsPanel::DrawReflectedProperties(FLevelEditorContext& Context, DObject* Object) -> void
	{
		const FReflectedPropertyViewContext ViewContext = MakePropertyViewContext(Context);
		if (!PropertyView.HandleOwnerContext(ViewContext, Object))
		{
			ImGui::TextDisabled("The active property preview must be restored before changing targets.");
			return;
		}
		if (!Object)
		{
			ImGui::TextDisabled("Nothing to inspect.");
			return;
		}

		ImGui::TextDisabled("%s", Object->GetClass()->GetName().c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##PropertySearch", "Search properties...", PropertySearchText.data(), PropertySearchText.size());

		FObjectPropertyViewBuilder Builder(PropertySearchText.data());
		for (const std::shared_ptr<IObjectDetailsCustomization>& Customization
			: FLevelEditorCustomizationRegistry::Get().FindObjectDetailsCustomizations(Object->GetClass()))
		{
			Customization->CustomizeDetails(Context, Object, Builder);
		}
		if (!MonaImGui::PropertyEdit::BeginTable("DetailsPropertyTable")) return;

		const FObjectPropertyViewBuilderResult BuilderResult = Builder.DrawRows(PropertyView, ViewContext);
		FObjectPropertyViewResult ObjectViewResult;
		if (!Builder.IsReplacingDefaultProperties())
		{
			ObjectViewResult = PropertyView.EditObject(ViewContext, Object, {
				.SearchText = PropertySearchText.data(),
				.Filter = [&Builder](const FProperty& Property, uint32) {
					return !Builder.IsPropertyHidden(Property);
				},
				.bCreatePropertyTable = false,
				.bShowEmptyMessage = false,
			});
		}
		MonaImGui::PropertyEdit::EndTable();

		if (BuilderResult.VisibleRowCount + ObjectViewResult.VisiblePropertyCount == 0)
		{
			ImGui::TextDisabled(PropertySearchText[0] != '\0'
				? "No properties match the current search."
				: "This object has no reflected Edit properties.");
		}
	}

	auto FDetailsPanel::MakePropertyViewContext(FLevelEditorContext& Context) const -> FReflectedPropertyViewContext
	{
		return {
			.Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr,
			.ReportError = [&Context](std::string Error) { Context.SetError(std::move(Error)); },
			.bReadOnly = Context.bReadOnly,
		};
	}

	auto FDetailsPanel::RequestDeactivate(FLevelEditorContext& Context) -> bool
	{
		return FinishActivePropertyEdit(&Context, true);
	}

	auto FDetailsPanel::FinishActivePropertyEdit(FLevelEditorContext* Context, bool bCancel) -> bool
	{
		if (!Context)
		{
			return PropertyView.FinishActiveEdit(nullptr, bCancel);
		}
		const FReflectedPropertyViewContext ViewContext = MakePropertyViewContext(*Context);
		return PropertyView.FinishActiveEdit(&ViewContext, bCancel);
	}
} // namespace Durin
