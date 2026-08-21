#include "Editor/PropertyView.h"
#include "Editor/PropertyValueDraft.h"

#include "AssetAuthoring.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "DObject/Package.h"
#include "Editor/AssetPicker.h"
#include "Icons/FontAwesomeIcons.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"

namespace Durin::Editor
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

		auto ReflectedClassDisplayName(const DClass* Class) -> std::string
		{
			if (!Class) return "Object";
			return Class->GetDisplayName().empty() ? Class->GetShortName() : Class->GetDisplayName();
		}

		auto ReflectedPropertyTypeName(const FProperty& Property) -> std::string
		{
			switch (Property.GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool: return "Boolean";
			case DurinCodeGen::EPropertyGenFlags::Int8: return "Int8";
			case DurinCodeGen::EPropertyGenFlags::Int16: return "Int16";
			case DurinCodeGen::EPropertyGenFlags::Int32: return "Int32";
			case DurinCodeGen::EPropertyGenFlags::Int64: return "Int64";
			case DurinCodeGen::EPropertyGenFlags::UInt8: return "UInt8";
			case DurinCodeGen::EPropertyGenFlags::UInt16: return "UInt16";
			case DurinCodeGen::EPropertyGenFlags::UInt32: return "UInt32";
			case DurinCodeGen::EPropertyGenFlags::UInt64: return "UInt64";
			case DurinCodeGen::EPropertyGenFlags::Float: return "Float";
			case DurinCodeGen::EPropertyGenFlags::Double: return "Double";
			case DurinCodeGen::EPropertyGenFlags::String: return "String";
			case DurinCodeGen::EPropertyGenFlags::Name: return "Name";
			case DurinCodeGen::EPropertyGenFlags::Guid: return "GUID";
			case DurinCodeGen::EPropertyGenFlags::Enum:
			{
				const DEnum* Enum = static_cast<const FEnumProperty&>(Property).GetEnum();
				if (!Enum) return "Enum";
				const std::string_view DisplayName = Enum->GetDisplayName();
				return std::format("Enum ({})", DisplayName.empty() ? Enum->GetShortName().ToString() : DisplayName);
			}
			case DurinCodeGen::EPropertyGenFlags::Object:
				return std::format("Object Reference ({})", ReflectedClassDisplayName(Property.GetReferencedClass()));
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				return std::format("Soft Object Reference ({})", ReflectedClassDisplayName(Property.GetReferencedClass()));
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const DStruct* Struct = static_cast<const FStructProperty&>(Property).GetStruct();
				if (Struct == Z_Construct_DStruct_Durin_FVector2()) return "Vector2";
				if (Struct == Z_Construct_DStruct_Durin_FVector3()) return "Vector3";
				if (Struct == Z_Construct_DStruct_Durin_FVector4()) return "Vector4";
				if (Struct == Z_Construct_DStruct_Durin_FQuat()) return "Quaternion";
				if (Struct == Z_Construct_DStruct_Durin_FTransform()) return "Transform";
				if (Struct == Z_Construct_DStruct_Durin_FLinearColor()) return "Linear Color";
				return Struct ? Struct->GetShortName().ToString() : "Struct";
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				const FProperty* Inner = static_cast<const FArrayProperty&>(Property).GetInner();
				return Inner ? std::format("Array<{}>", ReflectedPropertyTypeName(*Inner)) : "Array";
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				const auto& Map = static_cast<const FMapProperty&>(Property);
				return Map.GetKeyProp() && Map.GetValueProp()
					? std::format("Map<{}, {}>", ReflectedPropertyTypeName(*Map.GetKeyProp()), ReflectedPropertyTypeName(*Map.GetValueProp()))
					: "Map";
			}
			default: return "Unsupported";
			}
		}

		auto ReflectedPropertyTypeTooltip(const FProperty& Property) -> std::string
		{
			const FPropertyMetadata& Metadata = Property.GetTypedMetadata();
			const std::string Type = std::format("Type: {}", ReflectedPropertyTypeName(Property));
			return Metadata.ToolTip.empty() ? Type : std::format("{}\n{}", Metadata.ToolTip, Type);
		}

		auto PropertyUnitLabel(EPropertyUnit Unit) -> std::string_view
		{
			switch (Unit)
			{
			case EPropertyUnit::Percent: return "%";
			case EPropertyUnit::Degrees: return "deg";
			case EPropertyUnit::Radians: return "rad";
			case EPropertyUnit::Seconds: return "s";
			case EPropertyUnit::Milliseconds: return "ms";
			case EPropertyUnit::Meters: return "m";
			case EPropertyUnit::Centimeters: return "cm";
			case EPropertyUnit::Millimeters: return "mm";
			case EPropertyUnit::Kilometers: return "km";
			default: return {};
			}
		}

		auto HasInlineStructWidget(const DStruct* Struct) -> bool
		{
			return Struct == Z_Construct_DStruct_Durin_FTransform()
				|| Struct == Z_Construct_DStruct_Durin_FVector2()
				|| Struct == Z_Construct_DStruct_Durin_FVector3()
				|| Struct == Z_Construct_DStruct_Durin_FVector4()
				|| Struct == Z_Construct_DStruct_Durin_FQuat()
				|| Struct == Z_Construct_DStruct_Durin_FLinearColor();
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
		auto CaptureProposedPropertyValue(const FPropertyEditTarget& Target,
			TWriteProposed&& WriteProposed, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) -> bool
		{
			FPropertyValueDraft Draft(Target, OutError);
			if (!Draft.IsValid()) return false;
			FResolvedPropertyValue DraftValue;
			if (!Draft.Resolve(Target, DraftValue.Property, DraftValue.Container, DraftValue.ArrayIndex, OutError)) return false;
			WriteProposed(DraftValue, &Draft);
			if (!ValidatePropertyAuthoringValue(
				Draft.GetRootProperty(), Draft.GetRootContainer(), Draft.GetRootArrayIndex(), OutError)) return false;
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

		auto MakePropertySearchText(const FProperty& Property, uint32 ArrayIndex) -> std::string
		{
			const std::string SourceName = Property.GetArrayDim() > 1
				? std::format("{}[{}]", Property.NamePrivate.ToString(), ArrayIndex)
				: Property.NamePrivate.ToString();
			const std::string DisplayName = MakePropertyLabel(Property, ArrayIndex);
			if (Property.GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
				&& static_cast<const FStructProperty&>(Property).GetStruct() == Z_Construct_DStruct_Durin_FTransform())
			{
				return std::format("{} {} Location Rotation Scale", SourceName, DisplayName);
			}
			return std::format("{} {}", SourceName, DisplayName);
		}
	}

	auto GetSoftObjectStateLabel(ESoftObjectViewState State) -> std::string_view
	{
		switch (State)
		{
		case ESoftObjectViewState::Null: return "Null";
		case ESoftObjectViewState::Unloaded: return "Unloaded";
		case ESoftObjectViewState::Loaded: return "Loaded";
		case ESoftObjectViewState::Redirected: return "Redirected";
		case ESoftObjectViewState::Missing: return "Missing";
		case ESoftObjectViewState::TypeMismatch: return "Type mismatch";
		default: return "Unknown";
		}
	}

	auto InspectSoftObject(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex
	) -> FSoftObjectViewState
	{
		FSoftObjectViewState ViewState;
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim())
		{
			ViewState.State = ESoftObjectViewState::TypeMismatch;
			ViewState.Message = "Soft object property metadata or storage is unavailable.";
			return ViewState;
		}
		FSoftObjectPtr* Reference = Property->GetSoftObjectPtr(Container, ArrayIndex);
		if (!Reference)
		{
			ViewState.State = ESoftObjectViewState::TypeMismatch;
			ViewState.Message = "Soft object property has no typed value accessor.";
			return ViewState;
		}
		if (Reference->IsNull()) return ViewState;
		ViewState.Path = Reference->GetSoftObjectPath().GetAssetPath();

		const Asset::FSoftObjectResolveResult Resolve = Asset::ResolveSoftObject(
			*Reference, Property->GetExpectedClass(), Asset::ESoftObjectNullPolicy::Reject);
		if (!Resolve)
		{
			ViewState.State = Resolve.Result.Error == Asset::EAssetError::TypeMismatch
				|| Resolve.Result.Error == Asset::EAssetError::UnknownClass
				? ESoftObjectViewState::TypeMismatch
				: ESoftObjectViewState::Missing;
			ViewState.Message = Resolve.Result.Message;
			return ViewState;
		}
		ViewState.ResolvedPath = Resolve.ResolvedPath;
		ViewState.LoadedObject = Resolve.Object;
		if (Resolve.bRedirected)
		{
			ViewState.State = ESoftObjectViewState::Redirected;
			ViewState.Message = std::format(
				"Asset {} resolves to {}.",
				ViewState.Path.ToString(), ViewState.ResolvedPath.ToString());
			return ViewState;
		}
		if (Resolve.State == Asset::ESoftObjectResolveState::Loaded)
		{
			ViewState.State = ESoftObjectViewState::Loaded;
			ViewState.LoadedObject = Resolve.Object;
			return ViewState;
		}
		ViewState.State = ESoftObjectViewState::Unloaded;
		return ViewState;
	}

	auto LoadSoftObject(
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

	auto FPropertyView::EditObject(
		const FPropertyViewContext& Context,
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
		std::stable_sort(VisibleProperties.begin(), VisibleProperties.end(), [](const auto& Left, const auto& Right) {
			return Left.Property->GetTypedMetadata().Category < Right.Property->GetTypedMetadata().Category;
		});
		std::string CurrentCategory;
		for (const FVisibleProperty& VisibleProperty : VisibleProperties)
		{
			const std::string& Category = VisibleProperty.Property->GetTypedMetadata().Category;
			if (!Category.empty() && Category != CurrentCategory)
			{
				CurrentCategory = Category;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled("%s", Category.c_str());
			}
			Result.bChanged |= EditProperty(Context, Object, VisibleProperty.Property, VisibleProperty.ArrayIndex);
		}
		if (bOwnsPropertyTable) MonaImGui::PropertyEdit::EndTable();
		return Result;
	}

	auto FPropertyView::EditProperty(
		const FPropertyViewContext& Context,
		DObject* Object,
		FProperty* Property,
		uint32 ArrayIndex,
		const FPropertyViewOptions& Options
	) -> bool
	{
		if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return false;
		std::string Label = Options.Label;
		if (Label.empty()) Label = MakePropertyLabel(*Property, ArrayIndex);
		const bool bReadOnly = Context.bReadOnly || Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		ImGui::PushID(Property);
		ImGui::PushID(static_cast<int>(ArrayIndex));
		const bool bChanged = EditPropertyValue(Context, Object, Property, Object, ArrayIndex, Label, bReadOnly, FPropertyEditTarget::ForMember(Object, Property, ArrayIndex));
		ImGui::PopID();
		ImGui::PopID();
		return bChanged;
	}

	auto FPropertyView::EditPropertyValue(
		const FPropertyViewContext& Context,
		DObject* Object,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FPropertyEditTarget& EditTarget
	) -> bool
	{
		bReadOnly |= Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct)
		{
			DStruct* Struct = static_cast<FStructProperty*>(Property)->GetStruct();
			if (!HasInlineStructWidget(Struct))
				return EditStructProperty(Context, Object, Property, Container, ArrayIndex,
					Label, bReadOnly, EditTarget);
		}
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

	auto FPropertyView::EditStructPropertyWidget(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly
	) -> FPropertyWidgetEditResult
	{
		FPropertyWidgetEditResult Result;
		DStruct* Struct = static_cast<FStructProperty*>(Property)->GetStruct();
		const std::string TypeTooltip = ReflectedPropertyTypeTooltip(*Property);
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
				return MonaImGui::PropertyEdit::EditVector(Label.c_str(), Value, bReadOnly, 0.05, &State, {}, TypeTooltip.c_str());
			});
		};

		if (Struct == Z_Construct_DStruct_Durin_FTransform())
		{
			return EditMathStruct.template operator()<FTransform>([&](FTransform& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditTransform(Label.c_str(), Value, bReadOnly, &State, TypeTooltip.c_str());
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
				return MonaImGui::PropertyEdit::EditQuat(Label.c_str(), Value, bReadOnly, &State, TypeTooltip.c_str());
			});
		}

		if (Struct == Z_Construct_DStruct_Durin_FLinearColor())
		{
			const bool bShowAlpha = Property->GetMetaData(FName("HideAlpha")) != "true";
			return EditMathStruct.template operator()<FLinearColor>([&](FLinearColor& EditedValue, auto& State) {
				return MonaImGui::PropertyEdit::EditColor(Label.c_str(), EditedValue, bShowAlpha, bReadOnly, &State, TypeTooltip.c_str());
			});
		}

		MonaImGui::PropertyEdit::BeginRow(Label.c_str(), bReadOnly, 0.0f, TypeTooltip.c_str());
		ImGui::TextDisabled("<struct>");
		MonaImGui::PropertyEdit::EndRow(bReadOnly);
		return Result;
	}

	auto FPropertyView::EditStructProperty(
		const FPropertyViewContext& Context,
		DObject* Object,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FPropertyEditTarget& EditTarget
	) -> bool
	{
		auto* StructProperty = static_cast<FStructProperty*>(Property);
		DStruct* Struct = StructProperty->GetStruct();
		const std::string TypeTooltip = ReflectedPropertyTypeTooltip(*Property);
		if (!Struct || !Struct->HasCompleteAuthoredFields())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, TypeTooltip.c_str());
			ImGui::TextDisabled("<struct metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		std::vector<FProperty*> EditableFields;
		Struct->ForEachProperty([&](FProperty* Field) {
			if (Field && Field->HasAnyPropertyFlags(EPropertyFlags::Edit)) EditableFields.push_back(Field);
		});

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth;
		if (Property->GetMetaData(FName("DefaultCollapsed")) != "true")
			Flags |= ImGuiTreeNodeFlags_DefaultOpen;
		if (EditableFields.empty()) Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		const bool bOpen = MonaImGui::CompactTreeNode("##Struct", Flags, "%s", Label.c_str());
		MonaImGui::PropertyEdit::ShowLabelTooltip(TypeTooltip.c_str(), bReadOnly);
		ImGui::TableSetColumnIndex(1);
		const std::string StructName = Struct->GetShortName().ToString();
		ImGui::TextDisabled("%s", StructName.c_str());
		if (!bOpen || EditableFields.empty()) return false;

		void* StructValue = StructProperty->GetValuePtr(Container, ArrayIndex);
		bool bChanged = false;
		for (FProperty* Field : EditableFields)
		{
			for (uint32 FieldArrayIndex = 0; FieldArrayIndex < Field->GetArrayDim(); ++FieldArrayIndex)
			{
				ImGui::PushID(Field);
				ImGui::PushID(static_cast<int>(FieldArrayIndex));
				const FPropertyEditTarget FieldTarget = EditTarget.ForStructMember(Field, FieldArrayIndex);
				bChanged = EditPropertyValue(Context, Object, Field, StructValue, FieldArrayIndex,
					MakePropertyLabel(*Field, FieldArrayIndex), bReadOnly, FieldTarget);
				ImGui::PopID();
				ImGui::PopID();
				if (bChanged) break;
			}
			if (bChanged) break;
		}
		ImGui::TreePop();
		return bChanged;
	}

	auto FPropertyView::EditPropertyWidget(
		const FPropertyViewContext& Context,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly
	) -> FPropertyWidgetEditResult
	{
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
			return EditStructPropertyWidget(Property, Container, ArrayIndex, Label, bReadOnly);

		FPropertyWidgetEditResult Result;
		const std::string TypeTooltip = ReflectedPropertyTypeTooltip(*Property);
		auto CaptureResult = [&](bool bChanged, bool bContinuous, const MonaImGui::PropertyEdit::FWidgetState& State = {}) {
			Result.bChanged = bChanged;
			Result.bContinuous = bContinuous;
			Result.bActive = State.bActive;
			Result.bDeactivatedAfterEdit = State.bDeactivatedAfterEdit;
		};

		MonaImGui::PropertyEdit::BeginRow(Label.c_str(), bReadOnly, 0.0f, TypeTooltip.c_str());

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
			std::array<uint8, sizeof(uint64)> MinimumStorage{};
			std::array<uint8, sizeof(uint64)> MaximumStorage{};
			check(Property->GetElementSize() <= Value.size());
			std::memcpy(Value.data(), Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
			const FPropertyMetadata& Metadata = Property->GetTypedMetadata();
			float Speed = Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f;
			if (Metadata.Step.Kind == EPropertyMetadataNumericKind::Signed) Speed = static_cast<float>(Metadata.Step.Signed);
			else if (Metadata.Step.Kind == EPropertyMetadataNumericKind::Unsigned) Speed = static_cast<float>(Metadata.Step.Unsigned);
			else if (Metadata.Step.Kind == EPropertyMetadataNumericKind::Float) Speed = Metadata.Step.Float;
			else if (Metadata.Step.Kind == EPropertyMetadataNumericKind::Double) Speed = static_cast<float>(Metadata.Step.Double);
			auto StoreLimit = [&](const FPropertyMetadataNumber& Number, auto& Storage) -> const void* {
				if (Number.Kind == EPropertyMetadataNumericKind::None) return nullptr;
				auto Store = [&](auto TypedValue) -> const void* {
					std::memcpy(Storage.data(), &TypedValue, sizeof(TypedValue));
					return Storage.data();
				};
				switch (Kind)
				{
				case DurinCodeGen::EPropertyGenFlags::Int8: return Store(static_cast<int8>(Number.Signed));
				case DurinCodeGen::EPropertyGenFlags::Int16: return Store(static_cast<int16>(Number.Signed));
				case DurinCodeGen::EPropertyGenFlags::Int32: return Store(static_cast<int32>(Number.Signed));
				case DurinCodeGen::EPropertyGenFlags::Int64: return Store(Number.Signed);
				case DurinCodeGen::EPropertyGenFlags::UInt8: return Store(static_cast<uint8>(Number.Unsigned));
				case DurinCodeGen::EPropertyGenFlags::UInt16: return Store(static_cast<uint16>(Number.Unsigned));
				case DurinCodeGen::EPropertyGenFlags::UInt32: return Store(static_cast<uint32>(Number.Unsigned));
				case DurinCodeGen::EPropertyGenFlags::UInt64: return Store(Number.Unsigned);
				case DurinCodeGen::EPropertyGenFlags::Float: return Store(Number.Float);
				case DurinCodeGen::EPropertyGenFlags::Double: return Store(Number.Double);
				default: return nullptr;
				}
			};
			const void* Minimum = StoreLimit(Metadata.UIMin, MinimumStorage);
			const void* Maximum = StoreLimit(Metadata.UIMax, MaximumStorage);
			std::string Format;
			if (Metadata.Precision >= 0 && (Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double))
				Format = std::format("%.{}f", Metadata.Precision);
			const bool bChanged = ImGui::DragScalar("##Value", DataType, Value.data(), Speed, Minimum, Maximum,
				Format.empty() ? nullptr : Format.c_str());
			const MonaImGui::PropertyEdit::FWidgetState State{
				ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()
			};
			const std::string_view Unit = PropertyUnitLabel(Metadata.Units);
			if (!Unit.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%.*s", static_cast<int>(Unit.size()), Unit.data());
			}
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
			FAssetPath CurrentAssetPath;
			const bool bHasCurrentAsset = Current && Current->GetPackage()
				&& FAssetPath::TryCreate(Current->GetPackage()->GetPackagePath(), CurrentAssetPath)
				&& Asset::FindAssetExact(CurrentAssetPath);
			const FAssetPickerAction RevealAction{
				.Icon = Icons::Crosshairs,
				.ButtonId = "RevealObject",
				.Tooltip = "Reveal the selected asset in the Content Browser.",
				.bEnabled = bHasCurrentAsset && static_cast<bool>(Context.RevealAsset),
				.Execute = [&Context, &CurrentAssetPath](std::string& Error) {
					return Context.RevealAsset && Context.RevealAsset(CurrentAssetPath, Error);
				},
			};
			const FAssetPickerResult PickerResult = AssetPicker::Draw({
				.ComboId = "##Value",
				.SearchId = "##ObjectSearch",
				.SearchHint = "Search assets...",
				.RequiredClass = RequiredClass,
				.ClassPolicy = EAssetClassPolicy::Derived,
				.CurrentSelection = Current,
				.SearchText = AssetSearchText,
				.bAllowNone = true,
				.AssignSelection = [&](DObject* Selection, std::string& OutError) {
					if (Selection == Current) { SelectedObject = nullptr; return true; }
					if (RequiredClass && Selection && !AssetPicker::MatchesClass(Selection->GetClass(), RequiredClass, EAssetClassPolicy::Derived))
					{
						OutError = "The selected asset does not match the required class.";
						return false;
					}
					SelectedObject = Selection;
					return true;
				},
				.TrailingAction = RevealAction,
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
			const FSoftObjectViewState ViewState = InspectSoftObject(SoftProperty, Container, ArrayIndex);
			FSoftObjectPath SelectedPath = CurrentReference ? CurrentReference->GetSoftObjectPath() : FSoftObjectPath();
			const std::string_view StateLabel = GetSoftObjectStateLabel(ViewState.State);
			const bool bCanLoad = ViewState.State == ESoftObjectViewState::Unloaded
				|| (ViewState.State == ESoftObjectViewState::Redirected
					&& !ViewState.LoadedObject);
			const bool bCanReveal = ViewState.Path.IsValid()
				&& Asset::FindAssetExact(ViewState.Path) && Context.RevealAsset;

			const FAssetPickerAction LoadAction{
				.Icon = Icons::Play,
				.ButtonId = "LoadSoftObject",
				.Tooltip = bCanLoad ? "Load the referenced asset." : "The referenced asset is not loadable in its current state.",
				.bEnabled = bCanLoad,
				.Execute = [SoftProperty, Container, ArrayIndex](std::string& Error) {
					DObject* LoadedObject = nullptr;
					if (!LoadSoftObject(SoftProperty, Container, ArrayIndex, LoadedObject, &Error)) return false;
					return LoadedObject != nullptr;
				},
			};
			const std::array<FAssetPickerAction, 1> AdditionalActions{{
				{
					.Icon = Icons::Crosshairs,
					.ButtonId = "RevealSoftObject",
					.Tooltip = "Reveal the referenced asset in the Content Browser.",
					.bEnabled = bCanReveal,
					.Execute = [&Context, &ViewState](std::string& Error) {
						return Context.RevealAsset && Context.RevealAsset(ViewState.Path, Error);
					},
				},
			}};
			const FAssetPickerResult PickerResult = AssetPicker::Draw({
				.ComboId = "##Value",
				.SearchId = "##SoftObjectSearch",
				.SearchHint = "Search assets...",
				.RequiredClass = SoftProperty->GetExpectedClass(),
				.ClassPolicy = EAssetClassPolicy::Derived,
				.AssignmentMode = EAssetAssignmentMode::AssetPath,
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

	auto FPropertyView::SubmitWidgetEdit(
		const FPropertyViewContext& Context,
		const FPropertyEditTarget& EditTarget,
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

	auto FPropertyView::EditArrayProperty(
		const FPropertyViewContext& Context,
		DObject* Object,
		FArrayProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FPropertyEditTarget& EditTarget
	) -> bool
	{
		const std::string TypeTooltip = ReflectedPropertyTypeTooltip(*Property);
		if (!Property->HasArrayOps() || !Property->GetInner())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, TypeTooltip.c_str());
			ImGui::TextDisabled("<array metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		uint64 Num = 0;
		if (!Property->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess)
			|| Property->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, TypeTooltip.c_str());
			ImGui::TextDisabled("<array Count/RandomAccess unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = MonaImGui::CompactTreeNode("##Array", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%llu)", Label.c_str(), Num);
		MonaImGui::PropertyEdit::ShowLabelTooltip(TypeTooltip.c_str(), bReadOnly);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		bool bChanged = false;
		auto SubmitStructure = [&](EPropertyChangeKind Kind, auto&& Mutation) -> bool {
			FPropertyEditTarget StructuralTarget = EditTarget;
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
				const FPropertyEditTarget ElementTarget = EditTarget.ForArrayElement(Property->GetInner(), Index);
				const bool bElementChanged = EditPropertyValue(Context, Object, Property->GetInner(), Element, 0, std::format("[{}]", Index), bReadOnly, ElementTarget);
				bChanged |= bElementChanged;
				ImGui::PopID();
				if (bElementChanged) break;
			}
			ImGui::TreePop();
		}
		return bChanged;
	}

	auto FPropertyView::EditMapProperty(
		const FPropertyViewContext& Context,
		DObject* Object,
		FMapProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FPropertyEditTarget& EditTarget
	) -> bool
	{
		const std::string TypeTooltip = ReflectedPropertyTypeTooltip(*Property);
		if (!Property->HasMapOps() || !Property->GetKeyProp() || !Property->GetValueProp())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, TypeTooltip.c_str());
			ImGui::TextDisabled("<map metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		uint64 Num = 0;
		if (!Property->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::MutableMappedTraversal)
			|| Property->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, TypeTooltip.c_str());
			ImGui::TextDisabled("<map Count/MutableMappedTraversal unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = MonaImGui::CompactTreeNode("##Map", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%llu)", Label.c_str(), Num);
		MonaImGui::PropertyEdit::ShowLabelTooltip(TypeTooltip.c_str(), bReadOnly);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		bool bChanged = false;
		auto SubmitStructure = [&](FPropertyEditTarget StructuralTarget, EPropertyChangeKind Kind, auto&& Mutation, bool bContinuous = false) -> bool {
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
			if (MapInsertDraft.bActive && MapInsertDraft.Target.IsSameStableTarget(EditTarget))
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
							FPropertyEditTarget InsertTarget = EditTarget;
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

				FPropertyEditTarget KeyTarget = EditTarget.ForMapEntry(
					Property->GetKeyProp(), KeySnapshot, SerializedKey);
				KeyTarget.Kind = EPropertyChangeKind::MapKeyRename;
				const FPropertyWidgetEditResult KeyEdit = EditPropertyWidget(
					Context, Property->GetKeyProp(), const_cast<void*>(Key), 0, std::format("[{}] Key", Index), bReadOnly);
				const bool bKeyChanged = KeyEdit.bChanged;
				const FPropertyEditTarget ValueTarget = EditTarget.ForMapEntry(Property->GetValueProp(), KeySnapshot, SerializedKey);
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
					FPropertyEditTarget RemoveTarget = EditTarget;
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

	auto FPropertyView::SubmitPropertyEdit(
		const FPropertyViewContext& Context,
		const FPropertyEditTarget& Target,
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

		const EPropertyEditResult Result = EditSession.Apply(ProposedValue, &Error);
		if (Result == EPropertyEditResult::Failed)
		{
			ReportError(Context, std::move(Error));
			FinishActiveEdit(&Context, true);
			return false;
		}
		if (Result == EPropertyEditResult::Pending) return true;
		if (!bContinuous) FinishActiveEdit(&Context, false);
		return Result == EPropertyEditResult::Changed;
	}

	auto FPropertyView::SubmitPropertyValueEdit(
		const FPropertyViewContext& Context,
		const FPropertyEditTarget& Target,
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

	auto FPropertyView::FinishActiveEdit(const FPropertyViewContext* Context, bool bCancel) -> bool
	{
		if (!EditSession.IsActive()) return true;
		std::string Error;
		const EPropertyEditResult Result = bCancel ? EditSession.Cancel(&Error) : EditSession.Commit(&Error);
		if (Result == EPropertyEditResult::Failed && Context) ReportError(*Context, std::move(Error));
		if (!EditSession.IsActive())
		{
			ActiveEditObject = nullptr;
			ActiveEditOwnerObject = nullptr;
		}
		return Result != EPropertyEditResult::Failed;
	}


	auto FPropertyView::HandleOwnerContext(const FPropertyViewContext& Context, DObject* Object) -> bool
	{
		if (EditSession.IsActive() && (ActiveEditOwnerObject != Object || Context.bReadOnly))
		{
			if (!FinishActiveEdit(&Context, true)) return false;
		}
		if (OwnerContextObject != Object || Context.bReadOnly) MapInsertDraft = {};
		OwnerContextObject = Object;
		return true;
	}

	auto FPropertyView::ReportError(const FPropertyViewContext& Context, std::string Error) const -> void
	{
		if (Context.ReportError) Context.ReportError(std::move(Error));
	}

	auto MakePropertyDisplayName(std::string_view PropertyName, DurinCodeGen::EPropertyGenFlags Kind,
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

	auto MakePropertyLabel(const FProperty& Property, uint32 ArrayIndex) -> std::string
	{
		const FPropertyMetadata& Metadata = Property.GetTypedMetadata();
		static const FName DisplayNameMetaDataKey("DisplayName");
		std::string Label = MakePropertyDisplayName(
			Property.NamePrivate.ToString(),
			Property.GetKind(),
			Metadata.DisplayName.empty() ? Property.GetMetaData(DisplayNameMetaDataKey) : Metadata.DisplayName
		);
		if (Property.GetArrayDim() > 1) Label = std::format("{}[{}]", Label, ArrayIndex);
		return Label;
	}
}
