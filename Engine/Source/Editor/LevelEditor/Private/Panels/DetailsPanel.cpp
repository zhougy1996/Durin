#include "Panels/DetailsPanel.h"

#include "Components/SceneComponent.h"
#include "DObject/DurinPropertyTypes.h"
#include "Engine/Actor.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
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

		AActor* Actor = Context.SelectedActor.Get();
		if (Actor == nullptr)
		{
			ImGui::TextDisabled("Select an actor to inspect it.");
			ImGui::End();
			return;
		}

		ImGui::TextUnformatted(Actor->GetName().c_str());
		ImGui::TextDisabled("%s", Actor->GetClass()->GetName().c_str());
		ImGui::Separator();
		DrawTransform(Actor);
		DrawComponents(Actor);
		DrawReflectedProperties(Actor);
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

	auto FDetailsPanel::DrawComponents(AActor* Actor) -> void
	{
		if (!ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}
		for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
		{
			const DActorComponent* Component = ComponentPtr.Get();
			if (Component != nullptr)
			{
				ImGui::BulletText("%s  (%s)", Component->GetName().c_str(), Component->GetClass()->GetName().c_str());
			}
		}
	}

	auto FDetailsPanel::DrawReflectedProperties(AActor* Actor) -> void
	{
		if (!ImGui::CollapsingHeader("Editable Properties", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}
		bool bFoundEditableProperty = false;
		Actor->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property == nullptr || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit))
			{
				return;
			}
			bFoundEditableProperty = true;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				DrawProperty(Actor, Property, ArrayIndex);
			}
		});
		if (!bFoundEditableProperty)
		{
			ImGui::TextDisabled("This actor has no reflected Edit properties.");
		}
	}

	auto FDetailsPanel::DrawProperty(AActor* Actor, FProperty* Property, uint32 ArrayIndex) -> void
	{
		const std::string BaseName = Property->NamePrivate.ToString();
		const std::string Label = Property->GetArrayDim() > 1 ? std::format("{}[{}]", BaseName, ArrayIndex) : BaseName;
		const bool bReadOnly = Property->HasAnyPropertyFlags(EPropertyFlags::EditConst);
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
			bool* Value = Property->ContainerPtrToValuePtr<bool>(Actor, ArrayIndex);
			bChanged = ImGui::Checkbox(Label.c_str(), Value);
		}
		else if (const ImGuiDataType DataType = ImGuiDataTypeForProperty(Kind); DataType != ImGuiDataType_COUNT)
		{
			bChanged = ImGui::DragScalar(Label.c_str(), DataType, Property->GetValuePtr(Actor, ArrayIndex), Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f);
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			auto* StringProperty = static_cast<FStringProperty*>(Property);
			std::array<char, 512> Buffer{};
			const std::string& CurrentValue = *StringProperty->GetStringValuePtr(Actor, ArrayIndex);
			std::memcpy(Buffer.data(), CurrentValue.data(), FMath::Min(CurrentValue.size(), Buffer.size() - 1));
			if (ImGui::InputText(Label.c_str(), Buffer.data(), Buffer.size()))
			{
				*StringProperty->GetStringValuePtr(Actor, ArrayIndex) = Buffer.data();
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
				int64 CurrentValue = ReadEnumValue(*EnumProperty, Actor, ArrayIndex);
				FName CurrentName;
				const std::string Preview = Enum->FindNameByValue(CurrentValue, CurrentName) ? CurrentName.ToString() : std::format("{}", CurrentValue);
				if (ImGui::BeginCombo(Label.c_str(), Preview.c_str()))
				{
					Enum->ForEachValue([&](const FEnumValue& Value) {
						const bool bSelected = Value.Value == CurrentValue;
						if (ImGui::Selectable(Value.Name.ToString().c_str(), bSelected))
						{
							WriteEnumValue(*EnumProperty, Actor, ArrayIndex, Value.Value);
							bChanged = true;
						}
					});
					ImGui::EndCombo();
				}
			}
		}
		else
		{
			ImGui::LabelText(Label.c_str(), "<%s>", PropertyKindName(Kind));
		}
		if (bChanged) Actor->MarkPackageDirty();

		if (bReadOnly)
		{
			ImGui::EndDisabled();
		}
		ImGui::PopID();
		ImGui::PopID();
	}
} // namespace Durin
