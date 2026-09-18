#include "DObject/DurinPropertyTypes.h"

#include "DObject/Class.h"
#include "DObject/CanonicalMapKey.h"
#include "DObject/DefaultObjectGraph.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/PropertyKindTraits.h"

namespace Durin
{
	auto FProperty::InitializeDeprecation() -> void
	{
		Deprecation.reset();
		if (!HasAnyPropertyFlags(EPropertyFlags::Deprecated)) return;
		std::string HistoricalName = NamePrivate.ToString();
		HistoricalName.resize(HistoricalName.size() - std::string_view("_DEPRECATED").size());
		Deprecation = FPropertyDeprecation{.HistoricalName = FName(HistoricalName)};
	}

	auto FProperty::SetTypedMetadata(const FPropertyMetadataParams* InMetadata) -> void
	{
		TypedMetadata = {};
		if (!InMetadata) return;
		TypedMetadata.DisplayName = InMetadata->DisplayName ? InMetadata->DisplayName : "";
		TypedMetadata.ToolTip = InMetadata->ToolTip ? InMetadata->ToolTip : "";
		TypedMetadata.Category = InMetadata->Category ? InMetadata->Category : "";
		TypedMetadata.Units = InMetadata->Units;
		TypedMetadata.Step = InMetadata->Step;
		TypedMetadata.Precision = InMetadata->Precision;
		TypedMetadata.ClampMin = InMetadata->ClampMin;
		TypedMetadata.ClampMax = InMetadata->ClampMax;
		TypedMetadata.UIMin = InMetadata->UIMin;
		TypedMetadata.UIMax = InMetadata->UIMax;
		if (!TypedMetadata.DisplayName.empty()) SetMetaData(FName("DisplayName"), TypedMetadata.DisplayName);
		if (!TypedMetadata.ToolTip.empty()) SetMetaData(FName("ToolTip"), TypedMetadata.ToolTip);
		if (!TypedMetadata.Category.empty()) SetMetaData(FName("Category"), TypedMetadata.Category);
	}

	namespace
	{
		template<typename T>
		auto MetadataNumberAs(const FPropertyMetadataNumber& Number) -> T
		{
			if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) return static_cast<T>(Number.Signed);
			if constexpr (std::is_unsigned_v<T>) return static_cast<T>(Number.Unsigned);
			if constexpr (std::is_same_v<T, float>) return Number.Float;
			return static_cast<T>(Number.Double);
		}

		template<typename T>
		auto ValidateScalarMetadata(const FPropertyMetadata& Metadata, const void* Value) -> FPropertyEditValueResult
		{
			const T Current = *static_cast<const T*>(Value);
			FPropertyEditValueError Error{.Minimum = Metadata.ClampMin, .Maximum = Metadata.ClampMax};
			if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) Error.Current = FPropertyMetadataNumber::FromSigned(Current);
			else if constexpr (std::is_integral_v<T>) Error.Current = FPropertyMetadataNumber::FromUnsigned(Current);
			else if constexpr (std::is_same_v<T, float>) Error.Current = FPropertyMetadataNumber::FromFloat(Current);
			else Error.Current = FPropertyMetadataNumber::FromDouble(Current);
			if constexpr (std::is_floating_point_v<T>)
			{
				if ((Metadata.ClampMin.Kind != EPropertyMetadataNumericKind::None
					|| Metadata.ClampMax.Kind != EPropertyMetadataNumericKind::None) && !std::isfinite(Current))
					Error.Code = EPropertyEditValueError::NonFinite;
			}
			if (Error.Code == EPropertyEditValueError::None && Metadata.ClampMin.Kind != EPropertyMetadataNumericKind::None
				&& Current < MetadataNumberAs<T>(Metadata.ClampMin)) Error.Code = EPropertyEditValueError::BelowMinimum;
			if (Error.Code == EPropertyEditValueError::None && Metadata.ClampMax.Kind != EPropertyMetadataNumericKind::None
				&& Current > MetadataNumberAs<T>(Metadata.ClampMax)) Error.Code = EPropertyEditValueError::AboveMaximum;
			return {std::move(Error)};
		}

		auto ValidateMetadataValue(const FProperty* Property, const FPropertyMetadata& Metadata,
			const void* Value) -> FPropertyEditValueResult
		{
			auto Result = [&]() -> FPropertyEditValueResult {
				switch (Property->GetKind())
				{
				case DurinCodeGen::EPropertyGenFlags::Int8: return ValidateScalarMetadata<int8>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::Int16: return ValidateScalarMetadata<int16>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::Int32: return ValidateScalarMetadata<int32>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::Int64: return ValidateScalarMetadata<int64>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::UInt8: return ValidateScalarMetadata<uint8>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::UInt16: return ValidateScalarMetadata<uint16>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::UInt32: return ValidateScalarMetadata<uint32>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::UInt64: return ValidateScalarMetadata<uint64>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::Float: return ValidateScalarMetadata<float>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::Double: return ValidateScalarMetadata<double>(Metadata, Value);
				case DurinCodeGen::EPropertyGenFlags::Struct:
				{
					auto* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
					if (!Struct) return {{.Code = EPropertyEditValueError::MissingStruct}};
					FPropertyEditValueResult Nested;
					Struct->ForEachProperty([&](FProperty* Field) {
						if (Nested && Field) Nested = ValidateMetadataValue(Field, Metadata, Field->GetValuePtr(Value));
					}, false);
					return Nested;
				}
				default: return {};
				}
			}();
			if (!Result)
			{
				if (Result.Error.PropertyName.empty()) Result.Error.PropertyName = Property->NamePrivate.ToString();
				Result.Error.Route.insert(Result.Error.Route.begin(), Property->NamePrivate.ToString());
			}
			return Result;
		}
	}

	auto FormatPropertyEditValueError(const FPropertyEditValueError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EPropertyEditValueError::None: return {};
		case EPropertyEditValueError::NonFinite: return "The proposed value is non-finite and violates its property-edit bounds.";
		case EPropertyEditValueError::BelowMinimum: return "The proposed value is below ClampMin.";
		case EPropertyEditValueError::AboveMaximum: return "The proposed value exceeds ClampMax.";
		case EPropertyEditValueError::MissingStruct: return "The reflected Struct metadata is unavailable.";
		default: return "The reflected property value is unavailable.";
		}
	}

	auto ValidatePropertyEditValue(const FProperty* Property, const void* Container,
		uint32 ArrayIndex) -> FPropertyEditValueResult
	{
		FPropertyEditValueResult Result;
		if (!Property) Result.Error.Code = EPropertyEditValueError::NullProperty;
		else if (!Container) Result.Error.Code = EPropertyEditValueError::NullContainer;
		else if (ArrayIndex >= Property->GetArrayDim()) Result.Error.Code = EPropertyEditValueError::InvalidArrayIndex;
		else
		{
			const auto& Metadata = Property->GetTypedMetadata();
			if (Metadata.ClampMin.Kind == EPropertyMetadataNumericKind::None
				&& Metadata.ClampMax.Kind == EPropertyMetadataNumericKind::None) return {};
			Result = ValidateMetadataValue(Property, Metadata, Property->GetValuePtr(Container, ArrayIndex));
		}
		if (!Result)
		{
			Result.Error.ArrayIndex = ArrayIndex;
			if (Property)
			{
				Result.Error.ArrayDim = Property->GetArrayDim();
				if (Result.Error.PropertyName.empty()) Result.Error.PropertyName = Property->NamePrivate.ToString();
			}
		}
		return Result;
	}

	namespace
	{
		template<typename T>
		auto ReadEnumValue(const void* ValuePtr) -> uint64
		{
			T Value;
			std::memcpy(&Value, ValuePtr, sizeof(Value));
			if constexpr (std::is_signed_v<T>)
				return static_cast<uint64>(static_cast<int64>(Value));
			else
				return static_cast<uint64>(Value);
		}

		template<typename T>
		auto WriteEnumValue(void* ValuePtr, uint64 Value) -> void
		{
			const T NarrowValue = static_cast<T>(Value);
			std::memcpy(ValuePtr, &NarrowValue, sizeof(NarrowValue));
		}

		auto GetPropertyStruct(const FProperty* Property) -> DStruct*
		{
			if (!Property || Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct) return nullptr;
			return static_cast<const FStructProperty*>(Property)->GetStruct();
		}

		auto HasBuiltInValueLifecycle(const FProperty* Property) -> bool
		{
			if (!Property) return false;
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
			case DurinCodeGen::EPropertyGenFlags::Enum:
			case DurinCodeGen::EPropertyGenFlags::String:
			case DurinCodeGen::EPropertyGenFlags::Name:
			case DurinCodeGen::EPropertyGenFlags::Guid:
			case DurinCodeGen::EPropertyGenFlags::Byte:
			case DurinCodeGen::EPropertyGenFlags::Object:
				return true;
			default:
				return false;
			}
		}

		auto PropertyValueFailure(const FProperty* Property, EPropertyValueError Code,
			EPropertyValueOperation Operation, uint32 ArrayIndex = 0) -> FPropertyValueResult
		{
			FPropertyValueError Error;
			Error.Code = Code;
			Error.Operation = Operation;
			Error.ArrayIndex = ArrayIndex;
			if (Property)
			{
				Error.PropertyName = Property->NamePrivate.ToString();
				Error.ArrayDim = Property->GetArrayDim();
				Error.ValueSize = Property->GetValueSize();
				Error.ValueAlignment = Property->GetValueAlignment();
				if (const auto* Struct = GetPropertyStruct(Property))
					Error.StructName = Struct->GetQualifiedName().ToString();
			}
			return {std::move(Error)};
		}

		auto ContainerFailure(const FProperty* Property, EPropertyContainerOperation Operation,
			uint32 Index, EContainerOpResult Code, const FProperty* ValueProperty = nullptr,
			EPropertyContainerRequirement Requirement = EPropertyContainerRequirement::None) -> FPropertyContainerResult
		{
			FPropertyContainerError Error{.Code = Code, .Operation = Operation, .Requirement = Requirement,
				.PropertyName = Property->NamePrivate.ToString(), .ArrayIndex = Index, .ArrayDim = Property->GetArrayDim()};
			if (ValueProperty)
			{
				Error.ValuePropertyName = ValueProperty->NamePrivate.ToString();
				if (auto* Struct = GetPropertyStruct(ValueProperty)) Error.StructName = Struct->GetQualifiedName().ToString();
			}
			return {std::move(Error)};
		}

		struct FPropertyIdentityContext
		{
			FPropertyIdentityDiagnostic* Diagnostic = nullptr;
			const FDefaultObjectGraphMap* DefaultGraph = nullptr;
			std::vector<const DStruct*> ActiveStructs;
		};

		auto SetIdentityDiagnostic(
			FPropertyIdentityContext& Context,
			std::string_view Path,
			DurinCodeGen::EPropertyGenFlags Kind,
			EPropertyIdentityReason Reason,
			EPropertyIdentityResult Result
		) -> EPropertyIdentityResult
		{
			if (Context.Diagnostic && Context.Diagnostic->Reason == EPropertyIdentityReason::None)
			{
				Context.Diagnostic->PropertyPath.assign(Path);
				Context.Diagnostic->LogicalKind = Kind;
				Context.Diagnostic->Reason = Reason;
			}
			return Result;
		}

		auto AppendIdentityPath(
			FPropertyIdentityContext& Context,
			std::string_view Path,
			std::string_view Segment,
			DurinCodeGen::EPropertyGenFlags Kind,
			std::string& OutPath
		) -> bool
		{
			if (Path.size() + Segment.size() > PropertyIdentityMaxPathLength)
			{
				SetIdentityDiagnostic(
					Context, Path, Kind, EPropertyIdentityReason::DiagnosticPathLimit,
					EPropertyIdentityResult::Unsupported
				);
				return false;
			}
			OutPath.assign(Path);
			OutPath.append(Segment);
			return true;
		}

		auto HexToken(FByteView Token) -> std::string
		{
			static constexpr char Digits[] = "0123456789abcdef";
			std::string Result;
			Result.reserve(Token.size() * 2 + 2);
			Result.push_back('{');
			for (std::byte ByteValue : Token)
			{
				const uint8 Byte = std::to_integer<uint8>(ByteValue);
				Result.push_back(Digits[Byte >> 4]);
				Result.push_back(Digits[Byte & 0x0f]);
			}
			Result.push_back('}');
			return Result;
		}

		struct FIdentityMapEntry
		{
			const void* Key = nullptr;
			const void* Value = nullptr;
			FByteBuffer KeyToken;
		};

		struct FIdentityMapCollectContext
		{
			const FProperty* KeyProperty = nullptr;
			std::vector<FIdentityMapEntry>* Entries = nullptr;
			FReflectedMapKeyError Error;
			bool bSucceeded = true;
		};

		auto CollectIdentityMapEntry(void* RawContext, const void* Key, const void* Value) -> bool
		{
			auto& Context = *static_cast<FIdentityMapCollectContext*>(RawContext);
			FIdentityMapEntry Entry{Key, Value, {}};
			if (const auto Result = BuildCanonicalMapKeyToken(Context.KeyProperty, Key, 0, Entry.KeyToken); !Result)
			{
				Context.Error = Result.Error;
				Context.bSucceeded = false;
				return false;
			}
			Context.Entries->push_back(std::move(Entry));
			return true;
		}

		auto CompareStructValuesImpl(
			const DStruct* Struct,
			const void* LeftValue,
			const void* RightValue,
			uint32 Depth,
			std::string_view Path,
			FPropertyIdentityContext& Context
		) -> EPropertyIdentityResult;

		auto ComparePropertyValuesImpl(
			const FProperty* Property,
			const void* LeftContainer,
			uint32 LeftArrayIndex,
			const void* RightContainer,
			uint32 RightArrayIndex,
			uint32 Depth,
			std::string_view Path,
			FPropertyIdentityContext& Context
		) -> EPropertyIdentityResult
		{
			using enum EPropertyIdentityResult;
			using enum EPropertyIdentityReason;
			const auto Kind = Property ? Property->GetKind() : DurinCodeGen::EPropertyGenFlags::None;
			if (!Property || !LeftContainer || !RightContainer)
				return SetIdentityDiagnostic(Context, Path, Kind, InvalidInput, Unsupported);
			if (LeftArrayIndex >= Property->GetArrayDim() || RightArrayIndex >= Property->GetArrayDim())
				return SetIdentityDiagnostic(Context, Path, Kind, InvalidArrayIndex, Unsupported);
			if (Depth > PropertyIdentityMaxDepth)
				return SetIdentityDiagnostic(Context, Path, Kind, DepthLimit, Unsupported);

			if (DurinCodeGen::IsBitwiseIdentityKind(Kind))
			{
				return std::memcmp(
						   Property->GetValuePtr(LeftContainer, LeftArrayIndex),
						   Property->GetValuePtr(RightContainer, RightArrayIndex),
						   Property->GetElementSize()
					   )
					   == 0 ? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
			}

			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::String:
				{
					const auto* StringProperty = static_cast<const FStringProperty*>(Property);
					return *StringProperty->GetStringValuePtr(LeftContainer, LeftArrayIndex)
						   == *StringProperty->GetStringValuePtr(RightContainer, RightArrayIndex)
						? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::Name:
				{
					const auto* NameProperty = static_cast<const FNameProperty*>(Property);
					return *NameProperty->GetNameValuePtr(LeftContainer, LeftArrayIndex)
						   == *NameProperty->GetNameValuePtr(RightContainer, RightArrayIndex)
						? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::Guid:
				{
					const auto* GuidProperty = static_cast<const FGuidProperty*>(Property);
					return *GuidProperty->GetGuidValuePtr(LeftContainer, LeftArrayIndex)
						   == *GuidProperty->GetGuidValuePtr(RightContainer, RightArrayIndex)
						? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::Blob:
				{
					const auto& Left = *static_cast<const FByteBuffer*>(
						Property->GetValuePtr(LeftContainer, LeftArrayIndex));
					const auto& Right = *static_cast<const FByteBuffer*>(
						Property->GetValuePtr(RightContainer, RightArrayIndex));
					return Left == Right ? Identical
						: SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::BulkData:
				{
					const std::optional<bool> bIdentical = Property->AreBulkDataValuesIdentical(
						Property->GetValuePtr(LeftContainer, LeftArrayIndex),
						Property->GetValuePtr(RightContainer, RightArrayIndex));
					if (!bIdentical)
						return SetIdentityDiagnostic(Context, Path, Kind,
							UnsupportedLogicalKind, Unsupported);
					return *bIdentical ? Identical
						: SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::Object:
				{
					const auto* ObjectProperty = static_cast<const FObjectProperty*>(Property);
					const DObject* Left = ObjectProperty->GetObjectPropertyValue(LeftContainer, LeftArrayIndex);
					const DObject* Right = ObjectProperty->GetObjectPropertyValue(RightContainer, RightArrayIndex);
					return (Left == Right || (Context.DefaultGraph && Context.DefaultGraph->AreReferencesEquivalent(Left, Right)))
						? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				{
					const auto* SoftProperty = static_cast<const FSoftObjectProperty*>(Property);
					const FSoftObjectPtr* Left = SoftProperty->GetSoftObjectPtr(LeftContainer, LeftArrayIndex);
					const FSoftObjectPtr* Right = SoftProperty->GetSoftObjectPtr(RightContainer, RightArrayIndex);
					if (!Left || !Right) return SetIdentityDiagnostic(Context, Path, Kind, InvalidInput, Unsupported);
					return *Left == *Right ? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::WeakObject:
				{
					const auto* WeakProperty = static_cast<const FWeakObjectProperty*>(Property);
					const FWeakObjectPtr* Left = WeakProperty->GetWeakObjectPtr(LeftContainer, LeftArrayIndex);
					const FWeakObjectPtr* Right = WeakProperty->GetWeakObjectPtr(RightContainer, RightArrayIndex);
					if (!Left || !Right) return SetIdentityDiagnostic(Context, Path, Kind, InvalidInput, Unsupported);
					const DObject* LeftObject = Left->Get();
					const DObject* RightObject = Right->Get();
					return (LeftObject == RightObject || (Context.DefaultGraph && Context.DefaultGraph->AreReferencesEquivalent(LeftObject, RightObject)))
						? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);
				}
			case DurinCodeGen::EPropertyGenFlags::Struct:
				{
					const auto* StructProperty = static_cast<const FStructProperty*>(Property);
					DStruct* Struct = StructProperty->GetStruct();
					const void* LeftValue = Property->GetValuePtr(LeftContainer, LeftArrayIndex);
					const void* RightValue = Property->GetValuePtr(RightContainer, RightArrayIndex);
					return CompareStructValuesImpl(Struct, LeftValue, RightValue, Depth, Path, Context);
				}
			case DurinCodeGen::EPropertyGenFlags::Array:
				{
					const auto* ArrayProperty = static_cast<const FArrayProperty*>(Property);
					FProperty* Inner = ArrayProperty->GetInner();
					if (!Inner) return SetIdentityDiagnostic(Context, Path, Kind, MissingArrayDescriptor, Unsupported);
					if (!ArrayProperty->HasArrayOps()) return SetIdentityDiagnostic(Context, Path, Kind, MissingArrayOperations, Unsupported);
					uint64 LeftNum = 0;
					uint64 RightNum = 0;
					if (ArrayProperty->GetNum(LeftContainer, LeftNum, LeftArrayIndex) != EContainerOpResult::Success
						|| ArrayProperty->GetNum(RightContainer, RightNum, RightArrayIndex) != EContainerOpResult::Success)
						return SetIdentityDiagnostic(Context, Path, Kind, ContainerOperationFailed, Unsupported);
					if (LeftNum != RightNum)
						return SetIdentityDiagnostic(Context, Path, Kind, ContainerLengthMismatch, Different);
					for (uint64 Index = 0; Index < LeftNum; ++Index)
					{
						const void* LeftElement = nullptr;
						const void* RightElement = nullptr;
						if (ArrayProperty->GetElement(LeftContainer, Index, &LeftElement, LeftArrayIndex) != EContainerOpResult::Success
							|| ArrayProperty->GetElement(RightContainer, Index, &RightElement, RightArrayIndex) != EContainerOpResult::Success)
							return SetIdentityDiagnostic(Context, Path, Kind, ContainerOperationFailed, Unsupported);
						std::string ChildPath;
						if (!AppendIdentityPath(Context, Path, std::format("[{}]", Index), Inner->GetKind(), ChildPath)) return Unsupported;
						const auto Result = ComparePropertyValuesImpl(
							Inner, LeftElement, 0, RightElement, 0, Depth + 1, ChildPath, Context
						);
						if (Result != Identical) return Result;
					}
					return Identical;
				}
			case DurinCodeGen::EPropertyGenFlags::Map:
				{
					const auto* MapProperty = static_cast<const FMapProperty*>(Property);
					FProperty* KeyProperty = MapProperty->GetKeyProp();
					FProperty* ValueProperty = MapProperty->GetValueProp();
					if (!KeyProperty || !ValueProperty)
						return SetIdentityDiagnostic(Context, Path, Kind, MissingMapDescriptor, Unsupported);
					if (!MapProperty->HasMapOps() || !MapProperty->HasCapability(EMapOpsFlags::Lookup | EMapOpsFlags::ConstTraversal))
						return SetIdentityDiagnostic(Context, Path, Kind, MissingMapOperations, Unsupported);
					uint64 LeftNum = 0;
					uint64 RightNum = 0;
					if (MapProperty->GetNum(LeftContainer, LeftNum, LeftArrayIndex) != EContainerOpResult::Success
						|| MapProperty->GetNum(RightContainer, RightNum, RightArrayIndex) != EContainerOpResult::Success)
						return SetIdentityDiagnostic(Context, Path, Kind, ContainerOperationFailed, Unsupported);
					if (LeftNum != RightNum)
						return SetIdentityDiagnostic(Context, Path, Kind, ContainerLengthMismatch, Different);

					std::vector<FIdentityMapEntry> Entries;
					Entries.reserve(static_cast<size_t>(LeftNum));
					FIdentityMapCollectContext Collect{KeyProperty, &Entries};
					if (MapProperty->VisitEntries(LeftContainer, &CollectIdentityMapEntry, &Collect, LeftArrayIndex)
							!= EContainerOpResult::Success || !Collect.bSucceeded)
						return SetIdentityDiagnostic(Context, Path, Kind, MissingMapOperations, Unsupported);
					std::ranges::sort(Entries, {}, &FIdentityMapEntry::KeyToken);
					for (const FIdentityMapEntry& Entry : Entries)
					{
						const std::string Segment = HexToken(Entry.KeyToken);
						std::string ChildPath;
						if (!AppendIdentityPath(Context, Path, Segment, ValueProperty->GetKind(), ChildPath)) return Unsupported;
						const void* RightValue = nullptr;
						const EContainerOpResult Lookup = MapProperty->FindValue(
							RightContainer, Entry.Key, &RightValue, RightArrayIndex
						);
						if (Lookup == EContainerOpResult::NotFound)
							return SetIdentityDiagnostic(Context, ChildPath, Kind, MapKeyMissing, Different);
						if (Lookup != EContainerOpResult::Success)
							return SetIdentityDiagnostic(Context, ChildPath, Kind, ContainerOperationFailed, Unsupported);
						const auto Result = ComparePropertyValuesImpl(
							ValueProperty, Entry.Value, 0, RightValue, 0, Depth + 1, ChildPath, Context
						);
						if (Result != Identical) return Result;
					}
					return Identical;
				}
			default:
				return SetIdentityDiagnostic(Context, Path, Kind, UnsupportedLogicalKind, Unsupported);
			}
		}

		auto CompareStructValuesImpl(
			const DStruct* Struct,
			const void* LeftValue,
			const void* RightValue,
			uint32 Depth,
			std::string_view Path,
			FPropertyIdentityContext& Context
		) -> EPropertyIdentityResult
		{
			using enum EPropertyIdentityResult;
			using enum EPropertyIdentityReason;
			constexpr auto Kind = DurinCodeGen::EPropertyGenFlags::Struct;
			if (!Struct) return SetIdentityDiagnostic(Context, Path, Kind, MissingStructDescriptor, Unsupported);
			if (!LeftValue || !RightValue) return SetIdentityDiagnostic(Context, Path, Kind, InvalidInput, Unsupported);
			if (Depth > PropertyIdentityMaxDepth) return SetIdentityDiagnostic(Context, Path, Kind, DepthLimit, Unsupported);
			if (!Struct->HasCompleteAuthoredFields())
				return SetIdentityDiagnostic(Context, Path, Kind, IncompleteAuthoredFields, Unsupported);
			if (std::ranges::find(Context.ActiveStructs, Struct) != Context.ActiveStructs.end())
				return SetIdentityDiagnostic(Context, Path, Kind, DescriptorCycle, Unsupported);
			if (Struct->HasIdentical())
				return Struct->GetOps().Identical(LeftValue, RightValue)
					? Identical : SetIdentityDiagnostic(Context, Path, Kind, ValueMismatch, Different);

			Context.ActiveStructs.push_back(Struct);
			EPropertyIdentityResult Result = Identical;
			Struct->ForEachProperty([&](FProperty* Field) {
				if (Result != Identical || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
				for (uint32 Index = 0; Index < Field->GetArrayDim(); ++Index)
				{
					const std::string Segment = std::format(
						".{}{}", Field->NamePrivate.ToString(),
						Field->GetArrayDim() > 1 ? std::format("[{}]", Index) : std::string{}
					);
					std::string ChildPath;
					if (!AppendIdentityPath(Context, Path, Segment, Field->GetKind(), ChildPath))
					{
						Result = Unsupported;
						break;
					}
					Result = ComparePropertyValuesImpl(
						Field, LeftValue, Index, RightValue, Index, Depth + 1, ChildPath, Context
					);
				}
			}, false);
			Context.ActiveStructs.pop_back();
			return Result;
		}

		auto ValidatePropertyIdentityDescriptorImpl(
			const FProperty* Property,
			uint32 Depth,
			std::string_view Path,
			FPropertyIdentityContext& Context
		) -> bool
		{
			using enum EPropertyIdentityReason;
			using enum EPropertyIdentityResult;
			const auto Kind = Property ? Property->GetKind() : DurinCodeGen::EPropertyGenFlags::None;
			if (!Property)
			{
				SetIdentityDiagnostic(Context, Path, Kind, InvalidInput, Unsupported);
				return false;
			}
			if (Depth > PropertyIdentityMaxDepth)
			{
				SetIdentityDiagnostic(Context, Path, Kind, DepthLimit, Unsupported);
				return false;
			}

			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
			case DurinCodeGen::EPropertyGenFlags::Enum:
			case DurinCodeGen::EPropertyGenFlags::String:
			case DurinCodeGen::EPropertyGenFlags::Name:
			case DurinCodeGen::EPropertyGenFlags::Guid:
			case DurinCodeGen::EPropertyGenFlags::Byte:
			case DurinCodeGen::EPropertyGenFlags::Blob:
			case DurinCodeGen::EPropertyGenFlags::BulkData:
			case DurinCodeGen::EPropertyGenFlags::Object:
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			case DurinCodeGen::EPropertyGenFlags::WeakObject:
				return true;
			case DurinCodeGen::EPropertyGenFlags::Struct:
				{
					const DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
					if (!Struct)
					{
						SetIdentityDiagnostic(Context, Path, Kind, MissingStructDescriptor, Unsupported);
						return false;
					}
					if (!Struct->HasCompleteAuthoredFields())
					{
						SetIdentityDiagnostic(Context, Path, Kind, IncompleteAuthoredFields, Unsupported);
						return false;
					}
					if (std::ranges::find(Context.ActiveStructs, Struct) != Context.ActiveStructs.end())
					{
						SetIdentityDiagnostic(Context, Path, Kind, DescriptorCycle, Unsupported);
						return false;
					}
					if (Struct->HasIdentical()) return true;
					Context.ActiveStructs.push_back(Struct);
					bool bSupported = true;
					Struct->ForEachProperty([&](FProperty* Field) {
						if (!bSupported || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
						std::string ChildPath;
						const std::string Segment = std::format(".{}", Field->NamePrivate);
						bSupported = AppendIdentityPath(Context, Path, Segment, Field->GetKind(), ChildPath)
							&& ValidatePropertyIdentityDescriptorImpl(Field, Depth + 1, ChildPath, Context);
					}, false);
					Context.ActiveStructs.pop_back();
					return bSupported;
				}
			case DurinCodeGen::EPropertyGenFlags::Array:
				{
					const auto* Array = static_cast<const FArrayProperty*>(Property);
					if (!Array->GetInner())
					{
						SetIdentityDiagnostic(Context, Path, Kind, MissingArrayDescriptor, Unsupported);
						return false;
					}
					if (!Array->HasArrayOps()
						|| !Array->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess))
					{
						SetIdentityDiagnostic(Context, Path, Kind, MissingArrayOperations, Unsupported);
						return false;
					}
					std::string ChildPath;
					return AppendIdentityPath(Context, Path, "[]", Array->GetInner()->GetKind(), ChildPath)
						&& ValidatePropertyIdentityDescriptorImpl(Array->GetInner(), Depth + 1, ChildPath, Context);
				}
			case DurinCodeGen::EPropertyGenFlags::Map:
				{
					const auto* Map = static_cast<const FMapProperty*>(Property);
					if (!Map->GetKeyProp() || !Map->GetValueProp())
					{
						SetIdentityDiagnostic(Context, Path, Kind, MissingMapDescriptor, Unsupported);
						return false;
					}
					if (!Map->HasMapOps()
						|| !Map->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal | EMapOpsFlags::Lookup))
					{
						SetIdentityDiagnostic(Context, Path, Kind, MissingMapOperations, Unsupported);
						return false;
					}
					if (!ValidateCanonicalMapKeyProperty(Map->GetKeyProp()))
					{
						SetIdentityDiagnostic(Context, Path, Kind, UnsupportedMapKey, Unsupported);
						return false;
					}
					std::string ChildPath;
					return AppendIdentityPath(Context, Path, "{}", Map->GetValueProp()->GetKind(), ChildPath)
						&& ValidatePropertyIdentityDescriptorImpl(Map->GetValueProp(), Depth + 1, ChildPath, Context);
				}
			default:
				SetIdentityDiagnostic(Context, Path, Kind, UnsupportedLogicalKind, Unsupported);
				return false;
			}
		}
	} // namespace

	IMPLEMENT_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FNumericProperty, FProperty, EClassCastFlags::FNumericProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FBoolProperty, FProperty, EClassCastFlags::FBoolProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FStringProperty, FProperty, EClassCastFlags::FStringProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FNameProperty, FProperty, EClassCastFlags::FNameProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FGuidProperty, FProperty, EClassCastFlags::FGuidProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FEnumProperty, FProperty, EClassCastFlags::FEnumProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FObjectProperty, FProperty, EClassCastFlags::FObjectProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FSoftObjectProperty, FProperty, EClassCastFlags::FSoftObjectProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FWeakObjectProperty, FProperty, EClassCastFlags::FWeakObjectProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FStructProperty, FProperty, EClassCastFlags::FStructProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FArrayProperty, FProperty, EClassCastFlags::FArrayProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FMapProperty, FProperty, EClassCastFlags::FMapProperty, COREDOBJECT_API)

	FProperty::FProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(
			  InOwner,
			  InName,
			  InObjectFlags,
			  EPropertyFlags::None,
			  1,
			  0,
			  0,
			  DurinCodeGen::EPropertyGenFlags::None,
			  nullptr
		  )
	{
	}

	FProperty::FProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass,
		bool bInIsObjectPtrWrapper
	)
		: FField(InOwner, InName, InObjectFlags)
		, PropertyFlags(InPropertyFlags)
		, ArrayDim(InArrayDim)
		, Offset(InOffset)
		, ElementSize(InElementSize)
		, Kind(InKind)
		, ReferencedClass(InReferencedClass)
		, bIsObjectPtrWrapper(bInIsObjectPtrWrapper)
	{
		ClassPrivate = StaticClass();
	}

	auto FProperty::SetLegacyNames(std::span<const char* const> InLegacyNames) -> void
	{
		LegacyNames.clear();
		LegacyNames.reserve(InLegacyNames.size());
		for (const char* LegacyName : InLegacyNames)
		{
			const FName Name(LegacyName ? LegacyName : "");
			check(!Name.IsNone());
			check(Name != NamePrivate && "A property legacy name must differ from its current name.");
			check(std::ranges::find(LegacyNames, Name) == LegacyNames.end()
				&& "Property legacy names must be unique.");
			LegacyNames.push_back(Name);
		}
	}

	auto FProperty::GetValueSize() const -> uint32
	{
		if (ValueSize != 0) return ValueSize;
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->PropertiesSize;
		return ElementSize;
	}

	auto FProperty::GetValueAlignment() const -> uint32
	{
		if (ValueAlignment != 0) return ValueAlignment;
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->MinAlignment;
		switch (Kind)
		{
		case DurinCodeGen::EPropertyGenFlags::Bool: return alignof(bool);
		case DurinCodeGen::EPropertyGenFlags::Int8: return alignof(int8);
		case DurinCodeGen::EPropertyGenFlags::Int16: return alignof(int16);
		case DurinCodeGen::EPropertyGenFlags::Int32: return alignof(int32);
		case DurinCodeGen::EPropertyGenFlags::Int64: return alignof(int64);
		case DurinCodeGen::EPropertyGenFlags::UInt8: return alignof(uint8);
		case DurinCodeGen::EPropertyGenFlags::UInt16: return alignof(uint16);
		case DurinCodeGen::EPropertyGenFlags::UInt32: return alignof(uint32);
		case DurinCodeGen::EPropertyGenFlags::UInt64: return alignof(uint64);
		case DurinCodeGen::EPropertyGenFlags::Float: return alignof(float);
		case DurinCodeGen::EPropertyGenFlags::Double: return alignof(double);
		case DurinCodeGen::EPropertyGenFlags::Enum: return std::bit_floor<uint32>(std::max<uint32>(1, ElementSize));
		case DurinCodeGen::EPropertyGenFlags::String: return alignof(std::string);
		case DurinCodeGen::EPropertyGenFlags::Name: return alignof(FName);
		case DurinCodeGen::EPropertyGenFlags::Guid: return alignof(FGuid);
		case DurinCodeGen::EPropertyGenFlags::Object:
			return bIsObjectPtrWrapper ? alignof(FObjectPtr) : alignof(DObject*);
		default: return 0;
		}
	}

	auto FProperty::CanDefaultConstructValue() const -> bool
	{
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->CanDefaultConstruct();
		return InitializeValueFunction != nullptr || HasBuiltInValueLifecycle(this);
	}

	auto FProperty::CanDestroyValue() const -> bool
	{
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->CanDestroy();
		return DestroyValueFunction != nullptr || HasBuiltInValueLifecycle(this);
	}

	auto FProperty::CanCopyConstructValue() const -> bool
	{
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->CanCopyConstruct();
		return CopyConstructValueFunction != nullptr;
	}

	auto FProperty::CanCopyAssignValue() const -> bool
	{
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->CanCopyAssign();
		return CopyAssignValueFunction != nullptr;
	}

	auto FProperty::InitializeValue(void* Memory) const -> FPropertyValueResult
	{
		if (!Memory) return PropertyValueFailure(this, EPropertyValueError::NullValue, EPropertyValueOperation::DefaultConstruct);
		if (!CanDefaultConstructValue() || !CanDestroyValue())
			return PropertyValueFailure(this, EPropertyValueError::UnavailableOperation, EPropertyValueOperation::DefaultConstruct);
		if (DStruct* Struct = GetPropertyStruct(this))
		{
			Struct->GetOps().DefaultConstruct(Memory);
		}
		else if (InitializeValueFunction)
		{
			InitializeValueFunction(Memory);
		}
		else
		{
			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::String: std::construct_at(static_cast<std::string*>(Memory)); break;
			case DurinCodeGen::EPropertyGenFlags::Name: std::construct_at(static_cast<FName*>(Memory)); break;
			case DurinCodeGen::EPropertyGenFlags::Guid: std::construct_at(static_cast<FGuid*>(Memory)); break;
			case DurinCodeGen::EPropertyGenFlags::Object:
				if (bIsObjectPtrWrapper)
					std::construct_at(static_cast<FObjectPtr*>(Memory));
				else
					std::construct_at(static_cast<DObject**>(Memory), nullptr);
				break;
			default: std::memset(Memory, 0, GetValueSize()); break;
			}
		}
		return {};
	}

	auto FProperty::DestroyValue(void* Memory) const -> void
	{
		if (!Memory) return;
		if (DStruct* Struct = GetPropertyStruct(this))
		{
			if (Struct->NeedsDestroy()) Struct->GetOps().Destroy(Memory);
		}
		else if (DestroyValueFunction)
		{
			DestroyValueFunction(Memory);
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::String)
		{
			std::destroy_at(static_cast<std::string*>(Memory));
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Name)
		{
			std::destroy_at(static_cast<FName*>(Memory));
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Guid)
		{
			std::destroy_at(static_cast<FGuid*>(Memory));
		}
		else if (Kind == DurinCodeGen::EPropertyGenFlags::Object && bIsObjectPtrWrapper)
		{
			std::destroy_at(static_cast<FObjectPtr*>(Memory));
		}
	}

	auto FProperty::CopyConstructValue(void* Destination, const void* Source) const -> FPropertyValueResult
	{
		if (!Destination || !Source) return PropertyValueFailure(this, EPropertyValueError::NullValue, EPropertyValueOperation::CopyConstruct);
		if (!CanCopyConstructValue() || !CanDestroyValue())
			return PropertyValueFailure(this, EPropertyValueError::UnavailableOperation, EPropertyValueOperation::CopyConstruct);
		try
		{
			if (DStruct* Struct = GetPropertyStruct(this))
				Struct->GetOps().CopyConstruct(Destination, Source);
			else
				CopyConstructValueFunction(Destination, Source);
		}
		catch (...)
		{
			return PropertyValueFailure(this, EPropertyValueError::CopyFailed, EPropertyValueOperation::CopyConstruct);
		}
		return {};
	}

	auto FProperty::CopyAssignValue(void* Destination, const void* Source) const -> FPropertyValueResult
	{
		if (!Destination || !Source) return PropertyValueFailure(this, EPropertyValueError::NullValue, EPropertyValueOperation::CopyAssign);
		if (!CanCopyAssignValue())
			return PropertyValueFailure(this, EPropertyValueError::UnavailableOperation, EPropertyValueOperation::CopyAssign);
		try
		{
			if (DStruct* Struct = GetPropertyStruct(this))
				Struct->GetOps().CopyAssign(Destination, Source);
			else
				CopyAssignValueFunction(Destination, Source);
		}
		catch (...)
		{
			return PropertyValueFailure(this, EPropertyValueError::CopyFailed, EPropertyValueOperation::CopyAssign);
		}
		return {};
	}

	FReflectedValueStorage::~FReflectedValueStorage()
	{
		Reset();
	}

	FReflectedValueStorage::FReflectedValueStorage(FReflectedValueStorage&& Other) noexcept
		: Property(Other.Property)
		, ArrayIndex(Other.ArrayIndex)
		, Memory(Other.Memory)
		, Value(Other.Value)
		, Alignment(Other.Alignment)
		, bLive(Other.bLive)
	{
		Other.Property = nullptr;
		Other.Memory = nullptr;
		Other.Value = nullptr;
		Other.bLive = false;
	}

	auto FReflectedValueStorage::operator=(FReflectedValueStorage&& Other) noexcept -> FReflectedValueStorage&
	{
		if (this == &Other) return *this;
		Reset();
		Property = Other.Property;
		ArrayIndex = Other.ArrayIndex;
		Memory = Other.Memory;
		Value = Other.Value;
		Alignment = Other.Alignment;
		bLive = Other.bLive;
		Other.Property = nullptr;
		Other.Memory = nullptr;
		Other.Value = nullptr;
		Other.bLive = false;
		return *this;
	}

	auto FReflectedValueStorage::Allocate(const FProperty* InProperty, uint32 InArrayIndex) -> FPropertyValueResult
	{
		Property = InProperty;
		using C = EPropertyValueError;
		constexpr auto Op = EPropertyValueOperation::Storage;
		if (!Property) return Fail(C::NullProperty, Op, InArrayIndex);
		if (Property->HasValueAccessors()) return Fail(C::CustomAccessorStorage, Op, InArrayIndex);
		if (InArrayIndex >= Property->GetArrayDim()) return Fail(C::InvalidArrayIndex, Op, InArrayIndex);
		if (Property->GetValueSize() == 0 || Property->GetValueAlignment() == 0)
			return Fail(C::InvalidLayout, Op, InArrayIndex);
		ArrayIndex = InArrayIndex;
		Alignment = std::max<size_t>(InProperty->GetValueAlignment(), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		const size_t Size = std::max<size_t>(1, static_cast<size_t>(InProperty->GetOffset()) + static_cast<size_t>(InProperty->GetElementSize()) * static_cast<size_t>(InArrayIndex) + static_cast<size_t>(InProperty->GetValueSize()));
		Memory = ::operator new(Size, std::align_val_t(Alignment));
		Value = InProperty->GetValuePtr(Memory, InArrayIndex);
		return {};
	}

	auto FReflectedValueStorage::DefaultConstruct(const FProperty* InProperty, uint32 InArrayIndex) -> FPropertyValueResult
	{
		using C = EPropertyValueError;
		constexpr auto Op = EPropertyValueOperation::DefaultConstruct;
		if (Memory || bLive) return Fail(C::StorageAlreadyLive, Op, InArrayIndex);
		Property = InProperty;
		if (!Property) return Fail(C::NullProperty, Op, InArrayIndex);
		if (!Property->CanDefaultConstructValue() || !Property->CanDestroyValue())
			return Fail(C::UnavailableOperation, Op, InArrayIndex);
		if (auto Result = Allocate(InProperty, InArrayIndex); !Result) return Result;
		if (auto Result = Property->InitializeValue(Value); !Result)
		{
			Result.Error.ArrayIndex = InArrayIndex;
			Reset();
			return Result;
		}
		bLive = true;
		return {};
	}

	auto FReflectedValueStorage::CopyConstruct(const FProperty* InProperty, const void* SourceValue,
		uint32 InArrayIndex) -> FPropertyValueResult
	{
		using C = EPropertyValueError;
		constexpr auto Op = EPropertyValueOperation::CopyConstruct;
		if (Memory || bLive) return Fail(C::StorageAlreadyLive, Op, InArrayIndex);
		Property = InProperty;
		if (!Property) return Fail(C::NullProperty, Op, InArrayIndex);
		if (!SourceValue) return Fail(C::NullValue, Op, InArrayIndex);
		if (!Property->CanCopyConstructValue() || !Property->CanDestroyValue())
			return Fail(C::UnavailableOperation, Op, InArrayIndex);
		if (auto Result = Allocate(InProperty, InArrayIndex); !Result) return Result;
		if (auto Result = Property->CopyConstructValue(Value, SourceValue); !Result)
		{
			Result.Error.ArrayIndex = InArrayIndex;
			Reset();
			return Result;
		}
		bLive = true;
		return {};
	}

	auto FReflectedValueStorage::CopyAssign(const void* SourceValue) -> FPropertyValueResult
	{
		using C = EPropertyValueError;
		constexpr auto Op = EPropertyValueOperation::CopyAssign;
		if (!bLive || !Property) return Fail(C::StorageNotLive, Op, ArrayIndex);
		if (!SourceValue) return Fail(C::NullValue, Op, ArrayIndex);
		if (!Property->CanCopyAssignValue()) return Fail(C::UnavailableOperation, Op, ArrayIndex);
		auto Result = Property->CopyAssignValue(Value, SourceValue);
		if (!Result) Result.Error.ArrayIndex = ArrayIndex;
		return Result;
	}

	auto FReflectedValueStorage::Reset() -> void
	{
		if (bLive && Property) Property->DestroyValue(Value);
		if (Memory) ::operator delete(Memory, std::align_val_t(Alignment));
		Property = nullptr;
		ArrayIndex = 0;
		Memory = nullptr;
		Value = nullptr;
		Alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		bLive = false;
	}

	auto FReflectedValueStorage::Fail(EPropertyValueError Code, EPropertyValueOperation Operation,
		uint32 RequestedIndex) const -> FPropertyValueResult
	{
		return PropertyValueFailure(Property, Code, Operation, RequestedIndex);
	}

	auto FormatPropertyValueError(const FPropertyValueError& Error) -> std::string
	{
		if (!Error.HasError()) return {};
		if (Error.Code == EPropertyValueError::CopyFailed)
			return Error.Operation == EPropertyValueOperation::CopyConstruct
				? "ReflectedValueCopyFailed: copy construction failed."
				: "ReflectedValueCopyFailed: copy assignment failed.";
		std::string_view Operation;
		switch (Error.Operation)
		{
		case EPropertyValueOperation::Storage: Operation = "Storage"; break;
		case EPropertyValueOperation::DefaultConstruct: Operation = "DefaultConstruct"; break;
		case EPropertyValueOperation::CopyConstruct: Operation = "CopyConstruct"; break;
		case EPropertyValueOperation::CopyAssign: Operation = "CopyAssign"; break;
		}
		if (!Error.StructName.empty())
			return std::format("DStructOperationUnavailable: {} is unavailable for '{}'.", Operation, Error.StructName);
		return std::format("ReflectedValueOperationUnavailable: {} is unavailable for property '{}'.",
			Operation, Error.PropertyName.empty() ? "<null>" : Error.PropertyName);
	}

	auto ComparePropertyValues(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex,
		FPropertyIdentityDiagnostic* OutDiagnostic
	) -> EPropertyIdentityResult
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FPropertyIdentityContext Context{OutDiagnostic, nullptr};
		std::string Path = Property ? Property->NamePrivate.ToString() : std::string("<null>");
		if (Property && Property->GetArrayDim() > 1) Path += std::format("[{}]", LeftArrayIndex);
		if (Path.size() > PropertyIdentityMaxPathLength)
			return SetIdentityDiagnostic(
				Context, std::string_view(Path).substr(0, PropertyIdentityMaxPathLength),
				Property ? Property->GetKind() : DurinCodeGen::EPropertyGenFlags::None,
				EPropertyIdentityReason::DiagnosticPathLimit, EPropertyIdentityResult::Unsupported
			);
		return ComparePropertyValuesImpl(
			Property, LeftContainer, LeftArrayIndex, RightContainer, RightArrayIndex, 0, Path, Context
		);
	}

	auto ComparePropertyValuesWithDefaultGraph(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex,
		const FDefaultObjectGraphMap& DefaultGraph,
		FPropertyIdentityDiagnostic* OutDiagnostic
	) -> EPropertyIdentityResult
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FPropertyIdentityContext Context{OutDiagnostic, &DefaultGraph};
		std::string Path = Property ? Property->NamePrivate.ToString() : std::string("<null>");
		if (Property && Property->GetArrayDim() > 1) Path += std::format("[{}]", LeftArrayIndex);
		if (Path.size() > PropertyIdentityMaxPathLength)
			return SetIdentityDiagnostic(
				Context, std::string_view(Path).substr(0, PropertyIdentityMaxPathLength),
				Property ? Property->GetKind() : DurinCodeGen::EPropertyGenFlags::None,
				EPropertyIdentityReason::DiagnosticPathLimit, EPropertyIdentityResult::Unsupported
			);
		return ComparePropertyValuesImpl(
			Property, LeftContainer, LeftArrayIndex, RightContainer, RightArrayIndex, 0, Path, Context
		);
	}

	auto CompareObjectPropertyToClassDefault(
		const FProperty* Property,
		const DObject* LiveObject,
		uint32 ArrayIndex,
		const FDefaultObjectGraphMap& DefaultGraph,
		FPropertyIdentityDiagnostic* OutDiagnostic
	) -> EPropertyIdentityResult
	{
		DClass* PropertyOwner = Property ? Cast<DClass>(Property->Owner.ToDObject()) : nullptr;
		if (!Property || !LiveObject || !LiveObject->GetClass() || !PropertyOwner
			|| !LiveObject->GetClass()->IsChildOf(PropertyOwner)
			|| LiveObject->IsTemplateObject() || ArrayIndex >= Property->GetArrayDim())
		{
			if (OutDiagnostic)
			{
				OutDiagnostic->Reset();
				OutDiagnostic->PropertyPath = Property ? Property->NamePrivate.ToString() : "<null>";
				OutDiagnostic->LogicalKind = Property ? Property->GetKind() : DurinCodeGen::EPropertyGenFlags::None;
				OutDiagnostic->Reason = EPropertyIdentityReason::InvalidInput;
			}
			return EPropertyIdentityResult::Unsupported;
		}
		const DObject* DefaultObject = LiveObject->GetClass()->GetDefaultObject();
		if (!DefaultObject || DefaultGraph.FindInstance(DefaultObject) != LiveObject)
		{
			if (OutDiagnostic)
			{
				OutDiagnostic->Reset();
				OutDiagnostic->PropertyPath = Property->NamePrivate.ToString();
				OutDiagnostic->LogicalKind = Property->GetKind();
				OutDiagnostic->Reason = EPropertyIdentityReason::InvalidInput;
			}
			return EPropertyIdentityResult::Unsupported;
		}
		return ComparePropertyValuesWithDefaultGraph(
			Property, DefaultObject, ArrayIndex, LiveObject, ArrayIndex, DefaultGraph, OutDiagnostic
		);
	}

	auto CompareStructPropertyToTypeDefault(
		const FStructProperty* Property,
		const void* LiveContainer,
		uint32 ArrayIndex,
		FPropertyIdentityDiagnostic* OutDiagnostic
	) -> EPropertyIdentityResult
	{
		DStruct* Struct = Property ? Property->GetStruct() : nullptr;
		const void* DefaultValue = Struct ? Struct->GetDefaultValue() : nullptr;
		if (!Property || !LiveContainer || ArrayIndex >= Property->GetArrayDim() || !DefaultValue)
		{
			if (OutDiagnostic)
			{
				OutDiagnostic->Reset();
				OutDiagnostic->PropertyPath = Property ? Property->NamePrivate.ToString() : "<null>";
				OutDiagnostic->LogicalKind = DurinCodeGen::EPropertyGenFlags::Struct;
				OutDiagnostic->Reason = EPropertyIdentityReason::InvalidInput;
			}
			return EPropertyIdentityResult::Unsupported;
		}
		return CompareStructValues(
			Struct, Property->GetValuePtr(LiveContainer, ArrayIndex), DefaultValue, OutDiagnostic
		);
	}

	auto CompareStructValues(
		const DStruct* Struct,
		const void* LeftValue,
		const void* RightValue,
		FPropertyIdentityDiagnostic* OutDiagnostic
	) -> EPropertyIdentityResult
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FPropertyIdentityContext Context{OutDiagnostic, nullptr};
		const std::string Path = Struct ? Struct->GetQualifiedName().ToString() : std::string("<null>");
		return CompareStructValuesImpl(Struct, LeftValue, RightValue, 0, Path, Context);
	}

	auto ValidatePropertyIdentityDescriptor(
		const FProperty* Property,
		FPropertyIdentityDiagnostic* OutDiagnostic
	) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FPropertyIdentityContext Context{OutDiagnostic, nullptr};
		const std::string Path = Property ? Property->NamePrivate.ToString() : std::string("<null>");
		return ValidatePropertyIdentityDescriptorImpl(Property, 0, Path, Context);
	}

	auto ArePropertyValuesIdentical(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex
	) -> bool
	{
		return ComparePropertyValues(
			Property, LeftContainer, LeftArrayIndex, RightContainer, RightArrayIndex
		) == EPropertyIdentityResult::Identical;
	}

	FNumericProperty::FNumericProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FNumericProperty::FNumericProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
	}

	FBoolProperty::FBoolProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FBoolProperty::FBoolProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
	}

	FStringProperty::FStringProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FStringProperty::FStringProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
	}

	FNameProperty::FNameProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FNameProperty::FNameProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
	}

	FGuidProperty::FGuidProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FGuidProperty::FGuidProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
	}

	FEnumProperty::FEnumProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FEnumProperty::FEnumProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass,
		DEnum* InReferencedEnum
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
		, ReferencedEnum(InReferencedEnum)
	{
		ClassPrivate = StaticClass();
	}

	auto FEnumProperty::GetUnderlyingType() const -> DurinCodeGen::EEnumUnderlyingType
	{
		return ReferencedEnum ? ReferencedEnum->GetUnderlyingType() : DurinCodeGen::EEnumUnderlyingType::Unknown;
	}

	auto FEnumProperty::GetValueAsUInt64(const void* Container, uint32 ArrayIndex) const -> uint64
	{
		const void* ValuePtr = GetValuePtr(Container, ArrayIndex);
		switch (GetUnderlyingType())
		{
		case DurinCodeGen::EEnumUnderlyingType::Int8: return ReadEnumValue<int8>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::Int16: return ReadEnumValue<int16>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::Int32: return ReadEnumValue<int32>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::Int64: return ReadEnumValue<int64>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::UInt8: return ReadEnumValue<uint8>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::UInt16: return ReadEnumValue<uint16>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::UInt32: return ReadEnumValue<uint32>(ValuePtr);
		case DurinCodeGen::EEnumUnderlyingType::UInt64: return ReadEnumValue<uint64>(ValuePtr);
		default: return 0;
		}
	}

	auto FEnumProperty::SetValueFromUInt64(void* Container, uint64 Value, uint32 ArrayIndex) const -> void
	{
		void* ValuePtr = GetValuePtr(Container, ArrayIndex);
		switch (GetUnderlyingType())
		{
		case DurinCodeGen::EEnumUnderlyingType::Int8:
		case DurinCodeGen::EEnumUnderlyingType::UInt8:
			WriteEnumValue<uint8>(ValuePtr, Value);
			break;
		case DurinCodeGen::EEnumUnderlyingType::Int16:
		case DurinCodeGen::EEnumUnderlyingType::UInt16:
			WriteEnumValue<uint16>(ValuePtr, Value);
			break;
		case DurinCodeGen::EEnumUnderlyingType::Int32:
		case DurinCodeGen::EEnumUnderlyingType::UInt32:
			WriteEnumValue<uint32>(ValuePtr, Value);
			break;
		case DurinCodeGen::EEnumUnderlyingType::Int64:
		case DurinCodeGen::EEnumUnderlyingType::UInt64:
			WriteEnumValue<uint64>(ValuePtr, Value);
			break;
		default: break;
		}
	}

	FObjectProperty::FObjectProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FObjectProperty::FObjectProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass,
		bool bInIsObjectPtrWrapper,
		FReadObjectValue InReadObjectValue,
		FWriteObjectValue InWriteObjectValue
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass, bInIsObjectPtrWrapper)
		, ReadObjectValue(InReadObjectValue)
		, WriteObjectValue(InWriteObjectValue)
	{
		ClassPrivate = StaticClass();
	}

	auto FObjectProperty::GetObjectPropertyValue(const void* Container, uint32 ArrayIndex) const -> DObject*
	{
		return ReadObjectValue ? ReadObjectValue(GetValuePtr(Container, ArrayIndex)) : nullptr;
	}

	auto FObjectProperty::SetObjectPropertyValue(void* Container, DObject* Value, uint32 ArrayIndex) const -> void
	{
		if (WriteObjectValue) WriteObjectValue(GetValuePtr(Container, ArrayIndex), Value);
	}

	FSoftObjectProperty::FSoftObjectProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FSoftObjectProperty::FSoftObjectProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DClass* InExpectedClass,
		FMutableSoftValueAccessor InMutableSoftValueAccessor,
		FConstSoftValueAccessor InConstSoftValueAccessor
	)
		: FProperty(
			  InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, DurinCodeGen::EPropertyGenFlags::SoftObject, InExpectedClass
		  )
		, MutableSoftValueAccessor(InMutableSoftValueAccessor)
		, ConstSoftValueAccessor(InConstSoftValueAccessor)
	{
		ClassPrivate = StaticClass();
	}

	auto FSoftObjectProperty::GetSoftObjectPtr(void* Container, uint32 ArrayIndex) const -> FSoftObjectPtr*
	{
		return Container && MutableSoftValueAccessor ? MutableSoftValueAccessor(GetValuePtr(Container, ArrayIndex)) : nullptr;
	}

	auto FSoftObjectProperty::GetSoftObjectPtr(const void* Container, uint32 ArrayIndex) const -> const FSoftObjectPtr*
	{
		return Container && ConstSoftValueAccessor ? ConstSoftValueAccessor(GetValuePtr(Container, ArrayIndex)) : nullptr;
	}

	FWeakObjectProperty::FWeakObjectProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FWeakObjectProperty::FWeakObjectProperty(
		FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags, uint16 InArrayDim, uint16 InOffset,
		uint16 InElementSize, DClass* InExpectedClass,
		FMutableWeakValueAccessor InMutableWeakValueAccessor,
		FConstWeakValueAccessor InConstWeakValueAccessor)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset,
			InElementSize, DurinCodeGen::EPropertyGenFlags::WeakObject, InExpectedClass)
		, MutableWeakValueAccessor(InMutableWeakValueAccessor)
		, ConstWeakValueAccessor(InConstWeakValueAccessor)
	{
		ClassPrivate = StaticClass();
	}

	auto FWeakObjectProperty::GetWeakObjectPtr(void* Container, uint32 ArrayIndex) const -> FWeakObjectPtr*
	{
		return Container && MutableWeakValueAccessor ? MutableWeakValueAccessor(GetValuePtr(Container, ArrayIndex)) : nullptr;
	}

	auto FWeakObjectProperty::GetWeakObjectPtr(const void* Container, uint32 ArrayIndex) const -> const FWeakObjectPtr*
	{
		return Container && ConstWeakValueAccessor ? ConstWeakValueAccessor(GetValuePtr(Container, ArrayIndex)) : nullptr;
	}

	FStructProperty::FStructProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FStructProperty::FStructProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		DStruct* InStruct
	)
		: FProperty(
			  InOwner,
			  InName,
			  InObjectFlags,
			  InPropertyFlags,
			  InArrayDim,
			  InOffset,
			  static_cast<uint16>(InStruct->PropertiesSize),
			  DurinCodeGen::EPropertyGenFlags::Struct,
			  nullptr
		  )
		, Struct(InStruct)
	{
		ClassPrivate = StaticClass();
	}

	FArrayProperty::FArrayProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FArrayProperty::FArrayProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass,
		const FArrayOps* InOps
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
		, Ops(InOps)
	{
		check(IsValidArrayOps(Ops));
		ClassPrivate = StaticClass();
	}

	auto FArrayProperty::GetNum(const void* Container, uint64& OutNum, uint32 ArrayIndex) const -> EContainerOpResult
	{
		OutNum = 0;
		if (!Container) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EArrayOpsFlags::Count)) return EContainerOpResult::Unsupported;
		OutNum = Ops->Num(GetValuePtr(Container, ArrayIndex));
		return EContainerOpResult::Success;
	}

	auto FArrayProperty::VisitElements(const void* Container, FArrayConstVisitor Visitor, void* Context, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !Visitor) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EArrayOpsFlags::ConstTraversal)) return EContainerOpResult::Unsupported;
		return Ops->VisitConst(GetValuePtr(Container, ArrayIndex), Visitor, Context);
	}

	auto FArrayProperty::VisitMutableElements(void* Container, FArrayMutableVisitor Visitor, void* Context, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !Visitor) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EArrayOpsFlags::MutableTraversal)) return EContainerOpResult::Unsupported;
		return Ops->VisitMutable(GetValuePtr(Container, ArrayIndex), Visitor, Context);
	}

	auto FArrayProperty::GetElement(const void* Container, uint64 Index, const void** OutElement, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (OutElement) *OutElement = nullptr;
		if (!Container || !OutElement) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EArrayOpsFlags::RandomAccess)) return EContainerOpResult::Unsupported;
		return Ops->GetConstAt(GetValuePtr(Container, ArrayIndex), Index, OutElement);
	}

	auto FArrayProperty::GetMutableElement(void* Container, uint64 Index, void** OutElement, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (OutElement) *OutElement = nullptr;
		if (!Container || !OutElement) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EArrayOpsFlags::RandomAccess)) return EContainerOpResult::Unsupported;
		return Ops->GetMutableAt(GetValuePtr(Container, ArrayIndex), Index, OutElement);
	}

	auto FArrayProperty::ResizeChecked(void* Container, uint64 Num, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container) return EContainerOpResult::InvalidInput;
		uint64 CurrentNum = 0;
		const EContainerOpResult CountResult = GetNum(Container, CurrentNum, ArrayIndex);
		if (CountResult != EContainerOpResult::Success) return CountResult;
		if (Num < CurrentNum && !HasCapability(EArrayOpsFlags::Shrink)) return EContainerOpResult::Unsupported;
		if (Num > CurrentNum && !HasCapability(EArrayOpsFlags::DefaultGrow)) return EContainerOpResult::Unsupported;
		if (Num == CurrentNum) return EContainerOpResult::Success;
		return Ops->Resize(GetValuePtr(Container, ArrayIndex), Num);
	}

	auto FArrayProperty::Num(const void* Container, uint32 ArrayIndex) const -> uint64
	{
		uint64 Result = 0;
		requiref(GetNum(Container, Result, ArrayIndex) == EContainerOpResult::Success,
			"Array Count capability is unavailable.");
		return Result;
	}

	auto FArrayProperty::GetElementPtr(const void* Container, uint64 Index, uint32 ArrayIndex) const -> const void*
	{
		const void* Result = nullptr;
		requiref(GetElement(Container, Index, &Result, ArrayIndex) == EContainerOpResult::Success,
			"Array random access failed.");
		return Result;
	}

	auto FArrayProperty::GetMutableElementPtr(void* Container, uint64 Index, uint32 ArrayIndex) const -> void*
	{
		void* Result = nullptr;
		requiref(GetMutableElement(Container, Index, &Result, ArrayIndex) == EContainerOpResult::Success,
			"Array mutable random access failed.");
		return Result;
	}

	auto FormatPropertyContainerError(const FPropertyContainerError& Error) -> std::string
	{
		if (Error.Code == EContainerOpResult::Success) return {};
		std::string_view Requirement;
		switch (Error.Requirement)
		{
		case EPropertyContainerRequirement::DefaultConstruct: Requirement = "DefaultConstruct"; break;
		case EPropertyContainerRequirement::Destroy: Requirement = "Destroy"; break;
		case EPropertyContainerRequirement::CopyConstruct: Requirement = "CopyConstruct"; break;
		case EPropertyContainerRequirement::CopyAssign: Requirement = "CopyAssign"; break;
		default: break;
		}
		if (!Requirement.empty())
			return Error.StructName.empty()
				? std::format("ReflectedValueOperationUnavailable: {} is unavailable for property '{}'.", Requirement, Error.ValuePropertyName)
				: std::format("DStructOperationUnavailable: {} is unavailable for '{}'.", Requirement, Error.StructName);
		return std::format("Reflected container '{}' operation failed, code={}.", Error.PropertyName, static_cast<uint32>(Error.Code));
	}

	auto FArrayProperty::Resize(void* Container, uint64 Num, uint32 ArrayIndex) const -> FPropertyContainerResult
	{
		using R = EPropertyContainerRequirement;
		auto Fail = [&](EContainerOpResult Code, R Requirement = R::None) {
			auto Result = ContainerFailure(this, EPropertyContainerOperation::Resize, ArrayIndex, Code, Inner, Requirement);
			Result.Error.RequestedCount = Num;
			return Result;
		};
		if (!Container || !Inner) return Fail(EContainerOpResult::InvalidInput);
		if (ArrayIndex >= GetArrayDim()) return Fail(EContainerOpResult::OutOfRange);
		if (!HasArrayOps()) return Fail(EContainerOpResult::Unsupported);
		uint64 CurrentNum = 0;
		if (auto Code = GetNum(Container, CurrentNum, ArrayIndex); Code != EContainerOpResult::Success) return Fail(Code);
		FPropertyContainerResult Result;
		if (Num != CurrentNum && !Inner->CanDestroyValue()) Result = Fail(EContainerOpResult::Unsupported, R::Destroy);
		else if (Num > CurrentNum && !Inner->CanDefaultConstructValue()) Result = Fail(EContainerOpResult::Unsupported, R::DefaultConstruct);
		else if (auto Code = ResizeChecked(Container, Num, ArrayIndex); Code != EContainerOpResult::Success) Result = Fail(Code);
		if (!Result) Result.Error.CurrentCount = CurrentNum;
		return Result;
	}
	FMapProperty::FMapProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags)
		: FProperty(InOwner, InName, InObjectFlags)
	{
		ClassPrivate = StaticClass();
	}

	FMapProperty::FMapProperty(
		FFieldVariant InOwner,
		FName InName,
		EObjectFlags InObjectFlags,
		EPropertyFlags InPropertyFlags,
		uint16 InArrayDim,
		uint16 InOffset,
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DClass* InReferencedClass,
		const FMapOps* InOps
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
		, Ops(InOps)
	{
		check(IsValidMapOps(Ops));
		ClassPrivate = StaticClass();
	}

	auto FMapProperty::GetNum(const void* Container, uint64& OutNum, uint32 ArrayIndex) const -> EContainerOpResult
	{
		OutNum = 0;
		if (!Container) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::Count)) return EContainerOpResult::Unsupported;
		OutNum = Ops->Num(GetValuePtr(Container, ArrayIndex));
		return EContainerOpResult::Success;
	}
	auto FMapProperty::VisitEntries(const void* Container, FMapConstVisitor Visitor, void* Context, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !Visitor) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::ConstTraversal)) return EContainerOpResult::Unsupported;
		return Ops->VisitConst(GetValuePtr(Container, ArrayIndex), Visitor, Context);
	}
	auto FMapProperty::VisitMutableEntries(void* Container, FMapMutableVisitor Visitor, void* Context, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !Visitor) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::MutableMappedTraversal)) return EContainerOpResult::Unsupported;
		return Ops->VisitMutable(GetValuePtr(Container, ArrayIndex), Visitor, Context);
	}
	auto FMapProperty::FindValue(const void* Container, const void* Key, const void** OutValue, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (OutValue) *OutValue = nullptr;
		if (!Container || !Key || !OutValue) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::Lookup)) return EContainerOpResult::Unsupported;
		return Ops->Lookup(GetValuePtr(Container, ArrayIndex), Key, OutValue);
	}
	auto FMapProperty::FindMutableValue(void* Container, const void* Key, void** OutValue, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (OutValue) *OutValue = nullptr;
		if (!Container || !Key || !OutValue) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::MutableLookup)) return EContainerOpResult::Unsupported;
		return Ops->LookupMutable(GetValuePtr(Container, ArrayIndex), Key, OutValue);
	}
	auto FMapProperty::ClearChecked(void* Container, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::Clear)) return EContainerOpResult::Unsupported;
		return Ops->Clear(GetValuePtr(Container, ArrayIndex));
	}
	auto FMapProperty::InsertChecked(void* Container, const void* Key, const void* Value, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !Key || !Value) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::Insert)) return EContainerOpResult::Unsupported;
		return Ops->InsertCopy(GetValuePtr(Container, ArrayIndex), Key, Value);
	}
	auto FMapProperty::RenameKeyChecked(void* Container, const void* OldKey, const void* NewKey, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !OldKey || !NewKey) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::RenameKey)) return EContainerOpResult::Unsupported;
		return Ops->RenameKey(GetValuePtr(Container, ArrayIndex), OldKey, NewKey);
	}
	auto FMapProperty::RemoveChecked(void* Container, const void* Key, uint32 ArrayIndex) const -> EContainerOpResult
	{
		if (!Container || !Key) return EContainerOpResult::InvalidInput;
		if (!HasCapability(EMapOpsFlags::Remove)) return EContainerOpResult::Unsupported;
		return Ops->Remove(GetValuePtr(Container, ArrayIndex), Key);
	}
	auto FMapProperty::Num(const void* Container, uint32 ArrayIndex) const -> uint64
	{
		uint64 Result = 0;
		requiref(GetNum(Container, Result, ArrayIndex) == EContainerOpResult::Success,
			"Map Count capability is unavailable.");
		return Result;
	}
	auto FMapProperty::Clear(void* Container, uint32 ArrayIndex) const -> void
	{
		requiref(ClearChecked(Container, ArrayIndex) == EContainerOpResult::Success,
			"Map Clear capability is unavailable.");
	}
	auto FMapProperty::Insert(void* Container, const void* Key, const void* Value, uint32 ArrayIndex) const -> FPropertyContainerResult
	{
		using R = EPropertyContainerRequirement;
		auto Fail = [&](EContainerOpResult Code, const FProperty* ValueProperty = nullptr, R Requirement = R::None) {
			return ContainerFailure(this, EPropertyContainerOperation::Insert, ArrayIndex, Code, ValueProperty, Requirement);
		};
		if (!Container || !Key || !Value || !KeyProp || !ValueProp) return Fail(EContainerOpResult::InvalidInput);
		if (ArrayIndex >= GetArrayDim()) return Fail(EContainerOpResult::OutOfRange);
		if (!HasMapOps()) return Fail(EContainerOpResult::Unsupported);
		if (GetPropertyStruct(KeyProp) && !KeyProp->CanCopyConstructValue())
			return Fail(EContainerOpResult::Unsupported, KeyProp, R::CopyConstruct);
		if (GetPropertyStruct(ValueProp) && !ValueProp->CanCopyConstructValue())
			return Fail(EContainerOpResult::Unsupported, ValueProp, R::CopyConstruct);
		if (GetPropertyStruct(ValueProp) && !ValueProp->CanCopyAssignValue())
			return Fail(EContainerOpResult::Unsupported, ValueProp, R::CopyAssign);
		const auto Code = InsertChecked(Container, Key, Value, ArrayIndex);
		return Code == EContainerOpResult::Success ? FPropertyContainerResult{} : Fail(Code);
	}

	auto FMapProperty::Contains(const void* Container, const void* Key, uint32 ArrayIndex) const -> bool
	{
		const void* Value = nullptr;
		return FindValue(Container, Key, &Value, ArrayIndex) == EContainerOpResult::Success;
	}
	auto FMapProperty::RenameKey(void* Container, const void* OldKey, const void* NewKey, uint32 ArrayIndex) const -> FPropertyContainerResult
	{
		using R = EPropertyContainerRequirement;
		auto Fail = [&](EContainerOpResult Code, const FProperty* ValueProperty = nullptr, R Requirement = R::None) {
			return ContainerFailure(this, EPropertyContainerOperation::RenameKey, ArrayIndex, Code, ValueProperty, Requirement);
		};
		if (!Container || !OldKey || !NewKey || !KeyProp) return Fail(EContainerOpResult::InvalidInput);
		if (ArrayIndex >= GetArrayDim()) return Fail(EContainerOpResult::OutOfRange);
		if (!HasMapOps()) return Fail(EContainerOpResult::Unsupported);
		if (GetPropertyStruct(KeyProp) && !KeyProp->CanCopyConstructValue())
			return Fail(EContainerOpResult::Unsupported, KeyProp, R::CopyConstruct);
		if (GetPropertyStruct(KeyProp) && !KeyProp->CanCopyAssignValue())
			return Fail(EContainerOpResult::Unsupported, KeyProp, R::CopyAssign);
		const auto Code = RenameKeyChecked(Container, OldKey, NewKey, ArrayIndex);
		return Code == EContainerOpResult::Success ? FPropertyContainerResult{} : Fail(Code);
	}

	auto FMapProperty::Remove(void* Container, const void* Key, uint32 ArrayIndex) const -> bool { return RemoveChecked(Container, Key, ArrayIndex) == EContainerOpResult::Success; }

	namespace
	{
		auto FailCanonicalToken(EReflectedMapKeyError Code, const FProperty* Property,
			uint32 ArrayIndex = 0) -> FReflectedMapKeyResult
		{
			FReflectedMapKeyError Error;
			Error.Code = Code;
			Error.ArrayIndex = ArrayIndex;
			if (Property)
			{
				Error.Kind = Property->GetKind();
				Error.PropertyName = Property->NamePrivate.ToString();
				Error.ArrayDim = Property->GetArrayDim();
				Error.Route.push_back({Error.PropertyName, ArrayIndex});
			}
			return {std::move(Error)};
		}

		auto CanonicalKind(DurinCodeGen::EPropertyGenFlags Kind)
			-> std::optional<ObjectPackage::ECanonicalMapKeyKind>
		{
			using EPropertyKind = DurinCodeGen::EPropertyGenFlags;
			using ETokenKind = ObjectPackage::ECanonicalMapKeyKind;
			switch (Kind)
			{
			case EPropertyKind::Bool: return ETokenKind::Bool;
			case EPropertyKind::Int8: return ETokenKind::I8;
			case EPropertyKind::Int16: return ETokenKind::I16;
			case EPropertyKind::Int32: return ETokenKind::I32;
			case EPropertyKind::Int64: return ETokenKind::I64;
			case EPropertyKind::UInt8: return ETokenKind::U8;
			case EPropertyKind::UInt16: return ETokenKind::U16;
			case EPropertyKind::UInt32: return ETokenKind::U32;
			case EPropertyKind::UInt64: return ETokenKind::U64;
			case EPropertyKind::Float: return ETokenKind::F32;
			case EPropertyKind::Double: return ETokenKind::F64;
			case EPropertyKind::String: return ETokenKind::String;
			case EPropertyKind::Enum: return ETokenKind::Enum;
			case EPropertyKind::Struct: return ETokenKind::Struct;
			case EPropertyKind::Name: return ETokenKind::Name;
			case EPropertyKind::Guid: return ETokenKind::Guid;
			case EPropertyKind::Byte: return ETokenKind::Byte;
			default: return std::nullopt;
			}
		}

		auto AppendCanonicalProperty(const FProperty* Property, const void* Container,
			uint32 ArrayIndex, ObjectPackage::FCanonicalMapKeyWriter& Writer) -> FReflectedMapKeyResult
		{
			using EWidth = ObjectPackage::ECanonicalIntegerWidth;
			if (!Property) return FailCanonicalToken(EReflectedMapKeyError::NullProperty, Property, ArrayIndex);
			if (!Container) return FailCanonicalToken(EReflectedMapKeyError::NullContainer, Property, ArrayIndex);
			if (ArrayIndex >= Property->GetArrayDim())
				return FailCanonicalToken(EReflectedMapKeyError::InvalidArrayIndex, Property, ArrayIndex);
			const auto Tag = CanonicalKind(Property->GetKind());
			if (!Tag)
				return FailCanonicalToken(EReflectedMapKeyError::UnsupportedKind, Property, ArrayIndex);
			Writer.WriteType(*Tag);
			const void* Value = Property->GetValuePtr(Container, ArrayIndex);
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool: Writer.WriteBool(*static_cast<const bool*>(Value)); return {};
			case DurinCodeGen::EPropertyGenFlags::Int8: Writer.WriteSigned(*static_cast<const int8*>(Value), EWidth::One); return {};
			case DurinCodeGen::EPropertyGenFlags::Int16: Writer.WriteSigned(*static_cast<const int16*>(Value), EWidth::Two); return {};
			case DurinCodeGen::EPropertyGenFlags::Int32: Writer.WriteSigned(*static_cast<const int32*>(Value), EWidth::Four); return {};
			case DurinCodeGen::EPropertyGenFlags::Int64: Writer.WriteSigned(*static_cast<const int64*>(Value), EWidth::Eight); return {};
			case DurinCodeGen::EPropertyGenFlags::UInt8: Writer.WriteUnsigned(*static_cast<const uint8*>(Value), EWidth::One); return {};
			case DurinCodeGen::EPropertyGenFlags::UInt16: Writer.WriteUnsigned(*static_cast<const uint16*>(Value), EWidth::Two); return {};
			case DurinCodeGen::EPropertyGenFlags::UInt32: Writer.WriteUnsigned(*static_cast<const uint32*>(Value), EWidth::Four); return {};
			case DurinCodeGen::EPropertyGenFlags::UInt64: Writer.WriteUnsigned(*static_cast<const uint64*>(Value), EWidth::Eight); return {};
			case DurinCodeGen::EPropertyGenFlags::Float: Writer.WriteFloat32Bits(std::bit_cast<uint32>(*static_cast<const float*>(Value))); return {};
			case DurinCodeGen::EPropertyGenFlags::Double: Writer.WriteFloat64Bits(std::bit_cast<uint64>(*static_cast<const double*>(Value))); return {};
			case DurinCodeGen::EPropertyGenFlags::String:
				Writer.WriteString(*static_cast<const FStringProperty*>(Property)->GetStringValuePtr(Container, ArrayIndex)); return {};
			case DurinCodeGen::EPropertyGenFlags::Name:
			{
				const FName& Name = *static_cast<const FNameProperty*>(Property)->GetNameValuePtr(Container, ArrayIndex);
				Writer.WriteName(Name.GetComparisonNameEntry()->GetPlainNameString(), Name.GetNumber());
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Guid:
				Writer.WriteGuid(*static_cast<const FGuidProperty*>(Property)->GetGuidValuePtr(Container, ArrayIndex)); return {};
			case DurinCodeGen::EPropertyGenFlags::Byte:
				Writer.WriteUnsigned(std::to_integer<uint8>(*static_cast<const std::byte*>(Value)), EWidth::One); return {};
			case DurinCodeGen::EPropertyGenFlags::Enum:
			{
				const auto* Enum = static_cast<const FEnumProperty*>(Property);
				const uint64 Raw = Enum->GetValueAsUInt64(Container, ArrayIndex);
				switch (Enum->GetUnderlyingType())
				{
				case DurinCodeGen::EEnumUnderlyingType::Int8: Writer.WriteSigned(static_cast<int8>(Raw), EWidth::One); return {};
				case DurinCodeGen::EEnumUnderlyingType::Int16: Writer.WriteSigned(static_cast<int16>(Raw), EWidth::Two); return {};
				case DurinCodeGen::EEnumUnderlyingType::Int32: Writer.WriteSigned(static_cast<int32>(Raw), EWidth::Four); return {};
				case DurinCodeGen::EEnumUnderlyingType::Int64: Writer.WriteSigned(std::bit_cast<int64>(Raw), EWidth::Eight); return {};
				case DurinCodeGen::EEnumUnderlyingType::UInt8: Writer.WriteUnsigned(static_cast<uint8>(Raw), EWidth::One); return {};
				case DurinCodeGen::EEnumUnderlyingType::UInt16: Writer.WriteUnsigned(static_cast<uint16>(Raw), EWidth::Two); return {};
				case DurinCodeGen::EEnumUnderlyingType::UInt32: Writer.WriteUnsigned(static_cast<uint32>(Raw), EWidth::Four); return {};
				case DurinCodeGen::EEnumUnderlyingType::UInt64: Writer.WriteUnsigned(Raw, EWidth::Eight); return {};
				default: return FailCanonicalToken(EReflectedMapKeyError::UnknownEnumStorage, Property, ArrayIndex);
				}
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* StructProperty = static_cast<const FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct || !Struct->HasCompleteAuthoredFields() || Struct->HasIdentical() || Struct->HasSerializer())
					return FailCanonicalToken(EReflectedMapKeyError::IncompleteStructEquality, Property, ArrayIndex);
				uint32 Ordinal = 0;
				FReflectedMapKeyResult Result;
				Struct->ForEachProperty([&](FProperty* Field) {
					const uint32 FieldOrdinal = Ordinal++;
					if (!Result || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim() && Result; ++FieldIndex)
					{
						Writer.WriteStructField(FieldOrdinal, FieldIndex);
						Result = AppendCanonicalProperty(Field, Value, FieldIndex, Writer);
						if (!Result) Result.Error.Route.insert(Result.Error.Route.begin(),
							{Property->NamePrivate.ToString(), ArrayIndex});
					}
				}, false);
				return Result;
			}
			default: return FailCanonicalToken(EReflectedMapKeyError::UnsupportedKind, Property, ArrayIndex);
			}
		}
	} // namespace

	auto BuildCanonicalMapKeyToken(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		FByteBuffer& OutToken
	) -> FReflectedMapKeyResult
	{
		ObjectPackage::FCanonicalMapKeyWriter Writer;
		if (auto Result = AppendCanonicalProperty(Property, Container, ArrayIndex, Writer); !Result) return Result;
		OutToken = Writer.TakeBytes();
		return {};
	}

	auto ValidateCanonicalMapKeyProperty(const FProperty* Property) -> FReflectedMapKeyResult
	{
		if (!Property) return FailCanonicalToken(EReflectedMapKeyError::NullProperty, Property);
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Bool:
		case DurinCodeGen::EPropertyGenFlags::Int8:
		case DurinCodeGen::EPropertyGenFlags::Int16:
		case DurinCodeGen::EPropertyGenFlags::Int32:
		case DurinCodeGen::EPropertyGenFlags::Int64:
		case DurinCodeGen::EPropertyGenFlags::UInt8:
		case DurinCodeGen::EPropertyGenFlags::UInt16:
		case DurinCodeGen::EPropertyGenFlags::UInt32:
		case DurinCodeGen::EPropertyGenFlags::UInt64:
		case DurinCodeGen::EPropertyGenFlags::Float:
		case DurinCodeGen::EPropertyGenFlags::Double:
		case DurinCodeGen::EPropertyGenFlags::String:
		case DurinCodeGen::EPropertyGenFlags::Name:
		case DurinCodeGen::EPropertyGenFlags::Guid:
		case DurinCodeGen::EPropertyGenFlags::Byte:
			return {};
		case DurinCodeGen::EPropertyGenFlags::Enum:
			if (static_cast<const FEnumProperty*>(Property)->GetUnderlyingType()
				== DurinCodeGen::EEnumUnderlyingType::Unknown)
				return FailCanonicalToken(EReflectedMapKeyError::UnknownEnumStorage, Property);
			return {};
		case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
				if (!Struct || !Struct->HasCompleteAuthoredFields() || Struct->HasIdentical() || Struct->HasSerializer())
					return FailCanonicalToken(EReflectedMapKeyError::IncompleteStructEquality, Property);
				FReflectedMapKeyResult Result;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (Result && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
					{
						Result = ValidateCanonicalMapKeyProperty(Field);
						if (!Result) Result.Error.Route.insert(Result.Error.Route.begin(),
							{Property->NamePrivate.ToString(), 0});
					}
				}, false);
				return Result;
			}
		default:
			return FailCanonicalToken(EReflectedMapKeyError::UnsupportedKind, Property);
		}
	}

	auto FormatReflectedMapKeyError(const FReflectedMapKeyError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EReflectedMapKeyError::None: return {};
		case EReflectedMapKeyError::NullProperty: return "CanonicalMapKeyInvalidInput: key property is null.";
		case EReflectedMapKeyError::NullContainer:
		case EReflectedMapKeyError::InvalidArrayIndex:
			return "CanonicalMapKeyInvalidInput: property, value, or array index is invalid.";
		case EReflectedMapKeyError::UnsupportedKind:
			return "CanonicalMapKeyUnsupported: object and container keys are not canonicalizable.";
		case EReflectedMapKeyError::UnknownEnumStorage:
			return "CanonicalMapKeyUnsupported: enum underlying type is unknown.";
		case EReflectedMapKeyError::IncompleteStructEquality:
			return "CanonicalMapKeyUnsupported: struct key lacks complete reflected equality semantics.";
		}
		return {};
	}

	auto ForEachNestedProperty(FProperty* Property, const std::function<void(FProperty*)>& Visitor) -> void
	{
		if (!Property)
		{
			return;
		}

		if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array)
		{
			FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInner();
			if (Inner)
			{
				Visitor(Inner);
				ForEachNestedProperty(Inner, Visitor);
			}
			return;
		}

		if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Map)
		{
			auto* MapProperty = static_cast<FMapProperty*>(Property);
			if (FProperty* Key = MapProperty->GetKeyProp())
			{
				Visitor(Key);
				ForEachNestedProperty(Key, Visitor);
			}
			if (FProperty* Value = MapProperty->GetValueProp())
			{
				Visitor(Value);
				ForEachNestedProperty(Value, Visitor);
			}
		}

		if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct)
		{
			if (DStruct* Struct = static_cast<FStructProperty*>(Property)->GetStruct())
			{
				Struct->ForEachProperty([&](FProperty* Field) {
					Visitor(Field);
					ForEachNestedProperty(Field, Visitor);
				},
										false);
			}
		}
	}
} // namespace Durin
