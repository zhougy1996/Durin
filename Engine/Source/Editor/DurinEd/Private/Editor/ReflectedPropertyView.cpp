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

		auto ScratchFail(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		class FReflectedPropertyScratch
		{
		public:
			explicit FReflectedPropertyScratch(const FReflectedPropertyEditTarget& Target, std::string* OutError)
				: Property(Target.SnapshotProperty)
				, ArrayIndex(Target.SnapshotArrayIndex)
			{
				if (!Property || !Target.SnapshotContainer)
				{
					ScratchFail(OutError, "The reflected property scratch root is unavailable.");
					return;
				}
				if (Property->HasValueAccessors())
				{
					ScratchFail(OutError, "Properties with custom value accessors cannot be used as scratch roots.");
					return;
				}
				if (!Property->HasValueLifecycle() || Property->GetValueSize() == 0 || Property->GetValueAlignment() == 0)
				{
					ScratchFail(OutError, "The reflected property lacks generated scratch-value lifecycle metadata.");
					return;
				}

				FPropertyValueSnapshot Current;
				if (!CapturePropertyValue(Property, Target.SnapshotContainer, ArrayIndex, Current, OutError)) return;
				Alignment = std::max<size_t>(Property->GetValueAlignment(), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
				Size = std::max<size_t>(1, static_cast<size_t>(Property->GetOffset())
					+ static_cast<size_t>(Property->GetElementSize()) * static_cast<size_t>(ArrayIndex)
					+ static_cast<size_t>(Property->GetValueSize()));
				Memory = ::operator new(Size, std::align_val_t(Alignment));
				std::memset(Memory, 0, Size);
				Value = Property->GetValuePtr(Memory, ArrayIndex);
				if (!Property->InitializeValue(Value))
				{
					ScratchFail(OutError, "Unable to initialize reflected property scratch storage.");
					return;
				}
				bInitialized = true;
				if (!RestorePropertyValue(Property, Memory, ArrayIndex, Current, OutError)) return;
				bValid = true;
			}

			~FReflectedPropertyScratch()
			{
				if (bInitialized) Property->DestroyValue(Value);
				if (Memory) ::operator delete(Memory, std::align_val_t(Alignment));
			}

			auto IsValid() const -> bool { return bValid; }

			auto Resolve(const FReflectedPropertyEditTarget& Source, FReflectedPropertyEditTarget& OutTarget,
				std::string* OutError) -> bool
			{
				if (!bValid || Source.SnapshotProperty != Property || Source.SnapshotArrayIndex != ArrayIndex
					|| Source.Path.empty() || Source.Path.front().Property != Property)
				{
					return ScratchFail(OutError, "The edit target does not match its reflected property scratch root.");
				}

				void* Container = Memory;
				uint32 CurrentArrayIndex = ArrayIndex;
				for (size_t PathIndex = 0; PathIndex < Source.Path.size(); ++PathIndex)
				{
					const FReflectedPropertyEditPathSegment& Segment = Source.Path[PathIndex];
					FProperty* CurrentProperty = const_cast<FProperty*>(Segment.Property);
					if (!CurrentProperty) return ScratchFail(OutError, "The scratch property path contains an empty segment.");
					if (PathIndex + 1 == Source.Path.size())
					{
						OutTarget = Source;
						OutTarget.SnapshotContainer = Memory;
						OutTarget.LeafContainer = Container;
						OutTarget.LeafArrayIndex = CurrentArrayIndex;
						return true;
					}

					FProperty* NextProperty = const_cast<FProperty*>(Source.Path[PathIndex + 1].Property);
					switch (Segment.Selector)
					{
					case EPropertyPathSelector::None:
					case EPropertyPathSelector::StaticArrayIndex:
						Container = CurrentProperty->GetValuePtr(Container, CurrentArrayIndex);
						CurrentArrayIndex = Source.Path[PathIndex + 1].Selector == EPropertyPathSelector::StaticArrayIndex
							? static_cast<uint32>(Source.Path[PathIndex + 1].Index) : 0;
						break;
					case EPropertyPathSelector::ArrayIndex:
					{
						auto* ArrayProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
							? static_cast<FArrayProperty*>(CurrentProperty) : nullptr;
						if (!ArrayProperty || Segment.Index >= ArrayProperty->Num(Container, CurrentArrayIndex))
							return ScratchFail(OutError, "The scratch array path index is unavailable.");
						Container = ArrayProperty->GetMutableElementPtr(Container, Segment.Index, CurrentArrayIndex);
						CurrentArrayIndex = 0;
						break;
					}
					case EPropertyPathSelector::MapKey:
					{
						auto* MapProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
							? static_cast<FMapProperty*>(CurrentProperty) : nullptr;
						if (!MapProperty || !Segment.MapKey.IsValid())
							return ScratchFail(OutError, "The scratch map path lacks a stable key snapshot.");
						uint64 MapIndex = UINT64_MAX;
						for (uint64 Index = 0; Index < MapProperty->Num(Container, CurrentArrayIndex); ++Index)
						{
							FPropertyValueSnapshot StoredKey;
							const void* Key = MapProperty->GetKeyPtr(Container, Index, CurrentArrayIndex);
							if (Key && CapturePropertyValue(MapProperty->GetKeyProp(), Key, 0, StoredKey)
								&& StoredKey == Segment.MapKey)
							{
								MapIndex = Index;
								break;
							}
						}
						if (MapIndex == UINT64_MAX) return ScratchFail(OutError, "The scratch map key is unavailable.");
						if (NextProperty == MapProperty->GetKeyProp())
							Container = const_cast<void*>(MapProperty->GetKeyPtr(Container, MapIndex, CurrentArrayIndex));
						else if (NextProperty == MapProperty->GetValueProp())
							Container = MapProperty->GetMutableMappedValuePtr(Container, MapIndex, CurrentArrayIndex);
						else
							return ScratchFail(OutError, "The scratch map path does not select its key or value property.");
						CurrentArrayIndex = 0;
						break;
					}
					default:
						return ScratchFail(OutError, "The scratch property path selector is unsupported.");
					}
					if (!Container || !NextProperty) return ScratchFail(OutError, "The scratch property path could not be resolved.");
				}
				return ScratchFail(OutError, "The scratch property path is empty.");
			}

			auto Capture(FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool
			{
				return bValid && CapturePropertyValue(Property, Memory, ArrayIndex, OutSnapshot, OutError);
			}

		private:
			const FProperty* Property = nullptr;
			uint32 ArrayIndex = 0;
			void* Memory = nullptr;
			void* Value = nullptr;
			size_t Size = 0;
			size_t Alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
			bool bInitialized = false;
			bool bValid = false;
		};

		template<typename TWriteProposed>
		auto CaptureProposedPropertyValue(const FReflectedPropertyEditTarget& Target,
			TWriteProposed&& WriteProposed, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) -> bool
		{
			FReflectedPropertyScratch Scratch(Target, OutError);
			if (!Scratch.IsValid()) return false;
			FReflectedPropertyEditTarget ScratchTarget;
			if (!Scratch.Resolve(Target, ScratchTarget, OutError)) return false;
			WriteProposed(ScratchTarget, &Scratch);
			const bool bCaptured = Scratch.Capture(OutSnapshot, OutError);
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
		HandleOwnerContext(Context, Object);
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
		if (bOwnsPropertyTable && !MonaImGui::BeginPropertyTable(Options.PropertyTableId)) return Result;
		for (const FVisibleProperty& VisibleProperty : VisibleProperties)
		{
			Result.bChanged |= EditProperty(Context, Object, VisibleProperty.Property, VisibleProperty.ArrayIndex);
		}
		if (bOwnsPropertyTable) MonaImGui::EndPropertyTable();
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
		return EditPropertyValueImpl(Context, Object, Property, Container, ArrayIndex, Label, bReadOnly, EditTarget,
			EPropertyValueEditDestination::EditPipeline);
	}

	auto FReflectedPropertyView::EditDetachedTemporaryPropertyValue(
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
		// Detached widget state is never an editable reflected target. Its owner must
		// explicitly submit the resulting value through the edit pipeline.
		return EditPropertyValueImpl(Context, Object, Property, Container, ArrayIndex, Label, bReadOnly, EditTarget,
			EPropertyValueEditDestination::DetachedTemporary);
	}

	auto FReflectedPropertyView::EditPropertyValueImpl(
		const FReflectedPropertyViewContext& Context,
		DObject* Object,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const std::string& Label,
		bool bReadOnly,
		const FReflectedPropertyEditTarget& EditTarget,
		EPropertyValueEditDestination Destination
	) -> bool
	{
		bReadOnly |= Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
		DStruct* Struct = Kind == DurinCodeGen::EPropertyGenFlags::Struct ? static_cast<FStructProperty*>(Property)->GetStruct() : nullptr;
		auto SubmitProposed = [&](auto&& WriteProposed, bool bContinuous) -> bool {
			if (Destination == EPropertyValueEditDestination::DetachedTemporary)
			{
				WriteProposed(EditTarget, nullptr);
				return true;
			}
			FPropertyValueSnapshot Proposed;
			std::string Error;
			if (!CaptureProposedPropertyValue(EditTarget, std::forward<decltype(WriteProposed)>(WriteProposed), Proposed, &Error))
			{
				ReportError(Context, std::move(Error));
				return false;
			}
			return SubmitPropertyEdit(Context, EditTarget, Proposed, bContinuous);
		};
		auto FinishContinuousEdit = [&](const MonaImGui::FPropertyEditWidgetState& State) {
			if (Destination == EPropertyValueEditDestination::DetachedTemporary) return;
			if (State.bDeactivatedAfterEdit && IsEditingTarget(EditTarget)) FinishActiveEdit(&Context, false);
			else if (State.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && IsEditingTarget(EditTarget)) FinishActiveEdit(&Context, true);
		};

		if (Struct == Z_Construct_DStruct_Durin_FTransform())
		{
			FTransform Value = *Property->ContainerPtrToValuePtr<FTransform>(Container, ArrayIndex);
			MonaImGui::FPropertyEditWidgetState State;
			bool bChanged = MonaImGui::EditTransformProperty(Label.c_str(), Value, bReadOnly, &State);
			if (bChanged)
			{
				bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
					*Property->ContainerPtrToValuePtr<FTransform>(ProposedTarget.LeafContainer, ProposedTarget.LeafArrayIndex) = Value;
				}, true);
			}
			FinishContinuousEdit(State);
			return bChanged;
		}

		auto EditMathStruct = [&]<typename TValue, typename TEditor>(TValue Value, TEditor&& Editor) -> bool {
			MonaImGui::FPropertyEditWidgetState State;
			bool bChanged = Editor(Value, State);
			if (bChanged)
			{
				bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
					*Property->ContainerPtrToValuePtr<TValue>(ProposedTarget.LeafContainer, ProposedTarget.LeafArrayIndex) = Value;
				}, true);
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
			if (bChanged) bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
				*Property->ContainerPtrToValuePtr<bool>(ProposedTarget.LeafContainer, ProposedTarget.LeafArrayIndex) = Value;
			}, false);
		}
		else if (const ImGuiDataType DataType = ImGuiDataTypeForProperty(Kind); DataType != ImGuiDataType_COUNT)
		{
			std::array<uint8, sizeof(uint64)> Value{};
			check(Property->GetElementSize() <= Value.size());
			std::memcpy(Value.data(), Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
			bChanged = ImGui::DragScalar("##Value", DataType, Value.data(), Kind == DurinCodeGen::EPropertyGenFlags::Float || Kind == DurinCodeGen::EPropertyGenFlags::Double ? 0.05f : 1.0f);
			MonaImGui::FPropertyEditWidgetState State{ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()};
			if (bChanged) bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
				std::memcpy(Property->GetValuePtr(ProposedTarget.LeafContainer, ProposedTarget.LeafArrayIndex), Value.data(), Property->GetElementSize());
			}, true);
			FinishContinuousEdit(State);
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			auto* StringProperty = static_cast<FStringProperty*>(Property);
			std::string Value = *StringProperty->GetStringValuePtr(Container, ArrayIndex);
			if (MonaImGui::InputText("##Value", Value))
			{
				bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
					*StringProperty->GetStringValuePtr(ProposedTarget.LeafContainer, ProposedTarget.LeafArrayIndex) = Value;
				}, true);
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
							bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
								WriteEnumValue(*EnumProperty, ProposedTarget.LeafContainer, ProposedTarget.LeafArrayIndex, Value.Value);
							}, false);
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
			if (bChanged) bChanged = SubmitProposed([&](const FReflectedPropertyEditTarget& ProposedTarget, FReflectedPropertyScratch*) {
				ObjectProperty->SetObjectPropertyValue(ProposedTarget.LeafContainer, SelectedObject, ProposedTarget.LeafArrayIndex);
			}, false);
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
			if (!CaptureProposedPropertyValue(StructuralTarget,
				[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch*) {
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
			std::string MutationError;
			if (!CaptureProposedPropertyValue(StructuralTarget,
				[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch* Scratch) {
					Mutation(ScratchTarget, *Scratch, MutationError);
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
				CapturePropertyValue(Property->GetKeyProp(), Key, 0, InsertTarget.Path.back().MapKey);
				bChanged = SubmitStructure(std::move(InsertTarget), EPropertyChangeKind::MapInsert,
					[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch&, std::string&) {
						auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchTarget.LeafProperty);
						ScratchProperty->Insert(ScratchTarget.LeafContainer, Key, Value, ScratchTarget.LeafArrayIndex);
					});
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
				FPropertyValueSnapshot KeySnapshot;
				if (!CapturePropertyValue(Property->GetKeyProp(), Key, 0, KeySnapshot))
				{
					ImGui::PopID();
					continue;
				}
				const std::vector<uint8> SerializedKey = CaptureMapPathKey(Property->GetKeyProp(), Key);

				void* EditedKey = Property->CreateKeyCopy(Key);
				FReflectedPropertyEditTarget KeyTarget = EditTarget.ForMapEntry(Property->GetKeyProp(), EditedKey, KeySnapshot, SerializedKey);
				KeyTarget.Kind = EPropertyChangeKind::MapKeyRename;
				const bool bKeyChanged = EditedKey && EditDetachedTemporaryPropertyValue(Context, Object, Property->GetKeyProp(), EditedKey, 0, std::format("[{}] Key", Index), bReadOnly, KeyTarget);
				const MonaImGui::FPropertyEditWidgetState KeyState{
					ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()
				};
				const FReflectedPropertyEditTarget ValueTarget = EditTarget.ForMapEntry(Property->GetValueProp(), Value, KeySnapshot, SerializedKey);
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
					RemoveTarget.Path.back().MapKey = KeySnapshot;
					bChanged |= SubmitStructure(std::move(RemoveTarget), EPropertyChangeKind::MapRemove,
						[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch&, std::string&) {
							auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchTarget.LeafProperty);
							ScratchProperty->Remove(ScratchTarget.LeafContainer, Key, ScratchTarget.LeafArrayIndex);
						});
				}
				else if (bKeyChanged && Property->Contains(Container, EditedKey, ArrayIndex))
				{
					ReportError(Context, "Map keys must be unique.");
				}
				else if (bKeyChanged)
				{
					bChanged |= SubmitStructure(KeyTarget, EPropertyChangeKind::MapKeyRename,
						[&](const FReflectedPropertyEditTarget&, FReflectedPropertyScratch& Scratch, std::string& MutationError) {
							FReflectedPropertyEditTarget ScratchMapTarget;
							if (!Scratch.Resolve(EditTarget, ScratchMapTarget, &MutationError)) return;
							auto* ScratchProperty = static_cast<const FMapProperty*>(ScratchMapTarget.LeafProperty);
							ScratchProperty->RenameKey(ScratchMapTarget.LeafContainer, Key, EditedKey, ScratchMapTarget.LeafArrayIndex);
						}, true);
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
			[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch*) {
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
			[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch*) {
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
			[&](const FReflectedPropertyEditTarget& ScratchTarget, FReflectedPropertyScratch*) {
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
