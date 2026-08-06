#include "Editor/ReflectedPropertyView.h"
#include "Editor/PropertyValueDraft.h"

#include "AssetSystem.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "DObject/Package.h"
#include "Editor/EditorAssetPicker.h"
#include "Icons/FontAwesomeIcons.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"

namespace Durin
{
	namespace
	{
		using StringUtils::ContainsInsensitive;

		struct FEditableMapEntry
		{
			const void* Key = nullptr;
			void* Value = nullptr;
		};

		auto CollectEditableMapEntry(void* Context, const void* Key, void* Value) -> bool
		{
			static_cast<std::vector<FEditableMapEntry>*>(Context)->push_back({Key, Value});
			return true;
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
			case DurinCodeGen::EPropertyGenFlags::SoftObject: return "soft object";
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
			FResolvedPropertyValue DraftValue;
			if (!Draft.Resolve(Target, DraftValue.Property, DraftValue.Container, DraftValue.ArrayIndex, OutError)) return false;
			WriteProposed(DraftValue, &Draft);
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

		auto MatchesStableTarget(const FReflectedPropertyEditTarget& Left, const FReflectedPropertyEditTarget& Right) -> bool
		{
			if (Left.Object != Right.Object || Left.MemberProperty != Right.MemberProperty
				|| Left.LeafProperty != Right.LeafProperty || Left.SnapshotProperty != Right.SnapshotProperty
				|| Left.SnapshotArrayIndex != Right.SnapshotArrayIndex
				|| Left.LogicalIdentity != Right.LogicalIdentity || Left.Path.size() != Right.Path.size()) return false;
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

	auto GetSoftObjectPropertyStateLabel(ESoftObjectPropertyViewState State) -> std::string_view
	{
		switch (State)
		{
		case ESoftObjectPropertyViewState::Null: return "Null";
		case ESoftObjectPropertyViewState::Unloaded: return "Unloaded";
		case ESoftObjectPropertyViewState::Loaded: return "Loaded";
		case ESoftObjectPropertyViewState::Missing: return "Missing";
		case ESoftObjectPropertyViewState::TypeMismatch: return "Type mismatch";
		default: return "Unknown";
		}
	}

	auto InspectSoftObjectProperty(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex
	) -> FSoftObjectPropertyViewState
	{
		FSoftObjectPropertyViewState ViewState;
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim())
		{
			ViewState.State = ESoftObjectPropertyViewState::TypeMismatch;
			ViewState.Message = "Soft object property metadata or storage is unavailable.";
			return ViewState;
		}
		FSoftObjectPtr* Reference = Property->GetSoftObjectPtr(Container, ArrayIndex);
		if (!Reference)
		{
			ViewState.State = ESoftObjectPropertyViewState::TypeMismatch;
			ViewState.Message = "Soft object property has no typed value accessor.";
			return ViewState;
		}
		if (Reference->IsNull()) return ViewState;
		ViewState.Path = Reference->GetSoftObjectPath().GetAssetPath();

		const Asset::FSoftObjectResolveResult Resolve = Asset::ResolveSoftObject(
			*Reference, Property->GetExpectedClass(), Asset::ESoftObjectNullPolicy::Reject);
		if (!Resolve)
		{
			ViewState.State = ESoftObjectPropertyViewState::TypeMismatch;
			ViewState.Message = Resolve.Result.Message;
			return ViewState;
		}
		if (Resolve.State == Asset::ESoftObjectResolveState::Loaded)
		{
			ViewState.State = ESoftObjectPropertyViewState::Loaded;
			ViewState.LoadedObject = Resolve.Object;
			return ViewState;
		}
		if (!Asset::GetAssetRegistry().FindAsset(ViewState.Path))
		{
			ViewState.State = ESoftObjectPropertyViewState::Missing;
			ViewState.Message = std::format("Asset {} is not present in the registry.", ViewState.Path.ToString());
			return ViewState;
		}
		ViewState.State = ESoftObjectPropertyViewState::Unloaded;
		return ViewState;
	}

	auto LoadSoftObjectProperty(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		DObject*& OutObject,
		std::string* OutError
	) -> bool
	{
		if (OutError) OutError->clear();
		OutObject = nullptr;
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim())
		{
			if (OutError) *OutError = "Soft object property metadata or storage is unavailable.";
			return false;
		}
		FSoftObjectPtr* Reference = Property->GetSoftObjectPtr(Container, ArrayIndex);
		if (!Reference)
		{
			if (OutError) *OutError = "Soft object property has no typed value accessor.";
			return false;
		}
		const Asset::FAssetResult Result = Asset::LoadSoftObject(
			*Reference, Property->GetExpectedClass(), OutObject, Asset::ESoftObjectNullPolicy::Reject);
		if (!Result)
		{
			if (OutError) *OutError = Result.Message;
			return false;
		}
		return OutObject != nullptr;
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

		// Couples a reflected property with the display label selected by filtering.
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
				const FEnumValue* CurrentRecord = Enum->FindValueRecordByValue(CurrentValue);
				const std::string Preview = CurrentRecord ? CurrentRecord->DisplayName : FormatEnumValue(*Enum, CurrentValue);
				if (ImGui::BeginCombo("##Value", Preview.c_str()))
				{
					Enum->ForEachValue([&](const FEnumValue& Value) {
						const bool bSelected = Value.Value == CurrentValue;
						const std::string Label = std::format("{}##EnumValue_{}", Value.DisplayName, Value.Name.ToString());
						if (ImGui::Selectable(Label.c_str(), bSelected))
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
			DClass* RequiredClass = ObjectProperty->GetReferencedClass();
			DObject* Current = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
			DObject* SelectedObject = Current;
			const FEditorAssetPickerResult PickerResult = EditorAssetPicker::Draw({
				.ComboId = "##Value",
				.SearchId = "##ObjectSearch",
				.SearchHint = "Search assets...",
				.RequiredClass = RequiredClass,
				.ClassPolicy = EEditorAssetClassPolicy::Derived,
				.CurrentSelection = Current,
				.SearchText = AssetSearchText,
				.bAllowNone = true,
				.AssignSelection = [&](DObject* Selection, std::string& OutError) {
					if (Selection == Current) { SelectedObject = nullptr; return true; }
					if (RequiredClass && Selection && !EditorAssetPicker::MatchesClass(Selection->GetClass(), RequiredClass, EEditorAssetClassPolicy::Derived))
					{
						OutError = "The selected asset does not match the required class.";
						return false;
					}
					SelectedObject = Selection;
					return true;
				},
			});
			if (!PickerResult.Error.empty()) ReportError(Context, PickerResult.Error);
			const bool bChanged = PickerResult.bSelectionChanged;
			CaptureResult(bChanged, false);
			if (bChanged) Result.AssignValue = [SelectedObject](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				static_cast<FObjectProperty*>(DestinationProperty)->SetObjectPropertyValue(DestinationContainer, SelectedObject, DestinationArrayIndex);
			};
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::SoftObject)
		{
			auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
			FSoftObjectPtr* CurrentReference = SoftProperty->GetSoftObjectPtr(Container, ArrayIndex);
			const FSoftObjectPropertyViewState ViewState = InspectSoftObjectProperty(SoftProperty, Container, ArrayIndex);
			FSoftObjectPath SelectedPath = CurrentReference ? CurrentReference->GetSoftObjectPath() : FSoftObjectPath();
			const std::string_view StateLabel = GetSoftObjectPropertyStateLabel(ViewState.State);
			const bool bCanLoad = ViewState.State == ESoftObjectPropertyViewState::Unloaded
				|| ViewState.State == ESoftObjectPropertyViewState::Missing;
			const bool bCanReveal = ViewState.Path.IsValid()
				&& Asset::GetAssetRegistry().FindAsset(ViewState.Path) && Context.RevealAsset;
			const bool bCanOpen = ViewState.State == ESoftObjectPropertyViewState::Loaded && Context.OpenAsset;

			const FEditorAssetPickerAction LoadAction{
				.Icon = Icons::Play,
				.ButtonId = "LoadSoftObject",
				.Tooltip = bCanLoad ? "Load the referenced asset." : "The referenced asset is not loadable in its current state.",
				.bEnabled = bCanLoad,
				.Execute = [SoftProperty, Container, ArrayIndex](std::string& Error) {
					DObject* LoadedObject = nullptr;
					if (!LoadSoftObjectProperty(SoftProperty, Container, ArrayIndex, LoadedObject, &Error)) return false;
					return LoadedObject != nullptr;
				},
			};
			const std::array<FEditorAssetPickerAction, 2> AdditionalActions{{
				{
					.Icon = Icons::Eye,
					.ButtonId = "RevealSoftObject",
					.Tooltip = "Reveal the referenced asset in the Content Browser.",
					.bEnabled = bCanReveal,
					.Execute = [&Context, &ViewState](std::string& Error) {
						return Context.RevealAsset && Context.RevealAsset(ViewState.Path, Error);
					},
				},
				{
					.Icon = Icons::FolderOpen,
					.ButtonId = "OpenSoftObject",
					.Tooltip = "Open the loaded referenced asset.",
					.bEnabled = bCanOpen,
					.Execute = [&Context, &ViewState](std::string& Error) {
						return Context.OpenAsset && Context.OpenAsset(ViewState.Path, Error);
					},
				},
			}};
			const FEditorAssetPickerResult PickerResult = EditorAssetPicker::Draw({
				.ComboId = "##Value",
				.SearchId = "##SoftObjectSearch",
				.SearchHint = "Search assets...",
				.RequiredClass = SoftProperty->GetExpectedClass(),
				.ClassPolicy = EEditorAssetClassPolicy::Derived,
				.AssignmentMode = EEditorAssetAssignmentMode::AssetPath,
				.CurrentSelection = ViewState.LoadedObject,
				.CurrentSelectionPath = ViewState.Path.GetView(),
				.CurrentSelectionStatus = StateLabel,
				.SearchText = AssetSearchText,
				.bAllowNone = true,
				.AssignPathSelection = [&SelectedPath](std::string_view Path, std::string& Error) {
					if (Path.empty())
					{
						SelectedPath.Reset();
						return true;
					}
					return FSoftObjectPath::TryCreate(Path, SelectedPath, &Error);
				},
				.TrailingAction = LoadAction,
				.AdditionalTrailingActions = AdditionalActions,
			});
			if (!PickerResult.Error.empty()) ReportError(Context, PickerResult.Error);
			CaptureResult(PickerResult.bSelectionChanged, false);
			if (PickerResult.bSelectionChanged)
			{
				Result.AssignValue = [SelectedPath](
					FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
					auto* Destination = static_cast<FSoftObjectProperty*>(DestinationProperty)
						->GetSoftObjectPtr(DestinationContainer, DestinationArrayIndex);
					if (Destination) Destination->SetPath(SelectedPath);
				};
			}
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
				[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft*) {
					Edit.AssignValue(const_cast<FProperty*>(DraftValue.Property),
						DraftValue.Container, DraftValue.ArrayIndex);
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
		if (!Property->HasArrayOps() || !Property->GetInner())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true);
			ImGui::TextDisabled("<array metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		uint64 Num = 0;
		if (!Property->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess)
			|| Property->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true);
			ImGui::TextDisabled("<array Count/RandomAccess unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}
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
				[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft*) {
					Mutation(*static_cast<const FArrayProperty*>(DraftValue.Property), DraftValue.Container, DraftValue.ArrayIndex);
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
				[&](const FArrayProperty& DraftProperty, void* DraftContainer, uint32 DraftArrayIndex) {
					DraftProperty.Resize(DraftContainer, Num + 1, DraftArrayIndex);
				});
			if (bChanged) ++Num;
		}
		ImGui::SameLine();
		if (Num == 0) ImGui::BeginDisabled();
		if (ImGui::SmallButton("-") && Num > 0)
		{
			bChanged = SubmitStructure(EPropertyChangeKind::ArrayRemove,
				[&](const FArrayProperty& DraftProperty, void* DraftContainer, uint32 DraftArrayIndex) {
					DraftProperty.Resize(DraftContainer, Num - 1, DraftArrayIndex);
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
				const FReflectedPropertyEditTarget ElementTarget = EditTarget.ForArrayElement(Property->GetInner(), Index);
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
		if (!Property->HasMapOps() || !Property->GetKeyProp() || !Property->GetValueProp())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true);
			ImGui::TextDisabled("<map metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		uint64 Num = 0;
		if (!Property->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::MutableMappedTraversal)
			|| Property->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true);
			ImGui::TextDisabled("<map Count/MutableMappedTraversal unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}
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
				[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft* Draft) {
					Mutation(DraftValue, *Draft, MutationError);
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
			FReflectedValueStorage KeyStorage;
			FReflectedValueStorage ValueStorage;
			FPropertyValueSnapshot KeySnapshot;
			FPropertyValueSnapshot ValueSnapshot;
			std::string Error;
			if (!KeyStorage.DefaultConstruct(Property->GetKeyProp(), 0, &Error)
				|| !ValueStorage.DefaultConstruct(Property->GetValueProp(), 0, &Error)
				|| !CapturePropertyValue(Property->GetKeyProp(), KeyStorage.GetContainer(), 0, KeySnapshot, &Error)
				|| !CapturePropertyValue(Property->GetValueProp(), ValueStorage.GetContainer(), 0, ValueSnapshot, &Error))
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
				FReflectedValueStorage DraftKeyStorage;
				FReflectedValueStorage DraftValueStorage;
				std::string Error;
				if (!DraftKeyStorage.DefaultConstruct(Property->GetKeyProp(), 0, &Error)
					|| !DraftValueStorage.DefaultConstruct(Property->GetValueProp(), 0, &Error)
					|| !RestorePropertyValue(Property->GetKeyProp(), DraftKeyStorage.GetContainer(), 0, MapInsertDraft.Key, &Error)
					|| !RestorePropertyValue(Property->GetValueProp(), DraftValueStorage.GetContainer(), 0, MapInsertDraft.Value, &Error))
				{
					ReportError(Context, Error.empty() ? "Unable to restore the map-entry draft." : std::move(Error));
					MapInsertDraft = {};
				}
				else
				{
					void* DraftKey = DraftKeyStorage.GetValue();
					void* DraftValue = DraftValueStorage.GetValue();
					const FPropertyWidgetEditResult KeyEdit = EditPropertyWidget(
						Context, Property->GetKeyProp(), DraftKeyStorage.GetContainer(), 0, "New Key", bReadOnly);
					if (KeyEdit.bChanged && KeyEdit.AssignValue)
					{
						KeyEdit.AssignValue(Property->GetKeyProp(), DraftKey, 0);
						if (!CapturePropertyValue(Property->GetKeyProp(), DraftKey, 0, MapInsertDraft.Key, &Error))
							ReportError(Context, std::move(Error));
					}
					const FPropertyWidgetEditResult ValueEdit = EditPropertyWidget(
						Context, Property->GetValueProp(), DraftValueStorage.GetContainer(), 0, "New Value", bReadOnly);
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
						const void* ExistingValue = nullptr;
						if (Property->FindValue(Container, DraftKey, &ExistingValue, ArrayIndex)
							== EContainerOpResult::Success)
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
								[&](const FResolvedPropertyValue& DraftMap, FPropertyValueDraft&, std::string& MutationError) {
									auto* DraftProperty = static_cast<const FMapProperty*>(DraftMap.Property);
									const EContainerOpResult Result = DraftProperty->InsertChecked(
										DraftMap.Container, DraftKey, DraftValue, DraftMap.ArrayIndex);
									if (Result != EContainerOpResult::Success)
										MutationError = std::format("Unable to insert reflected map entry (result {}).", static_cast<uint32>(Result));
								});
							if (bChanged) MapInsertDraft = {};
						}
					}
				}
				ImGui::PopID();
				if (bChanged)
				{
					ImGui::TreePop();
					return true;
				}
			}

			std::vector<FEditableMapEntry> Entries;
			Entries.reserve(static_cast<size_t>(Num));
			if (Property->VisitMutableEntries(Container, &CollectEditableMapEntry, &Entries, ArrayIndex)
				!= EContainerOpResult::Success)
			{
				ReportError(Context, "Unable to traverse reflected map entries.");
				ImGui::TreePop();
				return false;
			}
			for (uint64 Index = 0; Index < Entries.size(); ++Index)
			{
				const void* Key = Entries[static_cast<size_t>(Index)].Key;
				void* Value = Entries[static_cast<size_t>(Index)].Value;
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
					Property->GetKeyProp(), KeySnapshot, SerializedKey);
				KeyTarget.Kind = EPropertyChangeKind::MapKeyRename;
				const FPropertyWidgetEditResult KeyEdit = EditPropertyWidget(
					Context, Property->GetKeyProp(), const_cast<void*>(Key), 0, std::format("[{}] Key", Index), bReadOnly);
				const bool bKeyChanged = KeyEdit.bChanged;
				const FReflectedPropertyEditTarget ValueTarget = EditTarget.ForMapEntry(Property->GetValueProp(), KeySnapshot, SerializedKey);
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
						[&](const FResolvedPropertyValue& DraftMap, FPropertyValueDraft&, std::string& MutationError) {
							auto* DraftProperty = static_cast<const FMapProperty*>(DraftMap.Property);
							const EContainerOpResult Result = DraftProperty->RemoveChecked(
								DraftMap.Container, Key, DraftMap.ArrayIndex);
							if (Result != EContainerOpResult::Success)
								MutationError = std::format("Unable to remove reflected map entry (result {}).", static_cast<uint32>(Result));
						});
				}
				else if (bKeyChanged)
				{
					FReflectedValueStorage ProposedKeyStorage;
					std::string ProposedKeyError;
					if (!ProposedKeyStorage.DefaultConstruct(Property->GetKeyProp(), 0, &ProposedKeyError)
						|| !KeyEdit.AssignValue)
					{
						ReportError(Context, ProposedKeyError.empty()
							? "Unable to materialize the proposed map key."
							: std::move(ProposedKeyError));
					}
					else
					{
						void* ProposedKey = ProposedKeyStorage.GetValue();
						KeyEdit.AssignValue(Property->GetKeyProp(), ProposedKeyStorage.GetContainer(), 0);
						FPropertyValueSnapshot ProposedKeySnapshot;
						std::string CaptureError;
						const void* ExistingValue = nullptr;
						if (!CapturePropertyValue(Property->GetKeyProp(), ProposedKey, 0, ProposedKeySnapshot, &CaptureError))
						{
							ReportError(Context, std::move(CaptureError));
						}
						else if (!(ProposedKeySnapshot == KeySnapshot)
							&& Property->FindValue(Container, ProposedKey, &ExistingValue, ArrayIndex)
								== EContainerOpResult::Success)
						{
							ReportError(Context, "Map keys must be unique.");
						}
						else
						{
							bChanged |= SubmitStructure(KeyTarget, EPropertyChangeKind::MapKeyRename,
								[&](const FResolvedPropertyValue&, FPropertyValueDraft& Draft, std::string& MutationError) {
									const FProperty* DraftProperty = nullptr;
									void* DraftContainer = nullptr;
									uint32 DraftArrayIndex = 0;
									if (!Draft.Resolve(EditTarget, DraftProperty, DraftContainer, DraftArrayIndex, &MutationError)) return;
									auto* DraftMap = static_cast<const FMapProperty*>(DraftProperty);
									const EContainerOpResult Result = DraftMap->RenameKeyChecked(
										DraftContainer, Key, ProposedKey, DraftArrayIndex);
									if (Result != EContainerOpResult::Success)
										MutationError = std::format("Unable to rename the reflected map key (result {}).", static_cast<uint32>(Result));
								}, true);
						}
					}
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
			if (!EditSession.Begin(Target, {}, &Error, Context.Transactions))
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
			[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft*) {
				AssignValue(const_cast<FProperty*>(DraftValue.Property), DraftValue.Container, DraftValue.ArrayIndex);
			}, Proposed, &Error))
		{
			ReportError(Context, std::move(Error));
			return false;
		}
		const bool bSubmitted = SubmitPropertyEdit(Context, Target, Proposed, bContinuous);
		return bSubmitted;
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
