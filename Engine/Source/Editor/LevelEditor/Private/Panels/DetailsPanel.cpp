#include "Panels/DetailsPanel.h"

#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Actor.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool
		{
			if (Filter.empty()) return true;
			std::string LowerText(Text);
			std::string LowerFilter(Filter);
			std::ranges::transform(LowerText, LowerText.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			std::ranges::transform(LowerFilter, LowerFilter.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return LowerText.find(LowerFilter) != std::string::npos;
		}

		auto ClassDisplayName(const DClass* Class) -> std::string
		{
			const std::string Name = Class ? Class->GetName() : std::string();
			const size_t Separator = Name.rfind("::");
			return Separator == std::string::npos ? Name : Name.substr(Separator + 2);
		}

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
			ImGui::TextDisabled("Select an actor to inspect it.");
			ImGui::End();
			return;
		}
		if (PropertyActor.Get() != Actor)
		{
			PropertyActor = Actor;
			SelectedComponent = nullptr;
		}
		if (Context.GetSelectedActors().size() > 1) ImGui::TextDisabled("%zu actors selected; editing primary actor only.", Context.GetSelectedActors().size());

		ImGui::TextUnformatted(Actor->GetName().c_str());
		ImGui::TextDisabled("%s", Actor->GetClass()->GetName().c_str());
		ImGui::Separator();
		DrawTransform(Actor);
		DrawComponents(Context, Actor);
		DrawReflectedProperties(Context, SelectedComponent ? static_cast<DObject*>(SelectedComponent.Get()) : static_cast<DObject*>(Actor));
		ImGui::End();
	}

	auto FDetailsPanel::DrawTransform(AActor* Actor) -> void
	{
		DSceneComponent* RootComponent = Actor->GetRootComponent();
		if (RootComponent == nullptr || !ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		FVector3 Location = RootComponent->GetWorldLocation();
		FVector3 RotationDegrees = glm::degrees(glm::eulerAngles(RootComponent->GetWorldRotation()));
		FVector3 Scale = RootComponent->GetWorldScale3D();
		bool bChanged = false;
		bChanged |= ImGui::DragScalarN("Location", ImGuiDataType_Double, &Location.x, 3, 0.05f);
		bChanged |= ImGui::DragScalarN("Rotation", ImGuiDataType_Double, &RotationDegrees.x, 3, 0.25f);
		bChanged |= ImGui::DragScalarN("Scale", ImGuiDataType_Double, &Scale.x, 3, 0.01f);
		if (!bChanged)
		{
			return;
		}

		RootComponent->SetWorldLocation(Location);
		RootComponent->SetWorldRotation(glm::quat(glm::radians(RotationDegrees)));
		RootComponent->SetWorldScale3D(Scale);
	}

	auto FDetailsPanel::DrawComponents(FLevelEditorContext& Context, AActor* Actor) -> void
	{
		if (!ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}
		if (ImGui::Button("Add Component"))
		{
			ComponentTypeSearchText.fill(0);
			ImGui::OpenPopup("Add Component");
		}
		if (ImGui::BeginPopup("Add Component"))
		{
			ImGui::InputTextWithHint("###ComponentTypeSearch", "Search component types...", ComponentTypeSearchText.data(), ComponentTypeSearchText.size());
			for (DClass* Class : GetDerivedClasses(DActorComponent::StaticClass(), true))
			{
				if (!CanConstructObjectOfClass(Class, DActorComponent::StaticClass())) continue;
				const std::string DisplayName = ClassDisplayName(Class);
				if (!ContainsInsensitive(DisplayName, ComponentTypeSearchText.data())) continue;
				if (ImGui::Selectable(DisplayName.c_str()))
				{
					if (!Actor->AddInstanceComponent(Class, FName(DisplayName))) Context.SetError(std::format("Failed to add component of class {}.", Class->GetQualifiedName().ToString()));
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}
		bool bRequestRemoveComponent = false;
		for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
		{
			DActorComponent* Component = ComponentPtr.Get();
			if (Component != nullptr)
			{
				ImGui::PushID(Component);
				const std::string ComponentLabel = std::format("{}  ({})", Component->GetName(), ClassDisplayName(Component->GetClass()));
				if (ImGui::Selectable(ComponentLabel.c_str(), SelectedComponent.Get() == Component)) SelectedComponent = Component;
				ImGui::SameLine();
				if (Actor->IsInstanceComponent(Component))
				{
					if (ImGui::SmallButton("Remove")) { PendingRemoveComponent = Component; bRequestRemoveComponent = true; }
				}
				else ImGui::TextDisabled("Default");
				ImGui::PopID();
			}
		}
		if (bRequestRemoveComponent) ImGui::OpenPopup("Remove Component?");
		if (ImGui::BeginPopupModal("Remove Component?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Remove component '%s'?", PendingRemoveComponent ? PendingRemoveComponent->GetName().c_str() : "");
			ImGui::TextDisabled("This action cannot be undone.");
			if (ImGui::Button("Remove"))
			{
				DActorComponent* Component = PendingRemoveComponent.Get();
				if (Component && !Actor->DestroyInstanceComponent(Component)) Context.SetError("Failed to remove component.");
				PendingRemoveComponent = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) { PendingRemoveComponent = nullptr; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}

	auto FDetailsPanel::DrawReflectedProperties(FLevelEditorContext& Context, DObject* Object) -> void
	{
		if (!ImGui::CollapsingHeader("Editable Properties", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}
		bool bFoundEditableProperty = false;
		ImGui::TextDisabled("%s", Object->GetClass()->GetName().c_str());
		Object->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property == nullptr || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit))
			{
				return;
			}
			bFoundEditableProperty = true;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				DrawProperty(Context, Object, Property, ArrayIndex);
			}
		});
		if (!bFoundEditableProperty)
		{
			ImGui::TextDisabled("This object has no reflected Edit properties.");
		}
	}

	auto FDetailsPanel::DrawProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex) -> void
	{
		const std::string BaseName = Property->NamePrivate.ToString();
		const std::string Label = Property->GetArrayDim() > 1 ? std::format("{}[{}]", BaseName, ArrayIndex) : BaseName;
		const bool bReadOnly = Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		ImGui::PushID(Property);
		ImGui::PushID(static_cast<int>(ArrayIndex));
		if (bReadOnly)
		{
			ImGui::BeginDisabled();
		}

		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		bool bChanged = false;
		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool)
		{
			bool* Value = Property->ContainerPtrToValuePtr<bool>(Object, ArrayIndex);
			bChanged = ImGui::Checkbox(Label.c_str(), Value);
		}
		else if (const ImGuiDataType DataType = ImGuiDataTypeForProperty(Kind); DataType != ImGuiDataType_COUNT)
		{
			bChanged = ImGui::DragScalar(Label.c_str(), DataType, Property->GetValuePtr(Object, ArrayIndex), Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f);
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			auto* StringProperty = static_cast<FStringProperty*>(Property);
			std::array<char, 512> Buffer{};
			const std::string& CurrentValue = *StringProperty->GetStringValuePtr(Object, ArrayIndex);
			std::memcpy(Buffer.data(), CurrentValue.data(), FMath::Min(CurrentValue.size(), Buffer.size() - 1));
			if (ImGui::InputText(Label.c_str(), Buffer.data(), Buffer.size()))
			{
				*StringProperty->GetStringValuePtr(Object, ArrayIndex) = Buffer.data();
				bChanged = true;
			}
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Enum)
		{
			auto* EnumProperty = static_cast<FEnumProperty*>(Property);
			DEnum* Enum = EnumProperty->GetEnum();
			if (Enum == nullptr)
			{
				ImGui::LabelText(Label.c_str(), "<enum metadata unavailable>");
			}
			else
			{
				int64 CurrentValue = ReadEnumValue(*EnumProperty, Object, ArrayIndex);
				FName CurrentName;
				const std::string Preview = Enum->FindNameByValue(CurrentValue, CurrentName) ? CurrentName.ToString() : std::format("{}", CurrentValue);
				if (ImGui::BeginCombo(Label.c_str(), Preview.c_str()))
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
			if (ImGui::BeginCombo(Label.c_str(), Preview.c_str()))
			{
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
			ImGui::LabelText(Label.c_str(), "<%s>", PropertyKindName(Kind));
		}
		if (bChanged) Object->MarkPackageDirty();

		if (bReadOnly)
		{
			ImGui::EndDisabled();
		}
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
