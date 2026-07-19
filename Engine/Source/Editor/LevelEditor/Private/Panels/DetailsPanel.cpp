#include "Panels/DetailsPanel.h"
#include "Panels/DetailsPropertyEditing.h"

#include "AssetSystem.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "Engine/Actor.h"
#include "Editor/EditorWorkspaceUI.h"
#include "LevelEditorContext.h"
#include "LevelEditorHelpers.h"
#include "LevelEditorWorkspace.h"
#include "LevelEditorCustomizations.h"
#include "EditorSessionSettings.h"
#include "Materials/MaterialInterface.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::ClassDisplayName;
		using StringUtils::ContainsInsensitive;

		constexpr const char* ComponentDragPayload = "DURIN_DETAILS_SCENE_COMPONENT";

		auto IsClassChildOf(const DClass* Class, const DClass* Parent) -> bool
		{
			for (const DClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			{
				if (Current == Parent) return true;
			}
			return false;
		}

		auto PropertyKindName(DurinCodeGen::EPropertyGenFlags Kind) -> const char*
		{
			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool: return "bool";
			case DurinCodeGen::EPropertyGenFlags::Int8: return "int8";
			case DurinCodeGen::EPropertyGenFlags::Int16: return "int16";
			case DurinCodeGen::EPropertyGenFlags::Int32: return "int32";
			case DurinCodeGen::EPropertyGenFlags::Int64: return "int64";
			case DurinCodeGen::EPropertyGenFlags::UInt8: return "uint8";
			case DurinCodeGen::EPropertyGenFlags::UInt16: return "uint16";
			case DurinCodeGen::EPropertyGenFlags::UInt32: return "uint32";
			case DurinCodeGen::EPropertyGenFlags::UInt64: return "uint64";
			case DurinCodeGen::EPropertyGenFlags::Float: return "float";
			case DurinCodeGen::EPropertyGenFlags::Double: return "double";
			case DurinCodeGen::EPropertyGenFlags::String: return "string";
			case DurinCodeGen::EPropertyGenFlags::Enum: return "enum";
			case DurinCodeGen::EPropertyGenFlags::Object: return "object";
			case DurinCodeGen::EPropertyGenFlags::Struct: return "struct";
			case DurinCodeGen::EPropertyGenFlags::Array: return "array";
			case DurinCodeGen::EPropertyGenFlags::Map: return "map";
			default: return "unsupported";
			}
		}

		auto ImGuiDataTypeForProperty(DurinCodeGen::EPropertyGenFlags Kind) -> ImGuiDataType
		{
			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Int8: return ImGuiDataType_S8;
			case DurinCodeGen::EPropertyGenFlags::Int16: return ImGuiDataType_S16;
			case DurinCodeGen::EPropertyGenFlags::Int32: return ImGuiDataType_S32;
			case DurinCodeGen::EPropertyGenFlags::Int64: return ImGuiDataType_S64;
			case DurinCodeGen::EPropertyGenFlags::UInt8: return ImGuiDataType_U8;
			case DurinCodeGen::EPropertyGenFlags::UInt16: return ImGuiDataType_U16;
			case DurinCodeGen::EPropertyGenFlags::UInt32: return ImGuiDataType_U32;
			case DurinCodeGen::EPropertyGenFlags::UInt64: return ImGuiDataType_U64;
			case DurinCodeGen::EPropertyGenFlags::Float: return ImGuiDataType_Float;
			case DurinCodeGen::EPropertyGenFlags::Double: return ImGuiDataType_Double;
			default: return ImGuiDataType_COUNT;
			}
		}

		auto ReadEnumValue(const FEnumProperty& Property, const void* Container, uint32 ArrayIndex) -> int64
		{
			switch (Property.GetUnderlyingType())
			{
			case DurinCodeGen::EEnumUnderlyingType::Int8: return *Property.GetEnumValuePtr<int8>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::Int16: return *Property.GetEnumValuePtr<int16>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::Int32: return *Property.GetEnumValuePtr<int32>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::Int64: return *Property.GetEnumValuePtr<int64>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::UInt8: return *Property.GetEnumValuePtr<uint8>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::UInt16: return *Property.GetEnumValuePtr<uint16>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::UInt32: return *Property.GetEnumValuePtr<uint32>(Container, ArrayIndex);
			case DurinCodeGen::EEnumUnderlyingType::UInt64: return static_cast<int64>(*Property.GetEnumValuePtr<uint64>(Container, ArrayIndex));
			default: return 0;
			}
		}

		auto WriteEnumValue(const FEnumProperty& Property, void* Container, uint32 ArrayIndex, int64 Value) -> void
		{
			switch (Property.GetUnderlyingType())
			{
			case DurinCodeGen::EEnumUnderlyingType::Int8: *Property.GetEnumValuePtr<int8>(Container, ArrayIndex) = static_cast<int8>(Value); break;
			case DurinCodeGen::EEnumUnderlyingType::Int16: *Property.GetEnumValuePtr<int16>(Container, ArrayIndex) = static_cast<int16>(Value); break;
			case DurinCodeGen::EEnumUnderlyingType::Int32: *Property.GetEnumValuePtr<int32>(Container, ArrayIndex) = static_cast<int32>(Value); break;
			case DurinCodeGen::EEnumUnderlyingType::Int64: *Property.GetEnumValuePtr<int64>(Container, ArrayIndex) = Value; break;
			case DurinCodeGen::EEnumUnderlyingType::UInt8: *Property.GetEnumValuePtr<uint8>(Container, ArrayIndex) = static_cast<uint8>(Value); break;
			case DurinCodeGen::EEnumUnderlyingType::UInt16: *Property.GetEnumValuePtr<uint16>(Container, ArrayIndex) = static_cast<uint16>(Value); break;
			case DurinCodeGen::EEnumUnderlyingType::UInt32: *Property.GetEnumValuePtr<uint32>(Container, ArrayIndex) = static_cast<uint32>(Value); break;
			case DurinCodeGen::EEnumUnderlyingType::UInt64: *Property.GetEnumValuePtr<uint64>(Container, ArrayIndex) = static_cast<uint64>(Value); break;
			default: break;
			}
		}

	} // namespace

	FDetailsPanel::FDetailsPanel(FEditorSessionSettings& InSessionSettings)
		: SessionSettings(InSessionSettings)
		, ComponentPaneRatio(InSessionSettings.GetDetailsPaneRatio())
	{
	}

	auto FDetailsPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!EditorWorkspaceUI::BeginDockablePanel(LevelEditorWorkspace::Type, "Details", "Details", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

		AActor* Actor = Context.GetPrimarySelectedActor();
		if (Actor == nullptr)
		{
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
			PropertyActor = Actor;
			SelectedComponent = nullptr;
			RenamingComponent = nullptr;
			RenameDialog.Cancel();
			PendingExpandComponent = nullptr;
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
		if (!Object)
		{
			ImGui::TextDisabled("Nothing to inspect.");
			return;
		}

		ImGui::TextDisabled("%s", Object->GetClass()->GetName().c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##PropertySearch", "Search properties...", PropertySearchText.data(), PropertySearchText.size());

		std::vector<std::pair<FProperty*, uint32>> VisibleProperties;
		Object->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit)) return;
			if (Cast<DStaticMeshComponent>(Object) && Property->NamePrivate == FName("Materials")) return;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				const std::string Name = Property->GetArrayDim() > 1 ? std::format("{}[{}]", Property->NamePrivate.ToString(), ArrayIndex) : Property->NamePrivate.ToString();
				const bool bIsTransform = Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
										  && static_cast<FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_Durin_FTransform();
				const std::string SearchText = bIsTransform ? std::format("{} Location Rotation Scale", Name) : Name;
				if (ContainsInsensitive(SearchText, PropertySearchText.data())) VisibleProperties.emplace_back(Property, ArrayIndex);
			}
		});

		auto* Actor = Cast<AActor>(Object);
		auto* StaticMeshComponent = Cast<DStaticMeshComponent>(Object);
		const std::shared_ptr<IObjectDetailsCustomization> DetailsCustomization = FLevelEditorCustomizationRegistry::Get().FindObjectDetails(Object->GetClass());
		bool bShowStaticMeshMaterials = StaticMeshComponent && StaticMeshComponent->GetNumMaterials() > 0;
		if (bShowStaticMeshMaterials && PropertySearchText[0] != '\0')
		{
			bShowStaticMeshMaterials = ContainsInsensitive("Materials Material Slots", PropertySearchText.data());
			const DStaticMesh* Mesh = StaticMeshComponent->GetStaticMesh();
			const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!bShowStaticMeshMaterials && Data)
			{
				bShowStaticMeshMaterials = std::ranges::any_of(Data->MaterialSlots, [this](const FStaticMeshMaterialSlot& Slot) {
					return ContainsInsensitive(Slot.Name, PropertySearchText.data());
				});
			}
		}
		const bool bShowActorTransform = Actor && Actor->GetRootComponent()
										 && ContainsInsensitive("Transform Location Rotation Scale", PropertySearchText.data());
		if (!bShowActorTransform && !bShowStaticMeshMaterials && VisibleProperties.empty() && !DetailsCustomization)
		{
			ImGui::TextDisabled(PropertySearchText[0] != '\0' ? "No properties match the current search." : "This object has no reflected Edit properties.");
			return;
		}

		if (!MonaImGui::BeginPropertyTable("DetailsPropertyTable")) return;

		if (bShowActorTransform)
		{
			ImGui::PushID("ActorTransform");
			FTransform Transform = Actor->GetRootComponent()->GetRelativeTransform();
			if (MonaImGui::EditTransformProperty("Transform", Transform, false))
			{
				Actor->GetRootComponent()->SetRelativeTransform(Transform);
				Actor->MarkPackageDirty();
			}
			ImGui::PopID();
		}
		const bool bReplaceReflectedProperties = DetailsCustomization && DetailsCustomization->DrawDetails(Context, Object);
		if (bShowStaticMeshMaterials) DrawStaticMeshMaterials(Context, StaticMeshComponent);
		if (!bReplaceReflectedProperties) for (const auto& [Property, ArrayIndex] : VisibleProperties)
		{
			DrawProperty(Context, Object, Property, ArrayIndex);
		}
		MonaImGui::EndPropertyTable();
	}

	auto FDetailsPanel::DrawStaticMeshMaterials(FLevelEditorContext& Context, DStaticMeshComponent* Component) -> void
	{
		DStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData) return;
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			const std::string Label = std::format("Material[{}] {}", SlotIndex, RenderData->MaterialSlots[SlotIndex].Name);
			if (!ContainsInsensitive(Label, PropertySearchText.data()) && PropertySearchText[0] != '\0') continue;
			ImGui::PushID("StaticMeshMaterial");
			ImGui::PushID(static_cast<int>(SlotIndex));
			MonaImGui::BeginPropertyRow(Label.c_str(), false);
			DMaterialInterface* Current = Component->GetMaterial(SlotIndex);
			const std::string Preview = Current && Current->GetPackage() ? Current->GetPackage()->GetPackagePath() : "None";
			if (ImGui::BeginCombo("##Value", Preview.c_str()))
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##AssetSearch", "Search materials...", AssetSearchText.data(), AssetSearchText.size());
				if (ImGui::Selectable("Clear", Current == nullptr)) Component->SetMaterial(SlotIndex, nullptr);
				for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
				{
					DClass* AssetClass = FindClassByQualifiedName(Data.AssetClassName);
					const std::string PathString = Path.ToString();
					if (!AssetClass || !IsClassChildOf(AssetClass, DMaterialInterface::StaticClass()) || !ContainsInsensitive(PathString, AssetSearchText.data())) continue;
					if (ImGui::Selectable(PathString.c_str(), Current && Current->GetPackage() && Current->GetPackage()->GetPackagePath() == PathString))
					{
						DObject* Loaded = nullptr;
						Asset::FAssetResult Result = Asset::LoadAsset(Path, Loaded);
						if (!Result) Context.SetError(Result.Message);
						else if (DMaterialInterface* Selected = Cast<DMaterialInterface>(Loaded)) Component->SetMaterial(SlotIndex, Selected);
					}
				}
				ImGui::EndCombo();
			}
			MonaImGui::EndPropertyRow(false);
			ImGui::PopID();
			ImGui::PopID();
		}
	}

	auto FDetailsPanel::DrawProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex) -> void
	{
		const std::string BaseName = Property->NamePrivate.ToString();
		const std::string Label = Property->GetArrayDim() > 1 ? std::format("{}[{}]", BaseName, ArrayIndex) : BaseName;
		const bool bReadOnly = Context.bReadOnly || Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		const bool bIsTransform = Kind == DurinCodeGen::EPropertyGenFlags::Struct
								  && static_cast<FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_Durin_FTransform();
		const bool bIsLinearColor = Kind == DurinCodeGen::EPropertyGenFlags::Struct
									&& static_cast<FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_Durin_FLinearColor();
		ImGui::PushID(Property);
		ImGui::PushID(static_cast<int>(ArrayIndex));

		if (bIsTransform)
		{
			FTransform Value = *Property->ContainerPtrToValuePtr<FTransform>(Object, ArrayIndex);
			if (MonaImGui::EditTransformProperty(Label.c_str(), Value, bReadOnly))
			{
				if (auto* SceneComponent = Cast<DSceneComponent>(Object); SceneComponent && Property->NamePrivate == FName("RelativeTransform"))
				{
					SceneComponent->SetRelativeTransform(Value);
				}
				else
				{
					*Property->ContainerPtrToValuePtr<FTransform>(Object, ArrayIndex) = Value;
				}
				Object->MarkPackageDirty();
			}
			ImGui::PopID();
			ImGui::PopID();
			return;
		}

		if (bIsLinearColor)
		{
			FLinearColor Value = *Property->ContainerPtrToValuePtr<FLinearColor>(Object, ArrayIndex);
			const bool bShowAlpha = Property->GetMetaData(FName("HideAlpha")) != "true";
			if (MonaImGui::EditColorProperty(Label.c_str(), Value, bShowAlpha, bReadOnly))
			{
				*Property->ContainerPtrToValuePtr<FLinearColor>(Object, ArrayIndex) = Value;
				Object->MarkPackageDirty();
			}
			ImGui::PopID();
			ImGui::PopID();
			return;
		}

		MonaImGui::BeginPropertyRow(Label.c_str(), bReadOnly);

		bool bChanged = false;
		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool)
		{
			bool* Value = Property->ContainerPtrToValuePtr<bool>(Object, ArrayIndex);
			bChanged = ImGui::Checkbox("##Value", Value);
		}
		else if (const ImGuiDataType DataType = ImGuiDataTypeForProperty(Kind); DataType != ImGuiDataType_COUNT)
		{
			bChanged = ImGui::DragScalar("##Value", DataType, Property->GetValuePtr(Object, ArrayIndex), Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f);
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			auto* StringProperty = static_cast<FStringProperty*>(Property);
			std::array<char, 512> Buffer{};
			const std::string& CurrentValue = *StringProperty->GetStringValuePtr(Object, ArrayIndex);
			std::memcpy(Buffer.data(), CurrentValue.data(), FMath::Min(CurrentValue.size(), Buffer.size() - 1));
			if (ImGui::InputText("##Value", Buffer.data(), Buffer.size()))
			{
				*StringProperty->GetStringValuePtr(Object, ArrayIndex) = Buffer.data();
				bChanged = true;
			}
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Enum)
		{
			auto* EnumProperty = static_cast<FEnumProperty*>(Property);
			DEnum* Enum = EnumProperty->GetEnum();
			if (!Enum)
			{
				ImGui::TextDisabled("<enum metadata unavailable>");
			}
			else
			{
				int64 CurrentValue = ReadEnumValue(*EnumProperty, Object, ArrayIndex);
				FName CurrentName;
				const std::string Preview = Enum->FindNameByValue(CurrentValue, CurrentName) ? CurrentName.ToString() : std::format("{}", CurrentValue);
				if (ImGui::BeginCombo("##Value", Preview.c_str()))
				{
					Enum->ForEachValue([&](const FEnumValue& Value) {
						const bool bSelected = Value.Value == CurrentValue;
						if (ImGui::Selectable(Value.Name.ToString().c_str(), bSelected))
						{
							WriteEnumValue(*EnumProperty, Object, ArrayIndex, Value.Value);
							bChanged = true;
						}
					});
					ImGui::EndCombo();
				}
			}
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Object)
		{
			auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
			DObject* Current = ObjectProperty->GetObjectPropertyValue(Object, ArrayIndex);
			const std::string Preview = Current && Current->GetPackage() ? Current->GetPackage()->GetPackagePath() : "None";
			if (ImGui::BeginCombo("##Value", Preview.c_str()))
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##AssetSearch", "Search assets...", AssetSearchText.data(), AssetSearchText.size());
				if (ImGui::Selectable("Clear", Current == nullptr)) bChanged = AssignObjectProperty(Context, Object, Property, ArrayIndex, nullptr);
				DClass* RequiredClass = ObjectProperty->GetReferencedClass();
				for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
				{
					DClass* AssetClass = FindClassByQualifiedName(Data.AssetClassName);
					const std::string PathString = Path.ToString();
					if (!AssetClass || !RequiredClass || !IsClassChildOf(AssetClass, RequiredClass) || !ContainsInsensitive(PathString, AssetSearchText.data())) continue;
					if (ImGui::Selectable(PathString.c_str(), Current && Current->GetPackage() && Current->GetPackage()->GetPackagePath() == PathString))
					{
						DObject* Loaded = nullptr;
						Asset::FAssetResult Result = Asset::LoadAsset(Path, Loaded);
						if (!Result)
							Context.SetError(Result.Message);
						else
							bChanged = AssignObjectProperty(Context, Object, Property, ArrayIndex, Loaded);
					}
				}
				ImGui::EndCombo();
			}
		}
		else
		{
			ImGui::TextDisabled("<%s>", PropertyKindName(Kind));
		}

		MonaImGui::EndPropertyRow(bReadOnly);
		if (bChanged && Kind != DurinCodeGen::EPropertyGenFlags::Object) Object->MarkPackageDirty();
		ImGui::PopID();
		ImGui::PopID();
	}

	auto FDetailsPanel::AssignObjectProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex, DObject* Value) -> bool
	{
		return AssignDetailsObjectProperty(Context, Object, Property, ArrayIndex, Value);
	}
} // namespace Durin
