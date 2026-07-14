#include "Panels/DetailsPanel.h"

#include "AssetSystem.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "Engine/Actor.h"
#include "LevelEditorContext.h"
#include "LevelEditorHelpers.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "StaticMesh/StaticMesh.h"

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

	auto FDetailsPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!ImGui::Begin("Details###Details", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

		AActor* Actor = Context.GetPrimarySelectedActor();
		if (Actor == nullptr)
		{
			PropertyActor = nullptr;
			SelectedComponent = nullptr;
			ImGui::TextDisabled("Select an actor to inspect it.");
			ImGui::End();
			return;
		}

		if (PropertyActor.Get() != Actor)
		{
			PropertyActor = Actor;
			SelectedComponent = nullptr;
			PendingExpandComponent = nullptr;
		}
		if (SelectedComponent && std::ranges::none_of(Actor->GetOwnedComponents(), [this](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == SelectedComponent.Get(); }))
		{
			SelectedComponent = nullptr;
		}

		ImGui::TextUnformatted(Actor->GetName().c_str());
		ImGui::TextDisabled("%s", Actor->GetClass()->GetName().c_str());
		if (Context.GetSelectedActors().size() > 1)
		{
			ImGui::TextDisabled("%zu actors selected; editing primary actor only.", Context.GetSelectedActors().size());
		}
		constexpr float SplitterThickness = 6.0f;
		const float AvailableHeight = ImGui::GetContentRegionAvail().y;
		const float UsableHeight = FMath::Max(AvailableHeight - SplitterThickness, 0.0f);
		float ComponentHeight = UsableHeight * ComponentPaneRatio;
		if (UsableHeight >= 220.0f)
		{
			ComponentHeight = std::clamp(ComponentHeight, 90.0f, UsableHeight - 120.0f);
		}

		if (ImGui::BeginChild("DetailsComponents", ImVec2(0.0f, ComponentHeight), ImGuiChildFlags_Borders))
		{
			DrawComponents(Context, Actor);
		}
		ImGui::EndChild();

		const ImVec2 SplitterPosition = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("DetailsSplitter", ImVec2(ImGui::GetContentRegionAvail().x, SplitterThickness));
		const bool bSplitterHovered = ImGui::IsItemHovered();
		const bool bSplitterActive = ImGui::IsItemActive();
		if (bSplitterHovered || bSplitterActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		const ImU32 SplitterColor = ImGui::GetColorU32(bSplitterActive ? ImGuiCol_SeparatorActive : bSplitterHovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
		ImGui::GetWindowDrawList()->AddRectFilled(SplitterPosition, ImVec2(SplitterPosition.x + ImGui::GetItemRectSize().x, SplitterPosition.y + SplitterThickness), SplitterColor);
		if (bSplitterActive && UsableHeight > 0.0f)
		{
			ComponentPaneRatio = std::clamp((ComponentHeight + ImGui::GetIO().MouseDelta.y) / UsableHeight, 0.1f, 0.9f);
		}

		if (ImGui::BeginChild("DetailsProperties", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
		{
			DObject* InspectedObject = SelectedComponent ? static_cast<DObject*>(SelectedComponent.Get()) : static_cast<DObject*>(Actor);
			DrawReflectedProperties(Context, InspectedObject);
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
		if (ImGui::SmallButton("+ Add"))
		{
			AddComponentParent = nullptr;
			bAddComponentAsChild = false;
			ComponentTypeSearchText.fill(0);
			bOpenAddPopup = true;
		}

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
			const std::string Status = bIsRoot
				? std::format("Root, {}", bIsInstance ? "Instance" : "Default")
				: bIsInstance ? "Instance" : "Default";
			const std::string Label = std::format("{}  ({})  [{}]", Component->GetName(), ClassDisplayName(Component->GetClass()), Status);
			const bool bOpen = ImGui::TreeNodeEx("##Component", Flags, "%s", Label.c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) SelectedComponent = Component;

			if (ImGui::BeginPopupContextItem("ComponentContext"))
			{
				if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
				if (SceneComponent && ImGui::MenuItem("Add Child Component")) QueueAddComponent(SceneComponent, true);
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
				ImGui::EndPopup();
			}

			if (SceneComponent && !bIsRoot && ImGui::BeginDragDropSource())
			{
				DSceneComponent* PayloadComponent = SceneComponent;
				ImGui::SetDragDropPayload(ComponentDragPayload, &PayloadComponent, sizeof(PayloadComponent));
				ImGui::Text("Move %s", Component->GetName().c_str());
				ImGui::EndDragDropSource();
			}
			if (SceneComponent && ImGui::BeginDragDropTarget())
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
		const bool bActorOpen = ImGui::TreeNodeEx("##Actor", ActorFlags, "%s  (%s)", Actor->GetName().c_str(), ClassDisplayName(Actor->GetClass()).c_str());
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) SelectedComponent = nullptr;
		if (ImGui::BeginPopupContextItem("ActorContext"))
		{
			if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
			ImGui::EndPopup();
		}
		if (ImGui::BeginDragDropTarget())
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
			if (ImGui::MenuItem("Add Component")) QueueAddComponent(nullptr, false);
			ImGui::EndPopup();
		}

		if (bOpenAddPopup) ImGui::OpenPopup("Add Component");
		if (ImGui::BeginPopup("Add Component"))
		{
			ImGui::SetNextItemWidth(320.0f);
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
						if (!bSucceeded) Actor->DestroyInstanceComponent(NewComponent);
						else PendingExpandComponent = Parent;
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
		if (ImGui::BeginPopupModal("Remove Component?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				const std::string Name = Property->GetArrayDim() > 1
					? std::format("{}[{}]", Property->NamePrivate.ToString(), ArrayIndex)
					: Property->NamePrivate.ToString();
				const bool bIsTransform = Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
					&& static_cast<FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_Durin_FTransform();
				const std::string SearchText = bIsTransform ? std::format("{} Location Rotation Scale", Name) : Name;
				if (ContainsInsensitive(SearchText, PropertySearchText.data())) VisibleProperties.emplace_back(Property, ArrayIndex);
			}
		});

		auto* Actor = Cast<AActor>(Object);
		const bool bShowActorTransform = Actor && Actor->GetRootComponent()
			&& ContainsInsensitive("Transform Location Rotation Scale", PropertySearchText.data());
		if (!bShowActorTransform && VisibleProperties.empty())
		{
			ImGui::TextDisabled(PropertySearchText[0] != '\0'
				? "No properties match the current search."
				: "This object has no reflected Edit properties.");
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
		for (const auto& [Property, ArrayIndex] : VisibleProperties)
		{
			DrawProperty(Context, Object, Property, ArrayIndex);
		}
		MonaImGui::EndPropertyTable();
	}

	auto FDetailsPanel::DrawProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex) -> void
	{
		const std::string BaseName = Property->NamePrivate.ToString();
		const std::string Label = Property->GetArrayDim() > 1 ? std::format("{}[{}]", BaseName, ArrayIndex) : BaseName;
		const bool bReadOnly = Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		const bool bIsTransform = Kind == DurinCodeGen::EPropertyGenFlags::Struct
			&& static_cast<FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_Durin_FTransform();
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
						if (!Result) Context.SetError(Result.Message);
						else bChanged = AssignObjectProperty(Context, Object, Property, ArrayIndex, Loaded);
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
		if (bChanged) Object->MarkPackageDirty();
		ImGui::PopID();
		ImGui::PopID();
	}

	auto FDetailsPanel::AssignObjectProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex, DObject* Value) -> bool
	{
		auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
		if (Value && ObjectProperty->GetReferencedClass() && !Value->IsA(ObjectProperty->GetReferencedClass()))
		{
			Context.SetError("Selected asset has an incompatible type.");
			return false;
		}
		if (auto* Component = Cast<DStaticMeshComponent>(Object); Component && Property->NamePrivate == FName("StaticMesh"))
		{
			DStaticMesh* Mesh = Value ? Cast<DStaticMesh>(Value) : nullptr;
			if (Value && !Mesh) return false;
			if (Component->GetStaticMesh() == Mesh) return false;
			Component->SetStaticMesh(Mesh);
			return true;
		}
		if (ObjectProperty->GetObjectPropertyValue(Object, ArrayIndex) == Value) return false;
		ObjectProperty->SetObjectPropertyValue(Object, Value, ArrayIndex);
		return true;
	}
} // namespace Durin
