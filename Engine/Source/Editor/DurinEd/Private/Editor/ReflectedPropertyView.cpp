#include "Editor/ReflectedPropertyView.h"
#include "Editor/PropertyValueDraft.h"

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
			case DurinCodeGen::EPropertyGenFlags::Name: return "name";
			case DurinCodeGen::EPropertyGenFlags::Guid: return "guid";
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

		auto FormatEnumValue(const DEnum& Enum, uint64 Value) -> std::string
		{
			switch (Enum.GetUnderlyingType())
			{
			case DurinCodeGen::EEnumUnderlyingType::Int8:
			case DurinCodeGen::EEnumUnderlyingType::Int16:
			case DurinCodeGen::EEnumUnderlyingType::Int32:
			case DurinCodeGen::EEnumUnderlyingType::Int64:
			{
				int64 SignedValue;
				std::memcpy(&SignedValue, &Value, sizeof(SignedValue));
				return std::format("{}", SignedValue);
			}
			default: return std::format("{}", Value);
			}
		}

		template<typename TWriteProposed>
		auto CaptureProposedPropertyValue(const FReflectedPropertyEditTarget& Target,
			TWriteProposed&& WriteProposed, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) -> bool
		{
			FPropertyValueDraft Draft(Target, OutError);
			if (!Draft.IsValid()) return false;
			FReflectedPropertyEditTarget DraftTarget;
			if (!Draft.Resolve(Target, DraftTarget, OutError)) return false;
			WriteProposed(DraftTarget, &Draft);
			const bool bCaptured = Draft.Capture(OutSnapshot, OutError);
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

		auto FindMapIndex(const FMapProperty* Property, const DObject* Object, const FPropertyValueSnapshot& Key) -> uint64
		{
			if (!Property || !Property->GetKeyProp() || !Key.IsValid()) return UINT64_MAX;
			for (uint64 Index = 0; Index < Property->Num(Object); ++Index)
			{
				const void* StoredKey = Property->GetKeyPtr(Object, Index);
				FPropertyValueSnapshot StoredKeySnapshot;
				if (StoredKey && CapturePropertyValue(Property->GetKeyProp(), StoredKey, 0, StoredKeySnapshot)
					&& StoredKeySnapshot == Key) return Index;
			}
			return UINT64_MAX;
		}

		auto MatchesStableTarget(const FReflectedPropertyEditTarget& Left, const FReflectedPropertyEditTarget& Right) -> bool
		{
			if (Left.Object != Right.Object || Left.MemberProperty != Right.MemberProperty
				|| Left.LeafProperty != Right.LeafProperty || Left.SnapshotProperty != Right.SnapshotProperty
				|| Left.SnapshotArrayIndex != Right.SnapshotArrayIndex || Left.Path.size() != Right.Path.size()) return false;
			for (size_t Index = 0; Index < Left.Path.size(); ++Index)
			{
				const FReflectedPropertyEditPathSegment& A = Left.Path[Index];
				const FReflectedPropertyEditPathSegment& B = Right.Path[Index];
				if (A.Property != B.Property || A.Selector != B.Selector || A.Index != B.Index
					|| A.MapKeyData != B.MapKeyData || A.MapKey != B.MapKey) return false;
			}
			return true;
		}

		auto MakePropertySearchText(const FProperty& Property, uint32 ArrayIndex) -> std::string
		{
			const std::string SourceName = Property.GetArrayDim() > 1
				? std::format("{}[{}]", Property.NamePrivate.ToString(), ArrayIndex)
				: Property.NamePrivate.ToString();
			const std::string DisplayName = MakeReflectedPropertyLabel(Property, ArrayIndex);
			if (Property.GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
				&& static_cast<const FStructProperty&>(Property).GetStruct() == Z_Construct_DStruct_Durin_FTransform())
			{
				return std::format("{} {} Location Rotation Scale", SourceName, DisplayName);
			}
			return std::format("{} {}", SourceName, DisplayName);
		}
	}

	auto FReflectedPropertyBinding::IsPresent() const -> bool
	{
		return IsValid() && FindMapIndex(MemberProperty, Object, MapKey) != UINT64_MAX;
	}

	auto FReflectedPropertyView::EditObject(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		const FObjectPropertyViewOptions& Options
	) -> FObjectPropertyViewResult
	{
		if (!HandleOwnerContext(Context, Object)) return {};
		if (!Object)
		{
			if (Options.bShowEmptyMessage) ImGui::TextDisabled("Nothing to inspect.");
			return {};
		}

		struct FVisibleProperty
		{
			FProperty* Property = nullptr;
			uint32 ArrayIndex = 0;
		};
		std::vector<FVisibleProperty> VisibleProperties;
		Object->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit)) return;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				if (Options.Filter && !Options.Filter(*Property, ArrayIndex)) continue;
				if (!ContainsInsensitive(MakePropertySearchText(*Property, ArrayIndex), Options.SearchText)) continue;
				VisibleProperties.push_back({Property, ArrayIndex});
			}
		});

		FObjectPropertyViewResult Result{.VisiblePropertyCount = static_cast<uint32>(VisibleProperties.size())};
		if (VisibleProperties.empty())
		{
			if (Options.bShowEmptyMessage)
			{
				ImGui::TextDisabled(Options.SearchText.empty()
					? "This object has no reflected Edit properties."
					: "No properties match the current search.");
			}
			return Result;
		}

		const bool bOwnsPropertyTable = Options.bCreatePropertyTable;
		if (bOwnsPropertyTable && !MonaImGui::PropertyEdit::BeginTable(Options.PropertyTableId)) return Result;
		for (const FVisibleProperty& VisibleProperty : VisibleProperties)
		{
			Result.bChanged |= EditProperty(Context, Object, VisibleProperty.Property, VisibleProperty.ArrayIndex);
		}
		if (bOwnsPropertyTable) MonaImGui::PropertyEdit::EndTable();
		return Result;
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
		const FReflectedPropertyEditTarget& EditTarget
	) -> bool
	{
		bReadOnly |= Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Array:
			return EditArrayProperty(Context, Object, static_cast<FArrayProperty*>(Property), Container, ArrayIndex, Label, bReadOnly, EditTarget);
		case DurinCodeGen::EPropertyGenFlags::Map:
			return EditMapProperty(Context, Object, static_cast<FMapProperty*>(Property), Container, ArrayIndex, Label, bReadOnly, EditTarget);
		default:
			return SubmitWidgetEdit(Context, EditTarget, EditPropertyWidget(Context, Property, Container, ArrayIndex, Label, bReadOnly));
		}
	}

	auto FReflectedPropertyView::EditStructPropertyWidget(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly
	) -> FPropertyWidgetEditResult
	{
		FPropertyWidgetEditResult Result;
		DStruct* Struct = static_cast<FStructProperty*>(Property)->GetStruct();
		auto CaptureResult = [&](bool bChanged, bool bContinuous, const MonaImGui::PropertyEdit::FWidgetState& State = {}) {
			Result.bChanged = bChanged;
			Result.bContinuous = bContinuous;
			Result.bActive = State.bActive;
			Result.bDeactivatedAfterEdit = State.bDeactivatedAfterEdit;
		};

		auto EditMathStruct = [&]<typename TValue, typename TEditor>(TEditor&& Editor) -> FPropertyWidgetEditResult {
			TValue Value = *Property->ContainerPtrToValuePtr<TValue>(Container, ArrayIndex);
			MonaImGui::PropertyEdit::FWidgetState State;
			const bool bChanged = Editor(Value, State);
			CaptureResult(bChanged, true, State);
			if (bChanged) Result.AssignValue = [Value](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*DestinationProperty->ContainerPtrToValuePtr<TValue>(DestinationContainer, DestinationArrayIndex) = Value;
			};
			return Result;
		};
		auto EditVector = [&]<typename TVector>() -> FPropertyWidgetEditResult {
			return EditMathStruct.template operator()<TVector>([&](TVector& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditVector(Label.c_str(), Value, bReadOnly, 0.05, &State);
			});
		};

		if (Struct == Z_Construct_DStruct_Durin_FTransform())
		{
			return EditMathStruct.template operator()<FTransform>([&](FTransform& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditTransform(Label.c_str(), Value, bReadOnly, &State);
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FVector2())
			return EditVector.template operator()<FVector2>();

		if (Struct == Z_Construct_DStruct_Durin_FVector3())
			return EditVector.template operator()<FVector3>();

		if (Struct == Z_Construct_DStruct_Durin_FVector4())
			return EditVector.template operator()<FVector4>();

		if (Struct == Z_Construct_DStruct_Durin_FQuat())
		{
			return EditMathStruct.template operator()<FQuat>([&](FQuat& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditQuat(Label.c_str(), Value, bReadOnly, &State);
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FLinearColor())
		{
			const bool bShowAlpha = Property->GetMetaData(FName("HideAlpha")) != "true";
			return EditMathStruct.template operator()<FLinearColor>([&](FLinearColor& EditedValue, auto& State) {
				return MonaImGui::PropertyEdit::EditColor(Label.c_str(), EditedValue, bShowAlpha, bReadOnly, &State);
			});
		}

		MonaImGui::PropertyEdit::BeginRow(Label.c_str(), bReadOnly);
		ImGui::TextDisabled("<struct>");
		MonaImGui::PropertyEdit::EndRow(bReadOnly);
		return Result;
	}

	auto FReflectedPropertyView::EditPropertyWidget(
		const FReflectedPropertyViewContext& Context,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly
	) -> FPropertyWidgetEditResult
	{
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
		{
			return EditStructPropertyWidget(Property, Container, ArrayIndex, Label, bReadOnly);
		}

		FPropertyWidgetEditResult Result;
		auto CaptureResult = [&](bool bChanged, bool bContinuous, const MonaImGui::PropertyEdit::FWidgetState& State = {}) {
			Result.bChanged = bChanged;
			Result.bContinuous = bContinuous;
			Result.bActive = State.bActive;
			Result.bDeactivatedAfterEdit = State.bDeactivatedAfterEdit;
		};

		MonaImGui::PropertyEdit::BeginRow(Label.c_str(), bReadOnly);

		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool)
		{
			bool Value = *Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex);
			const bool bChanged = ImGui::Checkbox("##Value", &Value);
			CaptureResult(bChanged, false);
			if (bChanged) Result.AssignValue = [Value](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*DestinationProperty->ContainerPtrToValuePtr<bool>(DestinationContainer, DestinationArrayIndex) = Value;
			};
		}
		else if (const ImGuiDataType DataType = ImGuiDataTypeForProperty(Kind); DataType != ImGuiDataType_COUNT)
		{
			std::array<uint8, sizeof(uint64)> Value{};
			check(Property->GetElementSize() <= Value.size());
			std::memcpy(Value.data(), Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
			const bool bChanged = ImGui::DragScalar("##Value", DataType, Value.data(),
				Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f);
			const MonaImGui::PropertyEdit::FWidgetState State{
				ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()
			};
			CaptureResult(bChanged, true, State);
			if (bChanged)
			{
				const uint16 ElementSize = Property->GetElementSize();
				Result.AssignValue = [Value, ElementSize](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
					std::memcpy(DestinationProperty->GetValuePtr(DestinationContainer, DestinationArrayIndex), Value.data(), ElementSize);
				};
			}
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			auto* StringProperty = static_cast<FStringProperty*>(Property);
			std::string Value = *StringProperty->GetStringValuePtr(Container, ArrayIndex);
			const bool bChanged = MonaImGui::InputText("##Value", Value);
			CaptureResult(bChanged, true, {ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()});
			if (bChanged) Result.AssignValue = [Value = std::move(Value)](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*static_cast<FStringProperty*>(DestinationProperty)->GetStringValuePtr(DestinationContainer, DestinationArrayIndex) = Value;
			};
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Name)
		{
			auto* NameProperty = static_cast<FNameProperty*>(Property);
			std::string Value = NameProperty->GetNameValuePtr(Container, ArrayIndex)->ToString();
			const bool bChanged = MonaImGui::InputText("##Value", Value);
			CaptureResult(bChanged, true, {ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()});
			if (bChanged) Result.AssignValue = [Value = std::move(Value)](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*static_cast<FNameProperty*>(DestinationProperty)->GetNameValuePtr(DestinationContainer, DestinationArrayIndex) = FName(Value);
			};
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Guid)
		{
			auto* GuidProperty = static_cast<FGuidProperty*>(Property);
			std::string Value = GuidProperty->GetGuidValuePtr(Container, ArrayIndex)->ToString();
			const bool bTextChanged = MonaImGui::InputText("##Value", Value);
			const MonaImGui::PropertyEdit::FWidgetState State{
				ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()
			};
			FGuid ParsedValue;
			const bool bChanged = bTextChanged && FGuid::Parse(Value, ParsedValue);
			CaptureResult(bChanged, true, State);
			if (bChanged) Result.AssignValue = [ParsedValue](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*static_cast<FGuidProperty*>(DestinationProperty)->GetGuidValuePtr(DestinationContainer, DestinationArrayIndex) = ParsedValue;
			};
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
				const uint64 CurrentValue = EnumProperty->GetValueAsUInt64(Container, ArrayIndex);
				FName CurrentName;
				const std::string Preview = Enum->FindNameByValue(CurrentValue, CurrentName) ? CurrentName.ToString() : FormatEnumValue(*Enum, CurrentValue);
				if (ImGui::BeginCombo("##Value", Preview.c_str()))
				{
					Enum->ForEachValue([&](const FEnumValue& Value) {
						const bool bSelected = Value.Value == CurrentValue;
						if (ImGui::Selectable(Value.Name.ToString().c_str(), bSelected))
						{
							CaptureResult(true, false);
							Result.AssignValue = [ProposedValue = Value.Value](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
								static_cast<FEnumProperty*>(DestinationProperty)->SetValueFromUInt64(DestinationContainer, ProposedValue, DestinationArrayIndex);
							};
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
			bool bChanged = false;
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
			CaptureResult(bChanged, false);
			if (bChanged) Result.AssignValue = [SelectedObject](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				static_cast<FObjectProperty*>(DestinationProperty)->SetObjectPropertyValue(DestinationContainer, SelectedObject, DestinationArrayIndex);
			};
		}
		else
		{
			ImGui::TextDisabled("<%s>", PropertyKindName(Kind));
		}

		MonaImGui::PropertyEdit::EndRow(bReadOnly);
		return Result;
	}

	auto FReflectedPropertyView::SubmitWidgetEdit(
		const FReflectedPropertyViewContext& Context,
		const FReflectedPropertyEditTarget& EditTarget,
		const FPropertyWidgetEditResult& Edit
	) -> bool
	{
		bool bChanged = false;
		if (Edit.bChanged && Edit.AssignValue)
		{
			FPropertyValueSnapshot Proposed;
			std::string Error;
			if (!CaptureProposedPropertyValue(EditTarget,
				[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft*) {
					Edit.AssignValue(const_cast<FProperty*>(ScratchTarget.LeafProperty),
						ScratchTarget.LeafContainer, ScratchTarget.LeafArrayIndex);
				}, Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
			}
			else
			{
				bChanged = SubmitPropertyEdit(Context, EditTarget, Proposed, Edit.bContinuous);
			}
		}
		if (Edit.bDeactivatedAfterEdit && IsEditingTarget(EditTarget)) FinishActiveEdit(&Context, false);
		else if (Edit.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && IsEditingTarget(EditTarget)) FinishActiveEdit(&Context, true);
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
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true);
			ImGui::TextDisabled("<array metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
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
			if (!CaptureProposedPropertyValue(StructuralTarget,
				[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft*) {
					Mutation(*static_cast<const FArrayProperty*>(ScratchTarget.LeafProperty), ScratchTarget.LeafContainer, ScratchTarget.LeafArrayIndex);
				}, Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			return SubmitPropertyEdit(Context, StructuralTarget, Proposed, false);
		};
		if (ImGui::SmallButton("+"))
		{
			bChanged = SubmitStructure(EPropertyChangeKind::ArrayAdd,
				[&](const FArrayProperty& ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					ScratchProperty.Resize(ScratchContainer, Num + 1, ScratchArrayIndex);
				});
			if (bChanged) ++Num;
		}
		ImGui::SameLine();
		if (Num == 0) ImGui::BeginDisabled();
		if (ImGui::SmallButton("-") && Num > 0)
		{
			bChanged = SubmitStructure(EPropertyChangeKind::ArrayRemove,
				[&](const FArrayProperty& ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					ScratchProperty.Resize(ScratchContainer, Num - 1, ScratchArrayIndex);
				});
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
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true);
			ImGui::TextDisabled("<map metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
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
			std::string MutationError;
			if (!CaptureProposedPropertyValue(StructuralTarget,
				[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft* Draft) {
					Mutation(ScratchTarget, *Draft, MutationError);
				}, Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			if (!MutationError.empty())
			{
				ReportError(Context, std::move(MutationError));
				return false;
			}
			return SubmitPropertyEdit(Context, StructuralTarget, Proposed, bContinuous);
		};
		if (ImGui::SmallButton("+ Add"))
		{
			void* Key = Property->CreateKey();
			void* Value = Property->CreateValue();
			FPropertyValueSnapshot KeySnapshot;
			FPropertyValueSnapshot ValueSnapshot;
			std::string Error;
			if (!Key || !Value
				|| !CapturePropertyValue(Property->GetKeyProp(), Key, 0, KeySnapshot, &Error)
				|| !CapturePropertyValue(Property->GetValueProp(), Value, 0, ValueSnapshot, &Error))
			{
				ReportError(Context, Error.empty() ? "Unable to create a map-entry draft." : std::move(Error));
			}
			else
			{
				MapInsertDraft.Target = EditTarget;
				MapInsertDraft.Key = std::move(KeySnapshot);
				MapInsertDraft.Value = std::move(ValueSnapshot);
				MapInsertDraft.bActive = true;
			}
			if (Key) Property->DestroyKey(Key);
			if (Value) Property->DestroyValue(Value);
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
			if (MapInsertDraft.bActive && MatchesStableTarget(MapInsertDraft.Target, EditTarget))
			{
				ImGui::PushID("MapInsertDraft");
				void* DraftKey = Property->CreateKey();
				void* DraftValue = Property->CreateValue();
				std::string Error;
				if (!DraftKey || !DraftValue
					|| !RestorePropertyValue(Property->GetKeyProp(), DraftKey, 0, MapInsertDraft.Key, &Error)
					|| !RestorePropertyValue(Property->GetValueProp(), DraftValue, 0, MapInsertDraft.Value, &Error))
				{
					ReportError(Context, Error.empty() ? "Unable to restore the map-entry draft." : std::move(Error));
					MapInsertDraft = {};
				}
				else
				{
					const FPropertyWidgetEditResult KeyEdit = EditPropertyWidget(
						Context, Property->GetKeyProp(), DraftKey, 0, "New Key", bReadOnly);
					if (KeyEdit.bChanged && KeyEdit.AssignValue)
					{
						KeyEdit.AssignValue(Property->GetKeyProp(), DraftKey, 0);
						if (!CapturePropertyValue(Property->GetKeyProp(), DraftKey, 0, MapInsertDraft.Key, &Error))
							ReportError(Context, std::move(Error));
					}
					const FPropertyWidgetEditResult ValueEdit = EditPropertyWidget(
						Context, Property->GetValueProp(), DraftValue, 0, "New Value", bReadOnly);
					if (ValueEdit.bChanged && ValueEdit.AssignValue)
					{
						ValueEdit.AssignValue(Property->GetValueProp(), DraftValue, 0);
						if (!CapturePropertyValue(Property->GetValueProp(), DraftValue, 0, MapInsertDraft.Value, &Error))
							ReportError(Context, std::move(Error));
					}

					MonaImGui::PropertyEdit::BeginRow("New Entry", bReadOnly);
					const bool bConfirmInsert = ImGui::SmallButton("Add");
					ImGui::SameLine();
					const bool bCancelInsert = ImGui::SmallButton("Cancel")
						|| ((KeyEdit.bActive || ValueEdit.bActive) && ImGui::IsKeyPressed(ImGuiKey_Escape));
					MonaImGui::PropertyEdit::EndRow(bReadOnly);
					if (bCancelInsert)
					{
						MapInsertDraft = {};
					}
					else if (bConfirmInsert)
					{
						if (Property->Contains(Container, DraftKey, ArrayIndex))
						{
							ReportError(Context, "Map keys must be unique.");
						}
						else
						{
							FReflectedPropertyEditTarget InsertTarget = EditTarget;
							InsertTarget.Path.back().Selector = EPropertyPathSelector::MapKey;
							InsertTarget.Path.back().MapKeyData = CaptureMapPathKey(Property->GetKeyProp(), DraftKey);
							InsertTarget.Path.back().MapKey = MapInsertDraft.Key;
							bChanged = SubmitStructure(std::move(InsertTarget), EPropertyChangeKind::MapInsert,
								[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft&, std::string&) {
									auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchTarget.LeafProperty);
									ScratchProperty->Insert(ScratchTarget.LeafContainer, DraftKey, DraftValue, ScratchTarget.LeafArrayIndex);
								});
							if (bChanged) MapInsertDraft = {};
						}
					}
				}
				if (DraftKey) Property->DestroyKey(DraftKey);
				if (DraftValue) Property->DestroyValue(DraftValue);
				ImGui::PopID();
				if (bChanged)
				{
					ImGui::TreePop();
					return true;
				}
			}

			for (uint64 Index = 0; Index < Num; ++Index)
			{
				const void* Key = Property->GetKeyPtr(Container, Index, ArrayIndex);
				void* Value = Property->GetMutableMappedValuePtr(Container, Index, ArrayIndex);
				if (!Key || !Value) continue;
				ImGui::PushID(Key);
				FPropertyValueSnapshot KeySnapshot;
				if (!CapturePropertyValue(Property->GetKeyProp(), Key, 0, KeySnapshot))
				{
					ImGui::PopID();
					continue;
				}
				const std::vector<uint8> SerializedKey = CaptureMapPathKey(Property->GetKeyProp(), Key);

				FReflectedPropertyEditTarget KeyTarget = EditTarget.ForMapEntry(
					Property->GetKeyProp(), const_cast<void*>(Key), KeySnapshot, SerializedKey);
				KeyTarget.Kind = EPropertyChangeKind::MapKeyRename;
				const FPropertyWidgetEditResult KeyEdit = EditPropertyWidget(
					Context, Property->GetKeyProp(), const_cast<void*>(Key), 0, std::format("[{}] Key", Index), bReadOnly);
				const bool bKeyChanged = KeyEdit.bChanged;
				const FReflectedPropertyEditTarget ValueTarget = EditTarget.ForMapEntry(Property->GetValueProp(), Value, KeySnapshot, SerializedKey);
				const bool bValueChanged = EditPropertyValue(Context, Object, Property->GetValueProp(), Value, 0, std::format("[{}] Value", Index), bReadOnly, ValueTarget);
				bChanged |= bValueChanged;
				if (bValueChanged)
				{
					ImGui::PopID();
					break;
				}

				MonaImGui::PropertyEdit::BeginRow(std::format("[{}] Actions", Index).c_str(), bReadOnly);
				const bool bRemove = ImGui::SmallButton("Remove");
				MonaImGui::PropertyEdit::EndRow(bReadOnly);
				if (bRemove)
				{
					FReflectedPropertyEditTarget RemoveTarget = EditTarget;
					RemoveTarget.Path.back().Selector = EPropertyPathSelector::MapKey;
					RemoveTarget.Path.back().MapKeyData = SerializedKey;
					RemoveTarget.Path.back().MapKey = KeySnapshot;
					bChanged |= SubmitStructure(std::move(RemoveTarget), EPropertyChangeKind::MapRemove,
						[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft&, std::string&) {
							auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchTarget.LeafProperty);
							ScratchProperty->Remove(ScratchTarget.LeafContainer, Key, ScratchTarget.LeafArrayIndex);
						});
				}
				else if (bKeyChanged)
				{
					void* ProposedKey = Property->CreateKey();
					if (!ProposedKey || !KeyEdit.AssignValue)
					{
						ReportError(Context, "Unable to materialize the proposed map key.");
					}
					else
					{
						KeyEdit.AssignValue(Property->GetKeyProp(), ProposedKey, 0);
						FPropertyValueSnapshot ProposedKeySnapshot;
						std::string CaptureError;
						if (!CapturePropertyValue(Property->GetKeyProp(), ProposedKey, 0, ProposedKeySnapshot, &CaptureError))
						{
							ReportError(Context, std::move(CaptureError));
						}
						else if (!(ProposedKeySnapshot == KeySnapshot) && Property->Contains(Container, ProposedKey, ArrayIndex))
						{
							ReportError(Context, "Map keys must be unique.");
						}
						else
						{
							bChanged |= SubmitStructure(KeyTarget, EPropertyChangeKind::MapKeyRename,
								[&](const FReflectedPropertyEditTarget&, FPropertyValueDraft& Draft, std::string& MutationError) {
									FReflectedPropertyEditTarget ScratchMapTarget;
									if (!Draft.Resolve(EditTarget, ScratchMapTarget, &MutationError)) return;
									auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchMapTarget.LeafProperty);
									if (!ScratchProperty->RenameKey(ScratchMapTarget.LeafContainer, Key, ProposedKey,
										ScratchMapTarget.LeafArrayIndex)) MutationError = "Unable to rename the reflected map key.";
								}, true);
						}
					}
					if (ProposedKey) Property->DestroyKey(ProposedKey);
				}
				if (KeyEdit.bDeactivatedAfterEdit && IsEditingTarget(KeyTarget)) FinishActiveEdit(&Context, false);
				else if (KeyEdit.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && IsEditingTarget(KeyTarget)) FinishActiveEdit(&Context, true);
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
			if (!EditSession.Begin(Target, {}, nullptr, &Error, Context.Transactions))
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
		const std::function<void(FProperty*, void*, uint32)>& AssignValue,
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
		if (!CaptureProposedPropertyValue(Target,
			[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft*) {
				AssignValue(const_cast<FProperty*>(ScratchTarget.LeafProperty), ScratchTarget.LeafContainer, ScratchTarget.LeafArrayIndex);
			}, Proposed, &Error))
		{
			ReportError(Context, std::move(Error));
			return false;
		}
		const bool bSubmitted = SubmitPropertyEdit(Context, Target, Proposed, bContinuous);
		return bSubmitted;
	}

	auto FReflectedPropertyView::BindStringMapValue(
		DObject* Object,
		FMapProperty* Property,
		std::string_view Key
	) const -> FReflectedPropertyBinding
	{
		FReflectedPropertyBinding Binding;
		if (!Object || !Property || !Property->GetKeyProp() || !Property->GetValueProp()
			|| Property->GetKeyProp()->GetKind() != DurinCodeGen::EPropertyGenFlags::String) return Binding;

		void* KeyValue = Property->CreateKey();
		if (!KeyValue) return Binding;
		*static_cast<FStringProperty*>(Property->GetKeyProp())->GetStringValuePtr(KeyValue) = Key;
		if (CapturePropertyValue(Property->GetKeyProp(), KeyValue, 0, Binding.MapKey))
		{
			Binding.Object = Object;
			Binding.MemberProperty = Property;
			Binding.LeafProperty = Property->GetValueProp();
			Binding.PathKeyData = CaptureMapPathKey(Property->GetKeyProp(), KeyValue);
		}
		Property->DestroyKey(KeyValue);
		return Binding;
	}

	auto FReflectedPropertyView::SubmitBoundPropertyValueEdit(
		const FReflectedPropertyViewContext& Context,
		const FReflectedPropertyBinding& Binding,
		const std::function<void(FProperty*, void*)>& AssignValue,
		bool bContinuous
	) -> bool
	{
		if (!Binding.IsValid() || !AssignValue)
		{
			ReportError(Context, "The reflected property binding is unavailable.");
			return false;
		}

		FMapProperty* Property = Binding.MemberProperty;
		DObject* Object = Binding.Object;
		void* Key = Property->CreateKey();
		void* Value = Property->CreateValue();
		std::string Error;
		if (!Key || !Value || !RestorePropertyValue(Property->GetKeyProp(), Key, 0, Binding.MapKey, &Error))
		{
			if (Key) Property->DestroyKey(Key);
			if (Value) Property->DestroyValue(Value);
			ReportError(Context, Error.empty() ? "Unable to resolve the reflected property binding." : std::move(Error));
			return false;
		}

		const uint64 Index = FindMapIndex(Property, Object, Binding.MapKey);
		if (Index != UINT64_MAX)
		{
			FPropertyValueSnapshot CurrentValue;
			if (!CapturePropertyValue(Property->GetValueProp(), Property->GetMappedValuePtr(Object, Index), 0, CurrentValue, &Error)
				|| !RestorePropertyValue(Property->GetValueProp(), Value, 0, CurrentValue, &Error))
			{
				Property->DestroyKey(Key);
				Property->DestroyValue(Value);
				ReportError(Context, std::move(Error));
				return false;
			}
		}
		AssignValue(Binding.LeafProperty, Value);

		// Leaf storage is deliberately scratch memory. The binding resolves the live
		// map on each submission while the target snapshots the stable member root.
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property)
			.ForMapEntry(Binding.LeafProperty, Object, Binding.MapKey, Binding.PathKeyData);

		FPropertyValueSnapshot Proposed;
		const FReflectedPropertyEditTarget RootTarget = FReflectedPropertyEditTarget::ForMember(Object, Property);
		const bool bCaptured = CaptureProposedPropertyValue(RootTarget,
			[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft*) {
				auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchTarget.LeafProperty);
				ScratchProperty->Insert(ScratchTarget.LeafContainer, Key, Value, ScratchTarget.LeafArrayIndex);
			}, Proposed, &Error);
		Property->DestroyKey(Key);
		Property->DestroyValue(Value);
		if (!bCaptured)
		{
			ReportError(Context, std::move(Error));
			return false;
		}
		return SubmitPropertyEdit(Context, Target, Proposed, bContinuous);
	}

	auto FReflectedPropertyView::SetBoundPropertyEnabled(
		const FReflectedPropertyViewContext& Context,
		const FReflectedPropertyBinding& Binding,
		bool bEnabled,
		const std::function<void(FProperty*, void*)>& InitializeValue
	) -> bool
	{
		if (!Binding.IsValid() || (bEnabled && !InitializeValue))
		{
			ReportError(Context, "The reflected property binding is unavailable.");
			return false;
		}
		FMapProperty* Property = Binding.MemberProperty;
		DObject* Object = Binding.Object;
		const uint64 ExistingIndex = FindMapIndex(Property, Object, Binding.MapKey);
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
		std::string Error;
		if (!RestorePropertyValue(Property->GetKeyProp(), EntryKey, 0, Binding.MapKey, &Error))
		{
			Property->DestroyKey(EntryKey);
			Property->DestroyValue(EntryValue);
			ReportError(Context, std::move(Error));
			return false;
		}
		if (bEnabled) InitializeValue(Binding.LeafProperty, EntryValue);
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property);
		Target.Path.back().Selector = EPropertyPathSelector::MapKey;
		Target.Path.back().MapKeyData = Binding.PathKeyData;
		Target.Path.back().MapKey = Binding.MapKey;
		Target.Kind = bEnabled ? EPropertyChangeKind::MapInsert : EPropertyChangeKind::MapRemove;

		FPropertyValueSnapshot Proposed;
		const FReflectedPropertyEditTarget RootTarget = FReflectedPropertyEditTarget::ForMember(Object, Property);
		const bool bCaptured = CaptureProposedPropertyValue(RootTarget,
			[&](const FReflectedPropertyEditTarget& ScratchTarget, FPropertyValueDraft*) {
				auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchTarget.LeafProperty);
				if (bEnabled) ScratchProperty->Insert(ScratchTarget.LeafContainer, EntryKey, EntryValue, ScratchTarget.LeafArrayIndex);
				else ScratchProperty->Remove(ScratchTarget.LeafContainer, EntryKey, ScratchTarget.LeafArrayIndex);
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

	auto FReflectedPropertyView::FinishActiveEdit(const FReflectedPropertyViewContext* Context, bool bCancel) -> bool
	{
		if (!EditSession.IsActive()) return true;
		std::string Error;
		const EReflectedPropertyEditResult Result = bCancel ? EditSession.Cancel(&Error) : EditSession.Commit(&Error);
		if (Result == EReflectedPropertyEditResult::Failed && Context) ReportError(*Context, std::move(Error));
		if (!EditSession.IsActive())
		{
			ActiveEditObject = nullptr;
			ActiveEditOwnerObject = nullptr;
		}
		return Result != EReflectedPropertyEditResult::Failed;
	}


	auto FReflectedPropertyView::HandleOwnerContext(const FReflectedPropertyViewContext& Context, DObject* Object) -> bool
	{
		if (EditSession.IsActive() && (ActiveEditOwnerObject != Object || Context.bReadOnly))
		{
			if (!FinishActiveEdit(&Context, true)) return false;
		}
		if (OwnerContextObject != Object || Context.bReadOnly) MapInsertDraft = {};
		OwnerContextObject = Object;
		return true;
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
