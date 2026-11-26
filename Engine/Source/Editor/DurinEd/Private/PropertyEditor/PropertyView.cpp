#include "PropertyEditor/PropertyView.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "Profiling/Profiling.h"
#include "PropertyEditor/PropertyValueDraft.h"

#include "Asset/Asset.h"
#include "Asset/RegistryOperations.h"
#include "Asset/EditorBulkData.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/MathStructs.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Asset/AssetPicker.h"
#include "Icons/FontAwesomeIcons.h"
#include "Logging/LogMacros.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "Notifications/EditorNotificationSubsystem.h"
#include "Widgets/MonaImGuiPropertyTable.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin
{
	namespace
	{
		auto AcceptPropertyResult(const std::expected<void, FPropertyValueError>& Result, std::string& OutError) -> bool
		{
			if (Result) return true;
			OutError = ToString(Result.error());
			return false;
		}

		auto AcceptPropertyResult(const std::expected<void, FPropertySnapshotError>& Result, std::string& OutError) -> bool
		{
			if (Result) return true;
			OutError = ToString(Result.error());
			return false;
		}

		// ArrayIndexEnum maps numeric enum values to storage indices; unknown metadata
		// falls back to ordinary array editing so a typo cannot hide authored data.
		auto GetArrayIndexEnum(const FProperty& Property) -> DEnum*
		{
			if (Property.GetArrayDim() <= 1) return nullptr;
			const std::string_view EnumName = Property.GetMetaData(FName("ArrayIndexEnum"));
			return EnumName.empty() ? nullptr : FindEnumByQualifiedName(FName(EnumName));
		}

		auto IsVisibleArrayIndex(const FProperty& Property, uint32 ArrayIndex) -> bool
		{
			const DEnum* Enum = GetArrayIndexEnum(Property);
			return !Enum || Enum->FindValueRecordByValue(ArrayIndex) != nullptr;
		}

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
			case DurinCodeGen::EPropertyGenFlags::Byte: return "byte";
			case DurinCodeGen::EPropertyGenFlags::Blob: return "blob";
			case DurinCodeGen::EPropertyGenFlags::BulkData: return "authored bulk data";
			case DurinCodeGen::EPropertyGenFlags::Enum: return "enum";
			case DurinCodeGen::EPropertyGenFlags::Object: return "object";
			case DurinCodeGen::EPropertyGenFlags::SoftObject: return "soft object";
			case DurinCodeGen::EPropertyGenFlags::WeakObject: return "weak object";
			case DurinCodeGen::EPropertyGenFlags::Struct: return "struct";
			case DurinCodeGen::EPropertyGenFlags::Array: return "array";
			case DurinCodeGen::EPropertyGenFlags::Map: return "map";
			default: return "unsupported";
			}
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

		auto PropertyDragSpeed(const FPropertyMetadata& Metadata, double DefaultSpeed) -> double
		{
			switch (Metadata.Step.Kind)
			{
			case EPropertyMetadataNumericKind::Signed: return static_cast<double>(Metadata.Step.Signed);
			case EPropertyMetadataNumericKind::Unsigned: return static_cast<double>(Metadata.Step.Unsigned);
			case EPropertyMetadataNumericKind::Float: return static_cast<double>(Metadata.Step.Float);
			case EPropertyMetadataNumericKind::Double: return Metadata.Step.Double;
			default: return DefaultSpeed;
			}
		}

		auto PropertyDecimalFormat(const FPropertyMetadata& Metadata, int8 DefaultPrecision = -1) -> const char*
		{
			static constexpr std::array<const char*, 128> Formats{
				"%.0f", "%.1f", "%.2f", "%.3f", "%.4f", "%.5f", "%.6f", "%.7f",
				"%.8f", "%.9f", "%.10f", "%.11f", "%.12f", "%.13f", "%.14f", "%.15f",
				"%.16f", "%.17f", "%.18f", "%.19f", "%.20f", "%.21f", "%.22f", "%.23f",
				"%.24f", "%.25f", "%.26f", "%.27f", "%.28f", "%.29f", "%.30f", "%.31f",
				"%.32f", "%.33f", "%.34f", "%.35f", "%.36f", "%.37f", "%.38f", "%.39f",
				"%.40f", "%.41f", "%.42f", "%.43f", "%.44f", "%.45f", "%.46f", "%.47f",
				"%.48f", "%.49f", "%.50f", "%.51f", "%.52f", "%.53f", "%.54f", "%.55f",
				"%.56f", "%.57f", "%.58f", "%.59f", "%.60f", "%.61f", "%.62f", "%.63f",
				"%.64f", "%.65f", "%.66f", "%.67f", "%.68f", "%.69f", "%.70f", "%.71f",
				"%.72f", "%.73f", "%.74f", "%.75f", "%.76f", "%.77f", "%.78f", "%.79f",
				"%.80f", "%.81f", "%.82f", "%.83f", "%.84f", "%.85f", "%.86f", "%.87f",
				"%.88f", "%.89f", "%.90f", "%.91f", "%.92f", "%.93f", "%.94f", "%.95f",
				"%.96f", "%.97f", "%.98f", "%.99f", "%.100f", "%.101f", "%.102f", "%.103f",
				"%.104f", "%.105f", "%.106f", "%.107f", "%.108f", "%.109f", "%.110f", "%.111f",
				"%.112f", "%.113f", "%.114f", "%.115f", "%.116f", "%.117f", "%.118f", "%.119f",
				"%.120f", "%.121f", "%.122f", "%.123f", "%.124f", "%.125f", "%.126f", "%.127f",
			};
			const int8 Precision = Metadata.Precision >= 0 ? Metadata.Precision : DefaultPrecision;
			return Precision >= 0 ? Formats[static_cast<uint8>(Precision)] : nullptr;
		}

		auto HasInlineStructWidget(const DStruct* Struct) -> bool
		{
			return Struct == Z_Construct_DStruct_FTransform()
				|| Struct == Z_Construct_DStruct_FVector2f()
				|| Struct == Z_Construct_DStruct_FVector3f()
				|| Struct == Z_Construct_DStruct_FVector4f()
				|| Struct == Z_Construct_DStruct_FVector2()
				|| Struct == Z_Construct_DStruct_FVector3()
				|| Struct == Z_Construct_DStruct_FVector4()
				|| Struct == Z_Construct_DStruct_FQuat()
				|| Struct == Z_Construct_DStruct_FLinearColor();
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

		// Proposal failures end at the view boundary; callers only display the message.
		auto CheckMapMutation(EContainerOpResult Result, std::string_view Operation)
			-> std::expected<void, std::string>
		{
			if (Result == EContainerOpResult::Success) return {};
			return std::unexpected(std::format("Reflected map {} failed (result {}).",
				Operation, static_cast<uint32>(Result)));
		}

		template<typename TWriteProposed>
		auto CaptureProposedPropertyValue(const FPropertyEditTarget& Target,
			TWriteProposed&& WriteProposed, FPropertyValueSnapshot& OutSnapshot) -> std::expected<void, std::string>
		{
			FPropertyValueDraft Draft(Target);
			if (!Draft.IsValid()) return std::unexpected(FormatPropertyValueDraftError(Draft.GetError()));
			FResolvedPropertyValue DraftValue;
			if (auto Result = Draft.Resolve(Target, DraftValue.Property, DraftValue.Container, DraftValue.ArrayIndex); !Result)
				return std::unexpected(FormatPropertyValueDraftError(Result.error()));
			if (auto Written = WriteProposed(DraftValue, &Draft); !Written) return Written;
			auto Validation = ValidatePropertyEditValue(
				Draft.GetRootProperty(), Draft.GetRootContainer(), Draft.GetRootArrayIndex());
			if (!Validation) return std::unexpected(ToString(Validation.error()));
			auto Result = Draft.Capture(OutSnapshot);
			if (!Result) return std::unexpected(FormatPropertyValueDraftError(Result.error()));
			return {};
		}

		auto DrawNumericProperty(const FProperty& Property, ImGuiDataType DataType, void* Value,
			MonaImGui::PropertyEdit::FWidgetState& OutState) -> bool
		{
			const auto Kind = Property.GetKind();
			alignas(uint64) std::array<std::byte, sizeof(uint64)> MinimumStorage{};
			alignas(uint64) std::array<std::byte, sizeof(uint64)> MaximumStorage{};
			const FPropertyMetadata& Metadata = Property.GetTypedMetadata();
			const float Speed = static_cast<float>(PropertyDragSpeed(Metadata,
				Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05 : 1.0));
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
			const void* Minimum = StoreLimit(Metadata.UIMin.Kind != EPropertyMetadataNumericKind::None
				? Metadata.UIMin : Metadata.ClampMin, MinimumStorage);
			const void* Maximum = StoreLimit(Metadata.UIMax.Kind != EPropertyMetadataNumericKind::None
				? Metadata.UIMax : Metadata.ClampMax, MaximumStorage);
			const char* Format = Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double
				? PropertyDecimalFormat(Metadata, 3) : nullptr;
			const bool bChanged = ImGui::DragScalar("##Value", DataType, Value, Speed, Minimum, Maximum,
				Format, ImGuiSliderFlags_ClampZeroRange);
			OutState = {
				ImGui::IsItemActive(), ImGui::IsItemDeactivatedAfterEdit()
			};
			if (bChanged)
			{
				// UI bounds guide dragging; hard bounds also apply to manual input and one-sided ranges.
				Minimum = StoreLimit(Metadata.ClampMin, MinimumStorage);
				Maximum = StoreLimit(Metadata.ClampMax, MaximumStorage);
				ImGui::DataTypeClamp(DataType, Value, Minimum, Maximum);
			}
			const std::string_view Unit = PropertyUnitLabel(Metadata.Units);
			if (!Unit.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%.*s", static_cast<int>(Unit.size()), Unit.data());
			}
			return bChanged;
		}

		auto CaptureMapPathKey(const FProperty* KeyProperty, const void* Key) -> FByteBuffer
		{
			FPropertyValueSnapshot Snapshot;
			if (!CapturePropertyValue(KeyProperty, Key, 0, Snapshot)) return {};
			FByteBuffer Result = Snapshot.GetBytes();
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

	auto InspectWeakObject(FWeakObjectProperty* Property, void* Container, uint32 ArrayIndex) -> FWeakObjectViewState
	{
		if (!Property) return {.State = EWeakObjectViewState::TypeMismatch};
		FWeakObjectPtr* Reference = Property->GetWeakObjectPtr(Container, ArrayIndex);
		if (!Reference) return {.State = EWeakObjectViewState::TypeMismatch};
		DObject* Object = Reference->Get();
		if (Object) return {.State = EWeakObjectViewState::Live, .Object = Object};
		return {.State = IsObjectKeyNull(Reference->GetKey())
			? EWeakObjectViewState::Null : EWeakObjectViewState::Expired};
	}

	auto GetWeakObjectStateLabel(EWeakObjectViewState State) -> std::string_view
	{
		switch (State)
		{
		case EWeakObjectViewState::Null: return "Null";
		case EWeakObjectViewState::Live: return "Live";
		case EWeakObjectViewState::Expired: return "Expired";
		case EWeakObjectViewState::TypeMismatch: return "Type mismatch";
		}
		return "Unknown";
	}

	auto InspectSoftObject(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex
	) -> FSoftObjectViewState
	{
		FSoftObjectViewState ViewState{.PropertyName = Property ? Property->NamePrivate.ToString() : std::string{},
			.ArrayIndex = ArrayIndex, .ArrayDim = Property ? Property->GetArrayDim() : 0};
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim())
		{
			ViewState.State = ESoftObjectViewState::TypeMismatch;
			ViewState.Error = ESoftObjectViewError::Storage;
			return ViewState;
		}
		FSoftObjectPtr* Reference = Property->GetSoftObjectPtr(Container, ArrayIndex);
		if (!Reference)
		{
			ViewState.State = ESoftObjectViewState::TypeMismatch;
			ViewState.Error = ESoftObjectViewError::Accessor;
			return ViewState;
		}
		if (Reference->IsNull()) return ViewState;
		ViewState.Path = Reference->ToSoftObjectPath();

		const auto Resolution = ResolveAssetObjectPathForOperation(
			Reference->ToSoftObjectPath(), {.ExpectedClass = Property->GetExpectedClass()});
		ViewState.LoadedObject = Reference->Get(Property->GetExpectedClass());
		if (!Resolution)
		{
			if (Resolution.State == EAssetPathResolveState::NotFound && ViewState.LoadedObject)
			{
				ViewState.State = ESoftObjectViewState::Loaded;
				ViewState.ResolvedPath = ViewState.Path;
				return ViewState;
			}
			const bool bTypeMismatch = Resolution.State == EAssetPathResolveState::RedirectTypeMismatch
				|| Resolution.State == EAssetPathResolveState::UnknownTargetClass;
			ViewState.State = bTypeMismatch ? ESoftObjectViewState::TypeMismatch : ESoftObjectViewState::Missing;
			ViewState.Error = ESoftObjectViewError::Asset;
			ViewState.AssetDiagnostic = std::format("Soft object path '{}' could not be resolved (state {}).",
					ViewState.Path.ToString(), static_cast<uint32>(Resolution.State));
			return ViewState;
		}
		ViewState.ResolvedPath = Resolution.FinalPath;
		if (!Resolution.RedirectChain.empty())
		{
			ViewState.State = ESoftObjectViewState::Redirected;
			return ViewState;
		}
		if (ViewState.LoadedObject)
		{
			ViewState.State = ESoftObjectViewState::Loaded;
			return ViewState;
		}

		ViewState.State = ESoftObjectViewState::Unloaded;
		return ViewState;
	}

	auto FormatPropertySoftLoadResult(const FPropertySoftLoadResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case EPropertySoftLoadError::None: return {};
		case EPropertySoftLoadError::Storage: return "Soft object property metadata or storage is unavailable.";
		case EPropertySoftLoadError::Accessor: return "Soft object property has no typed value accessor.";
		case EPropertySoftLoadError::Asset: return !Result.AssetDiagnostic.empty() ? Result.AssetDiagnostic : "Soft object asset loading failed.";
		case EPropertySoftLoadError::MissingObject: return "Soft object loading returned no object.";
		}
		return {};
	}

	auto LoadSoftObjectProperty(FSoftObjectProperty* Property, void* Container, uint32 ArrayIndex,
		DObject*& OutObject) -> FPropertySoftLoadResult
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		OutObject = nullptr;
		FPropertySoftLoadResult Result{.PropertyName = Property ? Property->NamePrivate.ToString() : std::string{},
			.ArrayIndex = ArrayIndex, .ArrayDim = Property ? Property->GetArrayDim() : 0};
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim())
		{
			Result.Error = EPropertySoftLoadError::Storage;
			return Result;
		}
		FSoftObjectPtr* Reference = Property->GetSoftObjectPtr(Container, ArrayIndex);
		if (!Reference)
		{
			Result.Error = EPropertySoftLoadError::Accessor;
			return Result;
		}
		Result.Path = Reference->ToSoftObjectPath();
		auto Loaded = LoadObject(Reference->ToSoftObjectPath(), Property->GetExpectedClass(), nullptr, &LoadedError);
		OutObject = Loaded;
		if (!Loaded)
		{
			Result.Error = EPropertySoftLoadError::Asset;
			Result.AssetDiagnostic = LoadedError->ToString();
			return Result;
		}
		if (!OutObject) Result.Error = EPropertySoftLoadError::MissingObject;
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.ReflectedRow");
		if (!ReconcileInteraction(Context)) return false;
		std::string Label;
		bool bReadOnly = false;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.Prepare");
			if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return false;
			if (!IsVisibleArrayIndex(*Property, ArrayIndex)) return false;
			Label = Options.Label;
			if (Label.empty()) Label = MakePropertyLabel(*Property, ArrayIndex);
			bReadOnly = Context.bReadOnly || Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		}
		ImGui::PushID(Property);
		ImGui::PushID(static_cast<int>(ArrayIndex));
		const auto Kind = Property->GetKind();
		const bool bInline = Kind != DurinCodeGen::EPropertyGenFlags::Array && Kind != DurinCodeGen::EPropertyGenFlags::Map
			&& (Kind != DurinCodeGen::EPropertyGenFlags::Struct || HasInlineStructWidget(static_cast<FStructProperty*>(Property)->GetStruct()));
		bool bChanged = false;
		if (bInline)
			bChanged = DrawAndSubmitPropertyWidget(Context, Object, Property, Object, ArrayIndex, Label, bReadOnly,
				nullptr, false, Options.bShowTransformHeader);
		else
		{
			const auto EditTarget = FPropertyEditTarget::ForMember(Object, Property, ArrayIndex);
			bChanged = EditPropertyValue(Context, Object, Property, Object, ArrayIndex, Label, bReadOnly, EditTarget, Options.bShowTransformHeader);
		}
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
		const FPropertyEditTarget& EditTarget, bool bShowTransformHeader
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
			return DrawAndSubmitPropertyWidget(Context, Object, Property, Container, ArrayIndex, Label,
				bReadOnly, &EditTarget, false, bShowTransformHeader);
		}
	}

	auto FPropertyView::DrawAndSubmitPropertyWidget(const FPropertyViewContext& Context, DObject* Object,
		FProperty* Property, void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
		const FPropertyEditTarget* ExistingTarget, bool bStructMember, bool bShowTransformHeader) -> bool
	{
		using DurinCodeGen::EPropertyGenFlags;
		const auto Kind = Property->GetKind();
		const bool bColor = Kind == EPropertyGenFlags::Struct
			&& static_cast<FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_FLinearColor();
		if (!Property->HasValueAccessors() && (bColor || (!ExistingTarget && (Kind == EPropertyGenFlags::Int32 || Kind == EPropertyGenFlags::Float))))
		{
			using namespace MonaImGui::PropertyEdit;
			using FValue = PropertyEditor::FPropertyControlFrame::FValue;
			const auto Handle = ExistingTarget
				? PropertyEditor::FReflectedPropertyHandle(bStructMember ? ExistingTarget->ForStructMember(Property, ArrayIndex) : *ExistingTarget)
				: PropertyEditor::FReflectedPropertyHandle(*Object, Property->NamePrivate, ArrayIndex);
			const char* Tooltip = Property->GetTypedMetadata().ToolTip.c_str();
			bReadOnly |= Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
			if (bColor)
			{
				FLinearColor Value;
				if (Handle.GetValue(Value) == PropertyEditor::FPropertyAccess::Success)
				{
					const bool bShowAlpha = Property->GetMetaData(FName("HideAlpha")) != "true";
					FPropertyInteractionState Interaction;
					const bool bChanged = EditColor(Label.c_str(), Value, bShowAlpha, bReadOnly, nullptr, Tooltip, &Interaction);
					HandleControls.Observe(Interaction.Id, Handle, Interaction.End, bChanged ? std::optional(FValue{Value}) : std::nullopt);
					return false;
				}
			}
			else
			{
				const auto DrawNumber = [&](auto Value, ImGuiDataType Type) {
					if (Handle.GetValue(Value) != PropertyEditor::FPropertyAccess::Success) return false;
					const ImGuiID Id = ImGui::GetID(Label.c_str());
					BeginRow(Label.c_str(), bReadOnly, 0.0f, Tooltip);
					FWidgetState Widget;
					const bool bChanged = DrawNumericProperty(*Property, Type, &Value, Widget);
					EndRow(bReadOnly);
					const auto State = FPropertyInteractionState::FromWidget(Id, Widget, bReadOnly, bChanged);
					HandleControls.Observe(Id, Handle, State.End, bChanged ? std::optional(FValue{Value}) : std::nullopt);
					return true;
				};
				if (Kind == EPropertyGenFlags::Float ? DrawNumber(float{}, ImGuiDataType_Float) : DrawNumber(int32{}, ImGuiDataType_S32))
					return false;
			}
		}
		FPropertyWidgetEditResult Edit;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.Widget");
			Edit = EditPropertyWidget(Context, Property, Container, ArrayIndex, Label,
				bReadOnly || Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly), bShowTransformHeader);
		}
		const bool bNeedsTarget = (Edit.bChanged && Edit.AssignValue) || EditInteraction.Matches(Edit.Interaction);
		if (!bNeedsTarget) return false;
		if (ExistingTarget && !bStructMember) return SubmitWidgetEdit(Context, *ExistingTarget, Edit);
		FPropertyEditTarget Target;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.BuildEditTarget");
			Target = bStructMember ? ExistingTarget->ForStructMember(Property, ArrayIndex)
				: FPropertyEditTarget::ForMember(Object, Property, ArrayIndex);
		}
		return SubmitWidgetEdit(Context, Target, Edit);
	}

	auto FPropertyView::EditStructPropertyWidget(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly, bool bShowTransformHeader
	) -> FPropertyWidgetEditResult
	{
		FPropertyWidgetEditResult Result;
		DStruct* Struct = static_cast<FStructProperty*>(Property)->GetStruct();
		const char* Tooltip = Property->GetTypedMetadata().ToolTip.c_str();
		auto CaptureResult = [&](bool bChanged, bool bContinuous, const MonaImGui::PropertyEdit::FWidgetState& State = {}) {
			Result.bChanged = bChanged;
			Result.bContinuous = bContinuous;
			Result.Interaction = MonaImGui::PropertyEdit::FPropertyInteractionState::FromWidget(ImGui::GetID(Label.c_str()), State, bReadOnly, bChanged);
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
			const FPropertyMetadata& Metadata = Property->GetTypedMetadata();
			const double Speed = PropertyDragSpeed(Metadata, 0.05);
			const char* Format = PropertyDecimalFormat(Metadata);
			const MonaImGui::PropertyEdit::FValueWidgetConfig Config{
				.Format = Format,
			};
			return EditMathStruct.template operator()<TVector>([&](TVector& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditVector(
					Label.c_str(), Value, bReadOnly, Speed, &State, Config, Tooltip);
			});
		};

		if (Struct == Z_Construct_DStruct_FTransform())
		{
			return EditMathStruct.template operator()<FTransform>([&](FTransform& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditTransform(Label.c_str(), Value, bReadOnly, &State, Tooltip, bShowTransformHeader);
			});
		}

		if (Struct == Z_Construct_DStruct_FVector2f())
			return EditVector.template operator()<FVector2f>();

		if (Struct == Z_Construct_DStruct_FVector3f())
			return EditVector.template operator()<FVector3f>();

		if (Struct == Z_Construct_DStruct_FVector4f())
			return EditVector.template operator()<FVector4f>();

		if (Struct == Z_Construct_DStruct_FVector2())
			return EditVector.template operator()<FVector2>();

		if (Struct == Z_Construct_DStruct_FVector3())
			return EditVector.template operator()<FVector3>();

		if (Struct == Z_Construct_DStruct_FVector4())
			return EditVector.template operator()<FVector4>();

		if (Struct == Z_Construct_DStruct_FQuat())
		{
			return EditMathStruct.template operator()<FQuat>([&](FQuat& Value, auto& State) {
				return MonaImGui::PropertyEdit::EditQuat(Label.c_str(), Value, bReadOnly, &State, Tooltip);
			});
		}

		if (Struct == Z_Construct_DStruct_FLinearColor())
		{
			const bool bShowAlpha = Property->GetMetaData(FName("HideAlpha")) != "true";
			MonaImGui::PropertyEdit::FPropertyInteractionState Interaction;
			auto Edit = EditMathStruct.template operator()<FLinearColor>([&](FLinearColor& EditedValue, auto& State) {
				return MonaImGui::PropertyEdit::EditColor(Label.c_str(), EditedValue, bShowAlpha, bReadOnly, &State, Tooltip, &Interaction);
			});
			Edit.Interaction = Interaction;
			return Edit;
		}

		MonaImGui::PropertyEdit::BeginRow(Label.c_str(), bReadOnly, 0.0f, Tooltip);
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.Struct");
		auto* StructProperty = static_cast<FStructProperty*>(Property);
		DStruct* Struct = StructProperty->GetStruct();
		const char* Tooltip = Property->GetTypedMetadata().ToolTip.c_str();
		if (!Struct || !Struct->HasCompleteAuthoredFields())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, Tooltip);
			ImGui::TextDisabled("<struct metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		std::vector<FProperty*> EditableFields;
		Struct->ForEachProperty([&](FProperty* Field) {
			if (Field && Field->HasAnyPropertyFlags(EPropertyFlags::Edit)) EditableFields.push_back(Field);
		});

		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth;
		if (Property->GetMetaData(FName("DefaultCollapsed")) != "true")
			Flags |= ImGuiTreeNodeFlags_DefaultOpen;
		if (EditableFields.empty()) Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		const bool bOpen = MonaImGui::PropertyEdit::BeginTreeRow("##Struct", Label.c_str(), Flags);
		MonaImGui::PropertyEdit::ShowLabelTooltip(Tooltip, bReadOnly);
		ImGui::TableSetColumnIndex(1);
		ImGui::AlignTextToFramePadding();
		const std::string StructName = Struct->GetShortName().ToString();
		ImGui::TextDisabled("%s", StructName.c_str());
		if (!bOpen || EditableFields.empty()) return false;

		void* StructValue = StructProperty->GetValuePtr(Container, ArrayIndex);
		bool bChanged = false;
		for (FProperty* Field : EditableFields)
		{
			for (uint32 FieldArrayIndex = 0; FieldArrayIndex < Field->GetArrayDim(); ++FieldArrayIndex)
			{
				if (!IsVisibleArrayIndex(*Field, FieldArrayIndex)) continue;
				ImGui::PushID(Field);
				ImGui::PushID(static_cast<int>(FieldArrayIndex));
				const auto Kind = Field->GetKind();
				const bool bInline = Kind != DurinCodeGen::EPropertyGenFlags::Array && Kind != DurinCodeGen::EPropertyGenFlags::Map
					&& (Kind != DurinCodeGen::EPropertyGenFlags::Struct || HasInlineStructWidget(static_cast<FStructProperty*>(Field)->GetStruct()));
				if (bInline)
					bChanged = DrawAndSubmitPropertyWidget(Context, Object, Field, StructValue, FieldArrayIndex,
						MakePropertyLabel(*Field, FieldArrayIndex), bReadOnly, &EditTarget, true);
				else
				{
					const auto FieldTarget = EditTarget.ForStructMember(Field, FieldArrayIndex);
					bChanged = EditPropertyValue(Context, Object, Field, StructValue, FieldArrayIndex,
						MakePropertyLabel(*Field, FieldArrayIndex), bReadOnly, FieldTarget);
				}
				ImGui::PopID();
				ImGui::PopID();
				if (bChanged) break;
			}
			if (bChanged) break;
		}
		MonaImGui::PropertyEdit::EndTreeRow();
		return bChanged;
	}

	auto FPropertyView::EditPropertyWidget(
		const FPropertyViewContext& Context,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly, bool bShowTransformHeader
	) -> FPropertyWidgetEditResult
	{
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
			return EditStructPropertyWidget(Property, Container, ArrayIndex, Label, bReadOnly, bShowTransformHeader);

		FPropertyWidgetEditResult Result;
		const char* Tooltip = Property->GetTypedMetadata().ToolTip.c_str();
		auto CaptureResult = [&](bool bChanged, bool bContinuous, const MonaImGui::PropertyEdit::FWidgetState& State = {}) {
			Result.bChanged = bChanged;
			Result.bContinuous = bContinuous;
			Result.Interaction = MonaImGui::PropertyEdit::FPropertyInteractionState::FromWidget(ImGui::GetID(Label.c_str()), State, bReadOnly, bChanged);
		};

		MonaImGui::PropertyEdit::BeginRow(Label.c_str(), bReadOnly, 0.0f, Tooltip);

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
			alignas(uint64) std::array<std::byte, sizeof(uint64)> Value{};
			check(Property->GetElementSize() <= Value.size());
			std::memcpy(Value.data(), Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
			MonaImGui::PropertyEdit::FWidgetState State;
			const bool bChanged = DrawNumericProperty(*Property, DataType, Value.data(), State);
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
			CaptureResult(bChanged, true, {ImGui::IsItemActive(), ImGui::IsItemDeactivatedAfterEdit()});
			if (bChanged) Result.AssignValue = [Value = std::move(Value)](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*static_cast<FStringProperty*>(DestinationProperty)->GetStringValuePtr(DestinationContainer, DestinationArrayIndex) = Value;
			};
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Name)
		{
			auto* NameProperty = static_cast<FNameProperty*>(Property);
			std::string Value = NameProperty->GetNameValuePtr(Container, ArrayIndex)->ToString();
			const bool bChanged = MonaImGui::InputText("##Value", Value);
			CaptureResult(bChanged, true, {ImGui::IsItemActive(), ImGui::IsItemDeactivatedAfterEdit()});
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
				ImGui::IsItemActive(), ImGui::IsItemDeactivatedAfterEdit()
			};
			FGuid ParsedValue;
			const bool bChanged = bTextChanged && FGuid::Parse(Value, ParsedValue);
			CaptureResult(bChanged, true, State);
			if (bChanged) Result.AssignValue = [ParsedValue](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
				*static_cast<FGuidProperty*>(DestinationProperty)->GetGuidValuePtr(DestinationContainer, DestinationArrayIndex) = ParsedValue;
			};
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Byte)
		{
			const auto Value = *static_cast<const std::byte*>(
				Property->GetValuePtr(Container, ArrayIndex));
			ImGui::Text("0x%02X", std::to_integer<uint8>(Value));
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Blob)
		{
			const auto& Value = *static_cast<const FByteBuffer*>(
				Property->GetValuePtr(Container, ArrayIndex));
			ImGui::TextDisabled("%s", std::format("{} bytes", Value.size()).c_str());
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
		{
			const auto& Value = *static_cast<const FEditorBulkData*>(
				Property->GetValuePtr(Container, ArrayIndex));
			ImGui::TextDisabled("%s", std::format(
				"{} bytes, {}, content {}",
				Value.GetPayloadSize(),
				Value.IsMemoryResident() ? "resident" : "package-backed",
				Value.GetPayloadId().ToString()).c_str());
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
						const std::string Label = std::format("{}##EnumValue_{}", Value.DisplayName, Value.Name);
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
			FPackagePath CurrentAssetPath;
			const bool bHasCurrentAsset = Current && Current->GetPackage()
				&& FPackagePath::TryCreate(Current->GetPackage()->GetPackagePath(), CurrentAssetPath)
				&& FindPackageExact(CurrentAssetPath);
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
			if (!PickerResult.Error.empty()) ReportEditError(Context, PickerResult.Error);
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
			FSoftObjectPath SelectedPath = CurrentReference ? CurrentReference->ToSoftObjectPath() : FSoftObjectPath();
			const std::string_view StateLabel = GetSoftObjectStateLabel(ViewState.State);
			const bool bCanLoad = ViewState.State == ESoftObjectViewState::Unloaded
				|| (ViewState.State == ESoftObjectViewState::Redirected
					&& !ViewState.LoadedObject);
			const bool bCanReveal = ViewState.Path.IsValid()
				&& FindAssetExact(ViewState.Path.GetAssetPath())
				&& Context.RevealAsset;

			const FAssetPickerAction LoadAction{
				.Icon = Icons::Play,
				.ButtonId = "LoadSoftObject",
				.Tooltip = bCanLoad ? "Load the referenced asset." : "The referenced asset is not loadable in its current state.",
				.bEnabled = bCanLoad,
				.Execute = [SoftProperty, Container, ArrayIndex](std::string& Error) {
					DObject* LoadedObject = nullptr;
					if (auto Loaded = LoadSoftObjectProperty(SoftProperty, Container, ArrayIndex, LoadedObject); !Loaded)
					{
						Error = FormatPropertySoftLoadResult(Loaded);
						return false;
					}
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
						return Context.RevealAsset
							&& Context.RevealAsset(ViewState.Path.GetPackagePath(), Error);
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
				.CurrentSelectionPath = ViewState.Path.ToString(),
				.CurrentSelectionStatus = StateLabel,
				.SearchText = AssetSearchText,
				.bAllowNone = true,
				.AssignPathSelection = [&SelectedPath](std::string_view Path, std::string& Error) {
					if (Path.empty())
					{
						SelectedPath = {};
						return true;
					}
					const auto PathValidation = FSoftObjectPath::TryCreateWithDiagnostic(Path, SelectedPath);
					if (!PathValidation) Error = ToString(PathValidation.error());
					return PathValidation.has_value();
				},
				.TrailingAction = LoadAction,
				.AdditionalTrailingActions = AdditionalActions,
			});
			if (!PickerResult.Error.empty()) ReportEditError(Context, PickerResult.Error);
			CaptureResult(PickerResult.bSelectionChanged, false);
			if (PickerResult.bSelectionChanged)
			{
				Result.AssignValue = [SelectedPath](
					FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
					auto* Destination = static_cast<FSoftObjectProperty*>(DestinationProperty)
						->GetSoftObjectPtr(DestinationContainer, DestinationArrayIndex);
					if (Destination) *Destination = SelectedPath;
				};
			}
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::WeakObject)
		{
			auto* WeakProperty = static_cast<FWeakObjectProperty*>(Property);
			const FWeakObjectViewState ViewState = InspectWeakObject(WeakProperty, Container, ArrayIndex);
			const std::string ObjectName = ViewState.Object ? ViewState.Object->GetName() : std::string();
			ImGui::TextUnformatted(ObjectName.empty() ? GetWeakObjectStateLabel(ViewState.State).data() : ObjectName.c_str());
			if (!bReadOnly && ViewState.State != EWeakObjectViewState::Null)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear##WeakObject"))
				{
					CaptureResult(true, false);
					Result.AssignValue = [](FProperty* DestinationProperty, void* DestinationContainer, uint32 DestinationArrayIndex) {
						if (auto* Destination = static_cast<FWeakObjectProperty*>(DestinationProperty)
							->GetWeakObjectPtr(DestinationContainer, DestinationArrayIndex)) Destination->Reset();
					};
				}
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.SubmitEdit");
		if (Edit.bContinuous)
		{
			bool bChanged = false;
			return UpdatePropertyValueEdit(Context, EditTarget, Edit.Interaction, Edit.AssignValue, &bChanged) && bChanged;
		}
		bool bChanged = false;
		return Edit.bChanged && ApplyPropertyValueEdit(Context, EditTarget, Edit.AssignValue, false, nullptr, &bChanged) && bChanged;
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.Array");
		const char* Tooltip = Property->GetTypedMetadata().ToolTip.c_str();
		if (!Property->HasArrayOps() || !Property->GetInner())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, Tooltip);
			ImGui::TextDisabled("<array metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		uint64 Num = 0;
		if (!Property->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess)
			|| Property->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, Tooltip);
			ImGui::TextDisabled("<array Count/RandomAccess unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = MonaImGui::CompactTreeNode("##Array", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%llu)", Label.c_str(), Num);
		MonaImGui::PropertyEdit::ShowLabelTooltip(Tooltip, bReadOnly);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		bool bChanged = false;
		auto SubmitStructure = [&](EPropertyChangeType::Type ChangeType, auto&& Mutation) -> bool {
			FPropertyEditTarget StructuralTarget = EditTarget;
			StructuralTarget.ChangeType = ChangeType;
			if (!PreparePropertyWrite(Context, StructuralTarget)) return false;
			FPropertyValueSnapshot Proposed;
			if (const auto Captured = CaptureProposedPropertyValue(StructuralTarget,
				[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft*) -> std::expected<void, std::string> {
					auto Mutated = Mutation(*static_cast<const FArrayProperty*>(DraftValue.Property), DraftValue.Container, DraftValue.ArrayIndex);
					if (!Mutated) return std::unexpected(ToString(Mutated.error()));
					return {};
				}, Proposed); !Captured)
			{
				ReportEditError(Context, Captured.error());
				return false;
			}
			bool bAppliedChange = false;
			return ApplyPreparedPropertyEdit(Context, StructuralTarget, Proposed, false, &bAppliedChange) && bAppliedChange;
		};
		if (ImGui::SmallButton("+"))
		{
			bChanged = SubmitStructure(EPropertyChangeType::ArrayAdd,
				[&](const FArrayProperty& DraftProperty, void* DraftContainer, uint32 DraftArrayIndex) {
					return DraftProperty.Resize(DraftContainer, Num + 1, DraftArrayIndex);
				});
			if (bChanged) ++Num;
		}
		ImGui::SameLine();
		if (Num == 0) ImGui::BeginDisabled();
		if (ImGui::SmallButton("-") && Num > 0)
		{
			bChanged = SubmitStructure(EPropertyChangeType::ArrayRemove,
				[&](const FArrayProperty& DraftProperty, void* DraftContainer, uint32 DraftArrayIndex) {
					return DraftProperty.Resize(DraftContainer, Num - 1, DraftArrayIndex);
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.Property.Map");
		const char* Tooltip = Property->GetTypedMetadata().ToolTip.c_str();
		if (!Property->HasMapOps() || !Property->GetKeyProp() || !Property->GetValueProp())
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, Tooltip);
			ImGui::TextDisabled("<map metadata unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}

		uint64 Num = 0;
		if (!Property->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::MutableMappedTraversal)
			|| Property->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
		{
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), true, 0.0f, Tooltip);
			ImGui::TextDisabled("<map Count/MutableMappedTraversal unavailable>");
			MonaImGui::PropertyEdit::EndRow(true);
			return false;
		}
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = MonaImGui::CompactTreeNode("##Map", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%llu)", Label.c_str(), Num);
		MonaImGui::PropertyEdit::ShowLabelTooltip(Tooltip, bReadOnly);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		bool bChanged = false;
		auto SubmitStructure = [&](FPropertyEditTarget StructuralTarget, EPropertyChangeType::Type ChangeType, auto&& Mutation,
			bool bContinuous = false, const MonaImGui::PropertyEdit::FPropertyInteractionState* Interaction = nullptr) -> bool {
			StructuralTarget.ChangeType = ChangeType;
			if (!PreparePropertyWrite(Context, StructuralTarget, Interaction)) return false;
			FPropertyValueSnapshot Proposed;
			if (const auto Captured = CaptureProposedPropertyValue(StructuralTarget,
				[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft* Draft) {
					return Mutation(DraftValue, *Draft);
				}, Proposed); !Captured)
			{
				ReportEditError(Context, Captured.error());
				return false;
			}

			bool bAppliedChange = false;
			return ApplyPreparedPropertyEdit(Context, StructuralTarget, Proposed, bContinuous, &bAppliedChange) && bAppliedChange;
		};
		if (ImGui::SmallButton("+ Add"))
		{
			FReflectedValueStorage KeyStorage;
			FReflectedValueStorage ValueStorage;
			FPropertyValueSnapshot KeySnapshot;
			FPropertyValueSnapshot ValueSnapshot;
			std::string Error;
			if (!AcceptPropertyResult(KeyStorage.DefaultConstruct(Property->GetKeyProp(), 0), Error)
				|| !AcceptPropertyResult(ValueStorage.DefaultConstruct(Property->GetValueProp(), 0), Error)
				|| !AcceptPropertyResult(CapturePropertyValue(Property->GetKeyProp(), KeyStorage.GetContainer(), 0, KeySnapshot), Error)
				|| !AcceptPropertyResult(CapturePropertyValue(Property->GetValueProp(), ValueStorage.GetContainer(), 0, ValueSnapshot), Error))
			{
				ReportEditError(Context, Error.empty() ? "Unable to create a map-entry draft." : std::move(Error));
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
				if (!AcceptPropertyResult(DraftKeyStorage.DefaultConstruct(Property->GetKeyProp(), 0), Error)
					|| !AcceptPropertyResult(DraftValueStorage.DefaultConstruct(Property->GetValueProp(), 0), Error)
					|| !AcceptPropertyResult(RestorePropertyValue(Property->GetKeyProp(), DraftKeyStorage.GetContainer(), 0, MapInsertDraft.Key), Error)
					|| !AcceptPropertyResult(RestorePropertyValue(Property->GetValueProp(), DraftValueStorage.GetContainer(), 0, MapInsertDraft.Value), Error))
				{
					ReportEditError(Context, Error.empty() ? "Unable to restore the map-entry draft." : std::move(Error));
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
						if (!AcceptPropertyResult(CapturePropertyValue(Property->GetKeyProp(), DraftKey, 0, MapInsertDraft.Key), Error))
							ReportEditError(Context, std::move(Error));
					}
					const FPropertyWidgetEditResult ValueEdit = EditPropertyWidget(
						Context, Property->GetValueProp(), DraftValueStorage.GetContainer(), 0, "New Value", bReadOnly);
					if (ValueEdit.bChanged && ValueEdit.AssignValue)
					{
						ValueEdit.AssignValue(Property->GetValueProp(), DraftValue, 0);
						if (!AcceptPropertyResult(CapturePropertyValue(Property->GetValueProp(), DraftValue, 0, MapInsertDraft.Value), Error))
							ReportEditError(Context, std::move(Error));
					}

					MonaImGui::PropertyEdit::BeginRow("New Entry", bReadOnly);
					const bool bConfirmInsert = ImGui::SmallButton("Add");
					ImGui::SameLine();
					const bool bCancelInsert = ImGui::SmallButton("Cancel")
						|| KeyEdit.Interaction.IsCancelled() || ValueEdit.Interaction.IsCancelled();
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
							ReportEditError(Context, "Map keys must be unique.");
						}
						else
						{
							FPropertyEditTarget InsertTarget = EditTarget;
							InsertTarget.Path.back().Selector = EPropertyPathSelector::MapKey;
							InsertTarget.Path.back().MapKeyData = CaptureMapPathKey(Property->GetKeyProp(), DraftKey);
							InsertTarget.Path.back().MapKey = MapInsertDraft.Key.GetPayload();
							bChanged = SubmitStructure(std::move(InsertTarget), EPropertyChangeType::ArrayAdd,
								[&](const FResolvedPropertyValue& DraftMap, FPropertyValueDraft&) -> std::expected<void, std::string> {
									auto* DraftProperty = static_cast<const FMapProperty*>(DraftMap.Property);
									const EContainerOpResult Result = DraftProperty->InsertChecked(
										DraftMap.Container, DraftKey, DraftValue, DraftMap.ArrayIndex);
									return CheckMapMutation(Result, "insertion");
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
				ReportEditError(Context, "Unable to traverse reflected map entries.");
				ImGui::TreePop();
				return false;
			}
			for (uint64 Index = 0; Index < Entries.size(); ++Index)
			{
				const void* Key = Entries[static_cast<size_t>(Index)].Key;
				void* Value = Entries[static_cast<size_t>(Index)].Value;
				if (!Key || !Value) continue;
				FPropertyValueSnapshot KeySnapshot;
				if (!CapturePropertyValue(Property->GetKeyProp(), Key, 0, KeySnapshot))
				{
					continue;
				}
				const FByteBuffer SerializedKey = CaptureMapPathKey(Property->GetKeyProp(), Key);

				FPropertyEditTarget KeyTarget = EditTarget.ForMapEntry(
					Property->GetKeyProp(), KeySnapshot, SerializedKey);

				const bool bCurrentKey = IsEditingTarget(KeyTarget) && MapKeyInteraction.Value == KeySnapshot;
				const ImGuiID KeyId = bCurrentKey ? MapKeyInteraction.Id
					: ImHashData(SerializedKey.data(), SerializedKey.size(), ImGui::GetID("MapEntry"));
				ImGui::PushOverrideID(KeyId);
				ImGui::PushID("Key");
				const FPropertyWidgetEditResult KeyEdit = EditPropertyWidget(
					Context, Property->GetKeyProp(), const_cast<void*>(Key), 0, "Key", bReadOnly);
				ImGui::PopID();
				const bool bCancelKey = KeyEdit.Interaction.IsCancelled();
				if (bCurrentKey && bCancelKey)
				{
					UpdatePropertyValueEdit(Context, KeyTarget, KeyEdit.Interaction, {});
					ImGui::PopID();
					break;
				}
				const bool bKeyChanged = KeyEdit.bChanged && !bCancelKey;
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
				// Finishing another edit can replace map storage before the mutation runs.
				FReflectedValueStorage KeyStorage;
				if ((bRemove || bKeyChanged) && (!KeyStorage.DefaultConstruct(Property->GetKeyProp(), 0)
					|| !RestorePropertyValue(Property->GetKeyProp(), KeyStorage.GetContainer(), 0, KeySnapshot)))
				{
					ImGui::PopID();
					break;
				}
				if (bRemove)
				{
					FPropertyEditTarget RemoveTarget = EditTarget;
					RemoveTarget.Path.back().Selector = EPropertyPathSelector::MapKey;
					RemoveTarget.Path.back().MapKeyData = SerializedKey;
					RemoveTarget.Path.back().MapKey = KeySnapshot.GetPayload();
					bChanged |= SubmitStructure(std::move(RemoveTarget), EPropertyChangeType::ArrayRemove,
						[&](const FResolvedPropertyValue& DraftMap, FPropertyValueDraft&) -> std::expected<void, std::string> {
							auto* DraftProperty = static_cast<const FMapProperty*>(DraftMap.Property);
							const EContainerOpResult Result = DraftProperty->RemoveChecked(
								DraftMap.Container, KeyStorage.GetValue(), DraftMap.ArrayIndex);
							return CheckMapMutation(Result, "removal");
						});
				}
				else if (bKeyChanged)
				{
					if (IsEditingTarget(KeyTarget) && !bCurrentKey && !FinishActiveEdit(&Context, false))
					{
						ImGui::PopID();
						break;
					}
					FReflectedValueStorage ProposedKeyStorage;
					std::string ProposedKeyError;
					if (!AcceptPropertyResult(ProposedKeyStorage.DefaultConstruct(Property->GetKeyProp(), 0), ProposedKeyError)
						|| !KeyEdit.AssignValue)
					{
						ReportEditError(Context, ProposedKeyError.empty()
							? "Unable to materialize the proposed map key."
							: std::move(ProposedKeyError));
					}
					else
					{
						void* ProposedKey = ProposedKeyStorage.GetValue();
						KeyEdit.AssignValue(Property->GetKeyProp(), ProposedKeyStorage.GetContainer(), 0);
						FPropertyValueSnapshot ProposedKeySnapshot;
						std::string CaptureError;
						if (!AcceptPropertyResult(CapturePropertyValue(Property->GetKeyProp(), ProposedKey, 0, ProposedKeySnapshot), CaptureError))
						{
							ReportEditError(Context, std::move(CaptureError));
						}
						else
						{
							const bool bRenamed = SubmitStructure(KeyTarget, EPropertyChangeType::ValueSet,
								[&](const FResolvedPropertyValue&, FPropertyValueDraft& Draft) -> std::expected<void, std::string> {
									const FProperty* DraftProperty = nullptr;
									void* DraftContainer = nullptr;
									uint32 DraftArrayIndex = 0;
									if (const auto Result = Draft.Resolve(EditTarget, DraftProperty, DraftContainer, DraftArrayIndex); !Result)
									{
										return std::unexpected(FormatPropertyValueDraftError(Result.error()));
									}
									auto* DraftMap = static_cast<const FMapProperty*>(DraftProperty);
									const void* ExistingValue = nullptr;
									if (!(ProposedKeySnapshot == KeySnapshot)
										&& DraftMap->FindValue(DraftContainer, ProposedKey, &ExistingValue, DraftArrayIndex)
											== EContainerOpResult::Success)
										return std::unexpected(std::string("Map keys must be unique."));
									const EContainerOpResult Result = DraftMap->RenameKeyChecked(
										DraftContainer, KeyStorage.GetValue(), ProposedKey, DraftArrayIndex);
									return CheckMapMutation(Result, "key rename");
								}, true, &KeyEdit.Interaction);
							bChanged |= bRenamed;
							if (bRenamed)
							{
								MapKeyInteraction = {ProposedKeySnapshot, KeyId};
							}
						}
					}
				}
				if (IsEditingTarget(KeyTarget) && MapKeyInteraction.Id == KeyId)
				{
					if (EditInteraction.Observe(KeyEdit.Interaction)) FinishActiveEdit(&Context, false);
					else ReconcileInteraction(Context);
				}
				ImGui::PopID();
				// Rename and erase may change iteration order, so resume traversal next frame.
				if (bRemove || bKeyChanged) break;
			}
			ImGui::TreePop();
		}
		return bChanged;
	}

	auto FPropertyView::PreparePropertyWrite(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
		const MonaImGui::PropertyEdit::FPropertyInteractionState* Interaction) -> bool
	{
		if (Context.bReadOnly)
			return false;
		if (HandleControls.IsActive() && !FinishActiveEdit(&Context, ImGui::GetCurrentContext()
			&& ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
			return false;
		if (!EditSession.IsActive()) return true;
		const bool bSameTarget = IsEditingTarget(Target);
		const bool bCancel = EditInteraction.ShouldCancel();
		const bool bDifferentWidget = Interaction && EditInteraction.IsActive() && !EditInteraction.Matches(*Interaction);
		if (bCancel || !bSameTarget || bDifferentWidget)
		{
			if (!FinishActiveEdit(&Context, bCancel))
				return false;
			// A proposal from the cancelled gesture must never restart that gesture.
			if (bCancel && bSameTarget && !bDifferentWidget)
				return false;
		}
		return true;
	}

	auto FPropertyView::ApplyPreparedPropertyEdit(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
		const FPropertyValueSnapshot& ProposedValue, bool bContinuous, bool* OutChanged) -> bool
	{
		std::string Error;
		if (!EditSession.IsActive())
		{
			if (!EditSession.Begin(Target, {}, Context.bShouldTransact, &Error))
			{
				ReportEditError(Context, std::move(Error));
				return false;
			}
			ActiveEditObject = Target.Object;
			ActiveEditOwnerObject = OwnerContextObject ? OwnerContextObject : Target.Object;
		}

		if (!EditSession.ApplyInternal(ProposedValue.GetPayload(), &Error, OutChanged))
		{
			ReportEditError(Context, std::move(Error));
			FinishActiveEdit(&Context, true);
			return false;
		}
		return bContinuous || FinishActiveEdit(&Context, false);
	}

	auto FPropertyView::SubmitPropertyValueEdit(
		const FPropertyViewContext& Context,
		const FPropertyEditTarget& Target,
		const std::function<void(FProperty*, void*, uint32)>& AssignValue,
		bool bContinuous
	) -> bool
	{
		return ApplyPropertyValueEdit(Context, Target, AssignValue, bContinuous);
	}

	auto FPropertyView::ApplyPropertyValueEdit(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
		const std::function<void(FProperty*, void*, uint32)>& AssignValue, bool bContinuous,
		const MonaImGui::PropertyEdit::FPropertyInteractionState* Interaction, bool* OutChanged) -> bool
	{
		if (!AssignValue)
		{
			ReportEditError(Context, "The reflected property assignment is unavailable.");
			return false;
		}
		if (!PreparePropertyWrite(Context, Target, Interaction)) return false;
		FPropertyValueSnapshot Proposed;
		if (const auto Captured = CaptureProposedPropertyValue(Target,
			[&](const FResolvedPropertyValue& DraftValue, FPropertyValueDraft*) {
				AssignValue(const_cast<FProperty*>(DraftValue.Property), DraftValue.Container, DraftValue.ArrayIndex);
				return std::expected<void, std::string>{};
			}, Proposed); !Captured)
		{
			ReportEditError(Context, Captured.error());
			return false;
		}
		return ApplyPreparedPropertyEdit(Context, Target, Proposed, bContinuous, OutChanged);
	}

	auto FPropertyView::UpdatePropertyValueEdit(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
		const MonaImGui::PropertyEdit::FPropertyInteractionState& Interaction,
		const std::function<void(FProperty*, void*, uint32)>& AssignValue) -> bool
	{
		return UpdatePropertyValueEdit(Context, Target, Interaction, AssignValue, nullptr);
	}

	auto FPropertyView::UpdatePropertyValueEdit(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
		const MonaImGui::PropertyEdit::FPropertyInteractionState& Interaction,
		const std::function<void(FProperty*, void*, uint32)>& AssignValue, bool* OutChanged) -> bool
	{
		if (Interaction.IsCancelled() || Context.bReadOnly)
		{
			if (!EditSession.MatchesTarget(Target)) return true;
			// A refused Escape still counts as a draw of the active widget.
			if (EditInteraction.Matches(Interaction))
				EditInteraction.Observe({Interaction.Id, Interaction.Boundary});
			return FinishActiveEdit(&Context, true);
		}

		const bool bApplied = !AssignValue || ApplyPropertyValueEdit(Context, Target, AssignValue, true, &Interaction, OutChanged);
		if (!EditSession.MatchesTarget(Target)) return bApplied;
		if (EditInteraction.Observe(Interaction))
			return FinishActiveEdit(&Context, false) && bApplied;
		return ReconcileInteraction(Context) && bApplied;
	}

	auto FPropertyView::FinishActiveEdit(const FPropertyViewContext* Context, bool bCancel) -> bool
	{
		std::string Error;
		const bool bHandleFinished = HandleControls.Finish(bCancel, &Error);
		if (EditSession.IsActive() && bHandleFinished)
		{
			if (bCancel) EditSession.Cancel(&Error);
			else EditSession.Commit(&Error);
			if (!EditSession.IsActive())
			{
				ActiveEditObject = nullptr;
				ActiveEditOwnerObject = nullptr;
				EditInteraction = {};
				MapKeyInteraction = {};
			}
		}
		if (!Error.empty())
		{
			if (Context) ReportEditError(*Context, std::move(Error));
			else DURIN_ERROR("Could not finish the property edit: {}", Error);
		}
		return bHandleFinished && !EditSession.IsActive();
	}

	auto FPropertyView::HandleOwnerContext(const FPropertyViewContext& Context, DObject* Object) -> bool
	{
		if (HandleControls.IsActive() && (OwnerContextObject != Object || Context.bReadOnly))
			if (!FinishActiveEdit(&Context, true)) return false;
		if (EditSession.IsActive() && (ActiveEditOwnerObject != Object || Context.bReadOnly))
		{
			if (!FinishActiveEdit(&Context, true)) return false;
		}
		if (OwnerContextObject != Object || Context.bReadOnly) MapInsertDraft = {};
		OwnerContextObject = Object;
		return ReconcileInteraction(Context);
	}

	auto FPropertyView::EndInteractionFrame(const FPropertyViewContext& Context, bool* OutChanged) -> bool
	{
		std::string Error;
		const bool bFinished = HandleControls.EndFrame(Context.bReadOnly || ImGui::IsKeyPressed(ImGuiKey_Escape, false),
			Context.bShouldTransact, [&] { return ReconcileInteraction(Context, true) && FinishActiveEdit(&Context, false); },
			&Error, OutChanged);
		if (!Error.empty()) ReportEditError(Context, std::move(Error));
		return ReconcileInteraction(Context, true) && bFinished;
	}

	auto FPropertyView::ReconcileInteraction(const FPropertyViewContext& Context, bool bEndOfFrame) -> bool
	{
		if ((Context.bReadOnly && EditInteraction.IsActive()) || EditInteraction.ShouldCancel(bEndOfFrame))
			return FinishActiveEdit(&Context, true);
		return true;
	}

	auto FPropertyView::ReportEditError(const FPropertyViewContext& Context, std::string Error) -> void
	{
		if (Error.empty()) return;
		if (Context.ReportPropertyError)
		{
			Context.ReportPropertyError(std::move(Error));
			return;
		}
		if (!GEditor || !ImGui::GetCurrentContext()) return;
		auto* Notifications = GEditor->GetEditorSubsystem<DEditorNotificationSubsystem>();
		if (!Notifications) return;

		constexpr float DurationSeconds = 3.0f;
		const double Now = ImGui::GetTime();
		if (LastTransientError == Error && Now - LastTransientErrorTime < DurationSeconds) return;
		LastTransientError = Error;
		LastTransientErrorTime = Now;
		Notifications->GetManager().Post({
			.Type = ENotificationType::Warning,
			.Message = std::move(Error),
			.DurationSeconds = DurationSeconds,
		});
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
		if (const DEnum* Enum = GetArrayIndexEnum(Property))
		{
			if (const FEnumValue* Value = Enum->FindValueRecordByValue(ArrayIndex))
				return Value->DisplayName.empty()
					? StringUtils::HumanizeName(Value->Name.ToString()) : Value->DisplayName;
		}
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
