#include "Editor/ReflectedPropertyView.h"

#include "AssetSystem.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "DObject/Package.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"

namespace Durin
{
	namespace
	{
		using StringUtils::ContainsInsensitive;

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

		template<typename TWriteProposed>
		auto CaptureProposedPropertyValue(const FProperty* Property, void* Container, uint32 ArrayIndex,
			TWriteProposed&& WriteProposed, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) -> bool
		{
			FPropertyValueSnapshot Previous;
			if (!CapturePropertyValue(Property, Container, ArrayIndex, Previous, OutError)) return false;
			WriteProposed();
			const bool bCaptured = CapturePropertyValue(Property, Container, ArrayIndex, OutSnapshot, OutError);
			std::string RestoreError;
			if (!RestorePropertyValue(Property, Container, ArrayIndex, Previous, &RestoreError))
			{
				if (OutError) *OutError = std::move(RestoreError);
				return false;
			}
			return bCaptured;
		}

		auto CaptureMapPathKey(const FProperty* KeyProperty, const void* Key) -> std::vector<uint8>
		{
			FPropertyValueSnapshot Snapshot;
			if (!CapturePropertyValue(KeyProperty, Key, 0, Snapshot)) return {};
			std::vector<uint8> Result = Snapshot.GetBytes();
			// Snapshot object tokens are local indices. Pointer identities keep distinct
			// object keys from collapsing to the same synchronous event path.
			for (DObject* Reference : Snapshot.GetReferencedObjects())
			{
				const uintptr_t Identity = reinterpret_cast<uintptr_t>(Reference);
				const size_t Offset = Result.size();
				Result.resize(Offset + sizeof(Identity));
				std::memcpy(Result.data() + Offset, &Identity, sizeof(Identity));
			}
			return Result;
		}

		auto FindStringMapIndex(const FMapProperty* Property, const DObject* Object, std::string_view Key) -> uint64
		{
			if (!Property || !Property->GetKeyProp()
				|| Property->GetKeyProp()->GetKind() != DurinCodeGen::EPropertyGenFlags::String) return UINT64_MAX;
			auto* KeyProperty = static_cast<FStringProperty*>(Property->GetKeyProp());
			for (uint64 Index = 0; Index < Property->Num(Object); ++Index)
			{
				const void* StoredKey = Property->GetKeyPtr(Object, Index);
				if (StoredKey && *KeyProperty->GetStringValuePtr(StoredKey) == Key) return Index;
			}
			return UINT64_MAX;
		}
	}
	auto FReflectedPropertyView::EditProperty(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FProperty* Property,
		uint32 ArrayIndex,
		const FPropertyViewOptions& Options
	) -> bool
	{
		if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return false;
		std::string Label = Options.Label;
		if (Label.empty()) Label = MakeReflectedPropertyLabel(*Property, ArrayIndex);
		const bool bReadOnly = Context.bReadOnly || Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		ImGui::PushID(Property);
		ImGui::PushID(static_cast<int>(ArrayIndex));
		const bool bChanged = EditPropertyValue(Context, Object, Property, Object, ArrayIndex, Label, bReadOnly, FReflectedPropertyEditTarget::ForMember(Object, Property, ArrayIndex));
		ImGui::PopID();
		ImGui::PopID();
		return bChanged;
	}

	auto FReflectedPropertyView::EditPropertyValue(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FReflectedPropertyEditTarget& EditTarget,
		bool bUseTransaction
	) -> bool
	{
		bReadOnly |= Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		DStruct* Struct = Kind == DurinCodeGen::EPropertyGenFlags::Struct ? static_cast<FStructProperty*>(Property)->GetStruct() : nullptr;
		auto SubmitProposed = [&](auto&& WriteProposed, bool bContinuous) -> bool {
			if (!bUseTransaction)
			{
				WriteProposed();
				return true;
			}
			FPropertyValueSnapshot Proposed;
			std::string Error;
			if (!CaptureProposedPropertyValue(EditTarget.SnapshotProperty, EditTarget.SnapshotContainer, EditTarget.SnapshotArrayIndex,
				std::forward<decltype(WriteProposed)>(WriteProposed), Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			return SubmitPropertyEdit(Context, EditTarget, Proposed, bContinuous);
		};
		auto FinishContinuousEdit = [&](const MonaImGui::FPropertyEditWidgetState& State) {
			if (!bUseTransaction) return;
			if (State.bDeactivatedAfterEdit && IsEditingTarget(EditTarget)) FinishActiveEdit(&Context, false);
			else if (State.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && IsEditingTarget(EditTarget)) FinishActiveEdit(&Context, true);
		};

		if (Struct == Z_Construct_DStruct_Durin_FTransform())
		{
			FTransform Value = *Property->ContainerPtrToValuePtr<FTransform>(Container, ArrayIndex);
			MonaImGui::FPropertyEditWidgetState State;
			const bool bChanged = MonaImGui::EditTransformProperty(Label.c_str(), Value, bReadOnly, &State);
			if (bChanged)
			{
				SubmitProposed([&] { *Property->ContainerPtrToValuePtr<FTransform>(Container, ArrayIndex) = Value; }, true);
			}
			FinishContinuousEdit(State);
			return bChanged;
		}

		auto EditMathStruct = [&]<typename TValue, typename TEditor>(TValue Value, TEditor&& Editor) -> bool {
			MonaImGui::FPropertyEditWidgetState State;
			const bool bChanged = Editor(Value, State);
			if (bChanged)
			{
				SubmitProposed([&] { *Property->ContainerPtrToValuePtr<TValue>(Container, ArrayIndex) = Value; }, true);
			}
			FinishContinuousEdit(State);
			return bChanged;
		};

		if (Struct == Z_Construct_DStruct_Durin_FVector2())
		{
			return EditMathStruct(*Property->ContainerPtrToValuePtr<FVector2>(Container, ArrayIndex), [&](FVector2& Value, auto& State) {
				return MonaImGui::EditVectorProperty(Label.c_str(), Value, bReadOnly, 0.05, &State);
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FVector3())
		{
			return EditMathStruct(*Property->ContainerPtrToValuePtr<FVector3>(Container, ArrayIndex), [&](FVector3& Value, auto& State) {
				return MonaImGui::EditVectorProperty(Label.c_str(), Value, bReadOnly, 0.05, &State);
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FVector4())
		{
			return EditMathStruct(*Property->ContainerPtrToValuePtr<FVector4>(Container, ArrayIndex), [&](FVector4& Value, auto& State) {
				return MonaImGui::EditVectorProperty(Label.c_str(), Value, bReadOnly, 0.05, &State);
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FQuat())
		{
			return EditMathStruct(*Property->ContainerPtrToValuePtr<FQuat>(Container, ArrayIndex), [&](FQuat& Value, auto& State) {
				return MonaImGui::EditQuatProperty(Label.c_str(), Value, bReadOnly, &State);
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FLinearColor())
		{
			FLinearColor Value = *Property->ContainerPtrToValuePtr<FLinearColor>(Container, ArrayIndex);
			const bool bShowAlpha = Property->GetMetaData(FName("HideAlpha")) != "true";
			return EditMathStruct(Value, [&](FLinearColor& EditedValue, auto& State) {
				return MonaImGui::EditColorProperty(Label.c_str(), EditedValue, bShowAlpha, bReadOnly, &State);
			});
		}
		if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
			return EditArrayProperty(Context, Object, static_cast<FArrayProperty*>(Property), Container, ArrayIndex, Label, bReadOnly, EditTarget);
		if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
			return EditMapProperty(Context, Object, static_cast<FMapProperty*>(Property), Container, ArrayIndex, Label, bReadOnly, EditTarget);

		MonaImGui::BeginPropertyRow(Label.c_str(), bReadOnly);

		bool bChanged = false;
		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool)
		{
			bool Value = *Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex);
			bChanged = ImGui::Checkbox("##Value", &Value);
			if (bChanged) SubmitProposed([&] { *Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex) = Value; }, false);
		}
		else if (const ImGuiDataType DataType = ImGuiDataTypeForProperty(Kind); DataType != ImGuiDataType_COUNT)
		{
			std::array<uint8, sizeof(uint64)> Value{};
			check(Property->GetElementSize() <= Value.size());
			std::memcpy(Value.data(), Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
			bChanged = ImGui::DragScalar("##Value", DataType, Value.data(), Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f);
			MonaImGui::FPropertyEditWidgetState State{ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()};
			if (bChanged) SubmitProposed([&] { std::memcpy(Property->GetValuePtr(Container, ArrayIndex), Value.data(), Property->GetElementSize()); }, true);
			FinishContinuousEdit(State);
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			auto* StringProperty = static_cast<FStringProperty*>(Property);
			std::array<char, 512> Buffer{};
			const std::string& CurrentValue = *StringProperty->GetStringValuePtr(Container, ArrayIndex);
			std::memcpy(Buffer.data(), CurrentValue.data(), FMath::Min(CurrentValue.size(), Buffer.size() - 1));
			if (ImGui::InputText("##Value", Buffer.data(), Buffer.size()))
			{
				SubmitProposed([&] { *StringProperty->GetStringValuePtr(Container, ArrayIndex) = Buffer.data(); }, true);
				bChanged = true;
			}
			FinishContinuousEdit({ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()});
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
				int64 CurrentValue = ReadEnumValue(*EnumProperty, Container, ArrayIndex);
				FName CurrentName;
				const std::string Preview = Enum->FindNameByValue(CurrentValue, CurrentName) ? CurrentName.ToString() : std::format("{}", CurrentValue);
				if (ImGui::BeginCombo("##Value", Preview.c_str()))
				{
					Enum->ForEachValue([&](const FEnumValue& Value) {
						const bool bSelected = Value.Value == CurrentValue;
						if (ImGui::Selectable(Value.Name.ToString().c_str(), bSelected))
						{
							SubmitProposed([&] { WriteEnumValue(*EnumProperty, Container, ArrayIndex, Value.Value); }, false);
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
			DObject* Current = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
			DObject* SelectedObject = Current;
			const std::string Preview = Current && Current->GetPackage() ? Current->GetPackage()->GetPackagePath() : "None";
			if (ImGui::BeginCombo("##Value", Preview.c_str()))
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##AssetSearch", "Search assets...", AssetSearchText.data(), AssetSearchText.size());
				if (ImGui::Selectable("Clear", Current == nullptr))
				{
					if (Current) { SelectedObject = nullptr; bChanged = true; }
				}
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
							ReportError(Context, Result.Message);
						else if (Loaded != Current)
						{
							SelectedObject = Loaded;
							bChanged = true;
						}
					}
				}
				ImGui::EndCombo();
			}
			if (bChanged) SubmitProposed([&] { ObjectProperty->SetObjectPropertyValue(Container, SelectedObject, ArrayIndex); }, false);
		}
		else
		{
			ImGui::TextDisabled("<%s>", PropertyKindName(Kind));
		}

		MonaImGui::EndPropertyRow(bReadOnly);
		return bChanged;
	}

	auto FReflectedPropertyView::EditArrayProperty(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FArrayProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FReflectedPropertyEditTarget& EditTarget
	) -> bool
	{
		if (!Property->HasArrayHelper() || !Property->GetInner())
		{
			MonaImGui::BeginPropertyRow(Label.c_str(), true);
			ImGui::TextDisabled("<array metadata unavailable>");
			MonaImGui::EndPropertyRow(true);
			return false;
		}

		uint64 Num = Property->Num(Container, ArrayIndex);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = MonaImGui::CompactTreeNode("##Array", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%llu)", Label.c_str(), Num);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		bool bChanged = false;
		auto SubmitStructure = [&](EPropertyChangeKind Kind, auto&& Mutation) -> bool {
			FReflectedPropertyEditTarget StructuralTarget = EditTarget;
			StructuralTarget.Kind = Kind;
			FPropertyValueSnapshot Proposed;
			std::string Error;
			if (!CaptureProposedPropertyValue(StructuralTarget.SnapshotProperty, StructuralTarget.SnapshotContainer,
				StructuralTarget.SnapshotArrayIndex, std::forward<decltype(Mutation)>(Mutation), Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			return SubmitPropertyEdit(Context, StructuralTarget, Proposed, false);
		};
		if (ImGui::SmallButton("+"))
		{
			bChanged = SubmitStructure(EPropertyChangeKind::ArrayAdd, [&] { Property->Resize(Container, Num + 1, ArrayIndex); });
			if (bChanged) ++Num;
		}
		ImGui::SameLine();
		if (Num == 0) ImGui::BeginDisabled();
		if (ImGui::SmallButton("-") && Num > 0)
		{
			bChanged = SubmitStructure(EPropertyChangeKind::ArrayRemove, [&] { Property->Resize(Container, Num - 1, ArrayIndex); });
			if (bChanged) --Num;
		}
		if (Num == 0) ImGui::EndDisabled();
		if (bReadOnly) ImGui::EndDisabled();
		// Restoring the stable outer snapshot may reallocate this array itself when
		// it is nested. Do not traverse through the pre-mutation container address.
		if (bChanged)
		{
			if (bOpen) ImGui::TreePop();
			return true;
		}

		if (bOpen)
		{
			for (uint64 Index = 0; Index < Num; ++Index)
			{
				void* Element = Property->GetMutableElementPtr(Container, Index, ArrayIndex);
				if (!Element) continue;
				ImGui::PushID(static_cast<int>(Index));
				const FReflectedPropertyEditTarget ElementTarget = EditTarget.ForArrayElement(Property->GetInner(), Element, Index);
				const bool bElementChanged = EditPropertyValue(Context, Object, Property->GetInner(), Element, 0, std::format("[{}]", Index), bReadOnly, ElementTarget);
				bChanged |= bElementChanged;
				ImGui::PopID();
				if (bElementChanged) break;
			}
			ImGui::TreePop();
		}
		return bChanged;
	}

	auto FReflectedPropertyView::EditMapProperty(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FMapProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FReflectedPropertyEditTarget& EditTarget
	) -> bool
	{
		if (!Property->HasMapHelper() || !Property->GetKeyProp() || !Property->GetValueProp())
		{
			MonaImGui::BeginPropertyRow(Label.c_str(), true);
			ImGui::TextDisabled("<map metadata unavailable>");
			MonaImGui::EndPropertyRow(true);
			return false;
		}

		uint64 Num = Property->Num(Container, ArrayIndex);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = MonaImGui::CompactTreeNode("##Map", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%llu)", Label.c_str(), Num);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		bool bChanged = false;
		auto SubmitStructure = [&](FReflectedPropertyEditTarget StructuralTarget, EPropertyChangeKind Kind, auto&& Mutation, bool bContinuous = false) -> bool {
			StructuralTarget.Kind = Kind;
			FPropertyValueSnapshot Proposed;
			std::string Error;
			if (!CaptureProposedPropertyValue(StructuralTarget.SnapshotProperty, StructuralTarget.SnapshotContainer,
				StructuralTarget.SnapshotArrayIndex, std::forward<decltype(Mutation)>(Mutation), Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			return SubmitPropertyEdit(Context, StructuralTarget, Proposed, bContinuous);
		};
		if (ImGui::SmallButton("+ Add"))
		{
			void* Key = Property->CreateKey();
			void* Value = Property->CreateValue();
			if (!Key || !Value)
			{
				ReportError(Context, "Unable to create a default map entry.");
			}
			else if (Property->Contains(Container, Key, ArrayIndex))
			{
				ReportError(Context, "Rename the existing default-key entry before adding another one.");
			}
			else
			{
				FReflectedPropertyEditTarget InsertTarget = EditTarget;
				InsertTarget.Path.back().Selector = EPropertyPathSelector::MapKey;
				InsertTarget.Path.back().MapKeyData = CaptureMapPathKey(Property->GetKeyProp(), Key);
				bChanged = SubmitStructure(std::move(InsertTarget), EPropertyChangeKind::MapInsert,
					[&] { Property->Insert(Container, Key, Value, ArrayIndex); });
				if (bChanged) Num = Property->Num(Container, ArrayIndex);
			}
			Property->DestroyKey(Key);
			Property->DestroyValue(Value);
		}
		if (bReadOnly) ImGui::EndDisabled();
		// A nested map address may have moved when the outer member snapshot was
		// restored and applied. Resume drawing from freshly resolved storage next frame.
		if (bChanged)
		{
			if (bOpen) ImGui::TreePop();
			return true;
		}

		if (bOpen)
		{
			for (uint64 Index = 0; Index < Num; ++Index)
			{
				const void* Key = Property->GetKeyPtr(Container, Index, ArrayIndex);
				void* Value = Property->GetMutableMappedValuePtr(Container, Index, ArrayIndex);
				if (!Key || !Value) continue;
				ImGui::PushID(Key);
				const std::vector<uint8> SerializedKey = CaptureMapPathKey(Property->GetKeyProp(), Key);

				void* EditedKey = Property->CreateKeyCopy(Key);
				FReflectedPropertyEditTarget KeyTarget = EditTarget.ForMapEntry(Property->GetKeyProp(), EditedKey, SerializedKey);
				KeyTarget.Kind = EPropertyChangeKind::MapKeyRename;
				const bool bKeyChanged = EditedKey && EditPropertyValue(Context, Object, Property->GetKeyProp(), EditedKey, 0, std::format("[{}] Key", Index), bReadOnly, KeyTarget, false);
				const MonaImGui::FPropertyEditWidgetState KeyState{
					ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()
				};
				const FReflectedPropertyEditTarget ValueTarget = EditTarget.ForMapEntry(Property->GetValueProp(), Value, SerializedKey);
				const bool bValueChanged = EditPropertyValue(Context, Object, Property->GetValueProp(), Value, 0, std::format("[{}] Value", Index), bReadOnly, ValueTarget);
				bChanged |= bValueChanged;
				if (bValueChanged)
				{
					Property->DestroyKey(EditedKey);
					ImGui::PopID();
					break;
				}

				MonaImGui::BeginPropertyRow(std::format("[{}] Actions", Index).c_str(), bReadOnly);
				const bool bRemove = ImGui::SmallButton("Remove");
				MonaImGui::EndPropertyRow(bReadOnly);
				if (bRemove)
				{
					FReflectedPropertyEditTarget RemoveTarget = EditTarget;
					RemoveTarget.Path.back().Selector = EPropertyPathSelector::MapKey;
					RemoveTarget.Path.back().MapKeyData = SerializedKey;
					bChanged |= SubmitStructure(std::move(RemoveTarget), EPropertyChangeKind::MapRemove, [&] { Property->Remove(Container, Key, ArrayIndex); });
				}
				else if (bKeyChanged && Property->Contains(Container, EditedKey, ArrayIndex))
				{
					ReportError(Context, "Map keys must be unique.");
				}
				else if (bKeyChanged)
				{
					bChanged |= SubmitStructure(KeyTarget, EPropertyChangeKind::MapKeyRename,
						[&] { Property->RenameKey(Container, Key, EditedKey, ArrayIndex); }, true);
				}
				if (KeyState.bDeactivatedAfterEdit && IsEditingTarget(KeyTarget)) FinishActiveEdit(&Context, false);
				else if (KeyState.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && IsEditingTarget(KeyTarget)) FinishActiveEdit(&Context, true);
				Property->DestroyKey(EditedKey);
				ImGui::PopID();
				// Rename and erase may change iteration order, so resume traversal next frame.
				if (bRemove || bKeyChanged) break;
			}
			ImGui::TreePop();
		}
		return bChanged;
	}

	auto FReflectedPropertyView::SubmitPropertyEdit(
		const FReflectedPropertyViewContext& Context,
		const FReflectedPropertyEditTarget& Target,
		const FPropertyValueSnapshot& ProposedValue,
		bool bContinuous
	) -> bool
	{
		if (EditSession.IsActive() && !IsEditingTarget(Target))
			FinishActiveEdit(&Context, false);

		std::string Error;
		if (!EditSession.IsActive())
		{
			const std::string Description = std::format("Edit {}", Target.MemberProperty->NamePrivate.ToString());
			if (!EditSession.Begin(Target, Description, nullptr, &Error, Context.Transactions))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			ActiveEditObject = Target.Object;
			ActiveEditOwnerObject = OwnerContextObject ? OwnerContextObject : Target.Object;
		}

		const EReflectedPropertyEditResult Result = EditSession.Apply(ProposedValue, &Error);
		if (Result == EReflectedPropertyEditResult::Failed)
		{
			ReportError(Context, std::move(Error));
			FinishActiveEdit(&Context, true);
			return false;
		}
		if (!bContinuous) FinishActiveEdit(&Context, false);
		return Result == EReflectedPropertyEditResult::Changed;
	}

	auto FReflectedPropertyView::SubmitPropertyValueEdit(
		const FReflectedPropertyViewContext& Context,
		const FReflectedPropertyEditTarget& Target,
		const std::function<void()>& AssignValue,
		bool bContinuous
	) -> bool
	{
		if (!AssignValue)
		{
			ReportError(Context, "The reflected property assignment is unavailable.");
			return false;
		}
		FPropertyValueSnapshot Proposed;
		std::string Error;
		if (!CaptureProposedPropertyValue(Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex,
			AssignValue, Proposed, &Error))
		{
			ReportError(Context, std::move(Error));
			return false;
		}
		return SubmitPropertyEdit(Context, Target, Proposed, bContinuous);
	}

	auto FReflectedPropertyView::SubmitStringMapValueEdit(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FMapProperty* Property,
		std::string_view Key,
		const std::function<void(FProperty*, void*)>& AssignValue,
		bool bContinuous
	) -> bool
	{
		if (!Object || !Property || !Property->GetKeyProp() || !Property->GetValueProp() || !AssignValue
			|| Property->GetKeyProp()->GetKind() != DurinCodeGen::EPropertyGenFlags::String)
		{
			ReportError(Context, "The reflected string-map value is unavailable.");
			return false;
		}

		const uint64 Index = FindStringMapIndex(Property, Object, Key);
		void* NewKey = nullptr;
		void* NewValue = nullptr;
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property);
		if (Index != UINT64_MAX)
		{
			const void* StoredKey = Property->GetKeyPtr(Object, Index);
			// Capturing/restoring the outer map can rehash it before Begin(), so the
			// leaf address is only descriptive; the stable member snapshot drives mutation.
			Target = Target.ForMapEntry(Property->GetValueProp(), Object, CaptureMapPathKey(Property->GetKeyProp(), StoredKey));
		}
		else
		{
			NewKey = Property->CreateKey();
			NewValue = Property->CreateValue();
			if (!NewKey || !NewValue)
			{
				if (NewKey) Property->DestroyKey(NewKey);
				if (NewValue) Property->DestroyValue(NewValue);
				ReportError(Context, "Unable to create a reflected string-map value.");
				return false;
			}
			*static_cast<FStringProperty*>(Property->GetKeyProp())->GetStringValuePtr(NewKey) = Key;
			Target = Target.ForMapEntry(Property->GetValueProp(), Object, CaptureMapPathKey(Property->GetKeyProp(), NewKey));
		}

		FPropertyValueSnapshot Proposed;
		std::string Error;
		const bool bCaptured = CaptureProposedPropertyValue(Property, Object, 0, [&] {
			if (Index != UINT64_MAX) AssignValue(Property->GetValueProp(), Property->GetMutableMappedValuePtr(Object, Index));
			else
			{
				AssignValue(Property->GetValueProp(), NewValue);
				Property->Insert(Object, NewKey, NewValue);
			}
		}, Proposed, &Error);
		if (NewKey) Property->DestroyKey(NewKey);
		if (NewValue) Property->DestroyValue(NewValue);
		if (!bCaptured)
		{
			ReportError(Context, std::move(Error));
			return false;
		}
		return SubmitPropertyEdit(Context, Target, Proposed, bContinuous);
	}

	auto FReflectedPropertyView::SetStringMapEntryEnabled(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FMapProperty* Property,
		std::string_view Key,
		bool bEnabled,
		const std::function<void(FProperty*, void*)>& InitializeValue
	) -> bool
	{
		if (!Object || !Property || !Property->GetKeyProp() || !Property->GetValueProp() || !InitializeValue
			|| Property->GetKeyProp()->GetKind() != DurinCodeGen::EPropertyGenFlags::String)
		{
			ReportError(Context, "The reflected string-map entry is unavailable.");
			return false;
		}
		const uint64 ExistingIndex = FindStringMapIndex(Property, Object, Key);
		if ((ExistingIndex != UINT64_MAX) == bEnabled) return false;

		void* EntryKey = Property->CreateKey();
		void* EntryValue = Property->CreateValue();
		if (!EntryKey || !EntryValue)
		{
			if (EntryKey) Property->DestroyKey(EntryKey);
			if (EntryValue) Property->DestroyValue(EntryValue);
			ReportError(Context, "Unable to create a reflected string-map entry.");
			return false;
		}
		*static_cast<FStringProperty*>(Property->GetKeyProp())->GetStringValuePtr(EntryKey) = Key;
		InitializeValue(Property->GetValueProp(), EntryValue);
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property);
		Target.Path.back().Selector = EPropertyPathSelector::MapKey;
		Target.Path.back().MapKeyData = CaptureMapPathKey(Property->GetKeyProp(), EntryKey);
		Target.Kind = bEnabled ? EPropertyChangeKind::MapInsert : EPropertyChangeKind::MapRemove;

		FPropertyValueSnapshot Proposed;
		std::string Error;
		const bool bCaptured = CaptureProposedPropertyValue(Property, Object, 0, [&] {
			if (bEnabled) Property->Insert(Object, EntryKey, EntryValue);
			else Property->Remove(Object, EntryKey);
		}, Proposed, &Error);
		Property->DestroyKey(EntryKey);
		Property->DestroyValue(EntryValue);
		if (!bCaptured)
		{
			ReportError(Context, std::move(Error));
			return false;
		}
		return SubmitPropertyEdit(Context, Target, Proposed, false);
	}

	auto FReflectedPropertyView::FinishActiveEdit(const FReflectedPropertyViewContext* Context, bool bCancel) -> void
	{
		if (!EditSession.IsActive()) return;
		std::string Error;
		const EReflectedPropertyEditResult Result = bCancel ? EditSession.Cancel(&Error) : EditSession.Commit(&Error);
		if (Result == EReflectedPropertyEditResult::Failed && Context) ReportError(*Context, std::move(Error));
		if (!EditSession.IsActive())
		{
			ActiveEditObject = nullptr;
			ActiveEditOwnerObject = nullptr;
		}
	}


	auto FReflectedPropertyView::HandleOwnerContext(const FReflectedPropertyViewContext& Context, DObject* Object) -> void
	{
		if (EditSession.IsActive() && (ActiveEditOwnerObject != Object || Context.bReadOnly))
			FinishActiveEdit(&Context, true);
		OwnerContextObject = Object;
	}

	auto FReflectedPropertyView::ReportError(const FReflectedPropertyViewContext& Context, std::string Error) const -> void
	{
		if (Context.ReportError) Context.ReportError(std::move(Error));
	}

	auto MakeReflectedPropertyDisplayName(std::string_view PropertyName, DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName) -> std::string
	{
		if (!ExplicitDisplayName.empty()) return std::string(ExplicitDisplayName);
		// The leading b is a C++ type convention, not part of a boolean's display name.
		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool && PropertyName.size() > 1 && PropertyName.front() == 'b'
			&& std::isupper(static_cast<unsigned char>(PropertyName[1])))
		{
			PropertyName.remove_prefix(1);
		}
		return StringUtils::HumanizeName(PropertyName);
	}

	auto MakeReflectedPropertyLabel(const FProperty& Property, uint32 ArrayIndex) -> std::string
	{
		static const FName DisplayNameMetaDataKey("DisplayName");
		std::string Label = MakeReflectedPropertyDisplayName(
			Property.NamePrivate.ToString(),
			Property.GetKind(),
			Property.GetMetaData(DisplayNameMetaDataKey)
		);
		if (Property.GetArrayDim() > 1) Label = std::format("{}[{}]", Label, ArrayIndex);
		return Label;
	}
}
