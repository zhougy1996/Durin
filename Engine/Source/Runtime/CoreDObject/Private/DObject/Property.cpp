#include "DObject/DurinPropertyTypes.h"

#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

#include <bit>

namespace Durin
{
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
			case DurinCodeGen::EPropertyGenFlags::Object:
				return true;
			default:
				return false;
			}
		}

		auto ReportUnavailablePropertyOperation(
			const FProperty* Property,
			std::string_view Operation,
			std::string* OutError) -> bool
		{
			if (!OutError) return false;
			if (DStruct* Struct = GetPropertyStruct(Property))
			{
				*OutError = std::format(
					"DStructOperationUnavailable: {} is unavailable for '{}'.",
					Operation,
					Struct->GetQualifiedName().ToString());
			}
			else
			{
				*OutError = std::format(
					"ReflectedValueOperationUnavailable: {} is unavailable for property '{}'.",
					Operation,
					Property ? Property->NamePrivate.ToString() : std::string("<null>"));
			}
			return false;
		}

		auto ArePropertyValuesIdenticalImpl(
			const FProperty* Property,
			const void* LeftContainer,
			uint32 LeftArrayIndex,
			const void* RightContainer,
			uint32 RightArrayIndex) -> bool
		{
			if (!Property || !LeftContainer || !RightContainer) return false;

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
				return std::memcmp(
					Property->GetValuePtr(LeftContainer, LeftArrayIndex),
					Property->GetValuePtr(RightContainer, RightArrayIndex),
					Property->GetElementSize()) == 0;
			case DurinCodeGen::EPropertyGenFlags::String:
			{
				const auto* StringProperty = static_cast<const FStringProperty*>(Property);
				return *StringProperty->GetStringValuePtr(LeftContainer, LeftArrayIndex)
					== *StringProperty->GetStringValuePtr(RightContainer, RightArrayIndex);
			}
			case DurinCodeGen::EPropertyGenFlags::Name:
			{
				const auto* NameProperty = static_cast<const FNameProperty*>(Property);
				return *NameProperty->GetNameValuePtr(LeftContainer, LeftArrayIndex)
					== *NameProperty->GetNameValuePtr(RightContainer, RightArrayIndex);
			}
			case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				const auto* GuidProperty = static_cast<const FGuidProperty*>(Property);
				return *GuidProperty->GetGuidValuePtr(LeftContainer, LeftArrayIndex)
					== *GuidProperty->GetGuidValuePtr(RightContainer, RightArrayIndex);
			}
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				const auto* ObjectProperty = static_cast<const FObjectProperty*>(Property);
				return ObjectProperty->GetObjectPropertyValue(LeftContainer, LeftArrayIndex)
					== ObjectProperty->GetObjectPropertyValue(RightContainer, RightArrayIndex);
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* StructProperty = static_cast<const FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct) return false;
				const void* LeftValue = Property->GetValuePtr(LeftContainer, LeftArrayIndex);
				const void* RightValue = Property->GetValuePtr(RightContainer, RightArrayIndex);
				if (Struct->HasIdentical()) return Struct->GetOps().Identical(LeftValue, RightValue);

				bool bIdentical = true;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (!bIdentical || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					for (uint32 Index = 0; Index < Field->GetArrayDim(); ++Index)
					{
						if (!ArePropertyValuesIdenticalImpl(Field, LeftValue, Index, RightValue, Index))
						{
							bIdentical = false;
							break;
						}
					}
				}, false);
				return bIdentical;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				const auto* ArrayProperty = static_cast<const FArrayProperty*>(Property);
				FProperty* Inner = ArrayProperty->GetInner();
				if (!Inner || !ArrayProperty->HasArrayHelper()) return false;
				const uint64 LeftNum = ArrayProperty->Num(LeftContainer, LeftArrayIndex);
				if (LeftNum != ArrayProperty->Num(RightContainer, RightArrayIndex)) return false;
				for (uint64 Index = 0; Index < LeftNum; ++Index)
				{
					if (!ArePropertyValuesIdenticalImpl(
						Inner, ArrayProperty->GetElementPtr(LeftContainer, Index, LeftArrayIndex), 0,
						ArrayProperty->GetElementPtr(RightContainer, Index, RightArrayIndex), 0)) return false;
				}
				return true;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				const auto* MapProperty = static_cast<const FMapProperty*>(Property);
				FProperty* KeyProperty = MapProperty->GetKeyProp();
				FProperty* ValueProperty = MapProperty->GetValueProp();
				if (!KeyProperty || !ValueProperty || !MapProperty->HasMapHelper()) return false;
				const uint64 LeftNum = MapProperty->Num(LeftContainer, LeftArrayIndex);
				if (LeftNum != MapProperty->Num(RightContainer, RightArrayIndex)) return false;
				std::vector<bool> Matched(static_cast<size_t>(LeftNum), false);
				for (uint64 LeftIndex = 0; LeftIndex < LeftNum; ++LeftIndex)
				{
					bool bFound = false;
					for (uint64 RightIndex = 0; RightIndex < LeftNum; ++RightIndex)
					{
						if (Matched[static_cast<size_t>(RightIndex)]) continue;
						if (!ArePropertyValuesIdenticalImpl(
							KeyProperty, MapProperty->GetKeyPtr(LeftContainer, LeftIndex, LeftArrayIndex), 0,
							MapProperty->GetKeyPtr(RightContainer, RightIndex, RightArrayIndex), 0)) continue;
						if (!ArePropertyValuesIdenticalImpl(
							ValueProperty, MapProperty->GetMappedValuePtr(LeftContainer, LeftIndex, LeftArrayIndex), 0,
							MapProperty->GetMappedValuePtr(RightContainer, RightIndex, RightArrayIndex), 0)) continue;
						Matched[static_cast<size_t>(RightIndex)] = true;
						bFound = true;
						break;
					}
					if (!bFound) return false;
				}
				return true;
			}
			default:
				return false;
			}
		}
	}

	IMPLEMENT_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FNumericProperty, FProperty, EClassCastFlags::FNumericProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FBoolProperty, FProperty, EClassCastFlags::FBoolProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FStringProperty, FProperty, EClassCastFlags::FStringProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FNameProperty, FProperty, EClassCastFlags::FNameProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FGuidProperty, FProperty, EClassCastFlags::FGuidProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FEnumProperty, FProperty, EClassCastFlags::FEnumProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FObjectProperty, FProperty, EClassCastFlags::FObjectProperty, COREDOBJECT_API)
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
		return false;
	}

	auto FProperty::CanCopyAssignValue() const -> bool
	{
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->CanCopyAssign();
		return false;
	}

	auto FProperty::InitializeValue(void* Memory, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Memory || !CanDefaultConstructValue() || !CanDestroyValue())
			return ReportUnavailablePropertyOperation(this, "DefaultConstruct", OutError);
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
				if (bIsObjectPtrWrapper) std::construct_at(static_cast<FObjectPtr*>(Memory));
				else std::construct_at(static_cast<DObject**>(Memory), nullptr);
				break;
			default: std::memset(Memory, 0, GetValueSize()); break;
			}
		}
		return true;
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

	auto FProperty::CopyConstructValue(void* Destination, const void* Source, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Destination || !Source || !CanCopyConstructValue() || !CanDestroyValue())
			return ReportUnavailablePropertyOperation(this, "CopyConstruct", OutError);
		GetPropertyStruct(this)->GetOps().CopyConstruct(Destination, Source);
		return true;
	}

	auto FProperty::CopyAssignValue(void* Destination, const void* Source, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Destination || !Source || !CanCopyAssignValue())
			return ReportUnavailablePropertyOperation(this, "CopyAssign", OutError);
		GetPropertyStruct(this)->GetOps().CopyAssign(Destination, Source);
		return true;
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

	auto FReflectedValueStorage::Allocate(
		const FProperty* InProperty,
		uint32 InArrayIndex,
		std::string* OutError) -> bool
	{
		if (!InProperty || InProperty->HasValueAccessors()
			|| InArrayIndex >= InProperty->GetArrayDim()
			|| InProperty->GetValueSize() == 0
			|| InProperty->GetValueAlignment() == 0)
		{
			Property = InProperty;
			return Fail(OutError, "Storage");
		}

		Property = InProperty;
		ArrayIndex = InArrayIndex;
		Alignment = std::max<size_t>(InProperty->GetValueAlignment(), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		const size_t Size = std::max<size_t>(1,
			static_cast<size_t>(InProperty->GetOffset())
			+ static_cast<size_t>(InProperty->GetElementSize()) * static_cast<size_t>(InArrayIndex)
			+ static_cast<size_t>(InProperty->GetValueSize()));
		Memory = ::operator new(Size, std::align_val_t(Alignment));
		Value = InProperty->GetValuePtr(Memory, InArrayIndex);
		return true;
	}

	auto FReflectedValueStorage::DefaultConstruct(
		const FProperty* InProperty,
		uint32 InArrayIndex,
		std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (Memory || bLive)
		{
			return Fail(OutError, "DefaultConstruct");
		}
		if (!InProperty || !InProperty->CanDefaultConstructValue() || !InProperty->CanDestroyValue())
		{
			Property = InProperty;
			return Fail(OutError, "DefaultConstruct");
		}
		if (!Allocate(InProperty, InArrayIndex, OutError)) return false;
		if (!Property->InitializeValue(Value, OutError))
		{
			Reset();
			return false;
		}
		bLive = true;
		return true;
	}

	auto FReflectedValueStorage::CopyConstruct(
		const FProperty* InProperty,
		const void* SourceValue,
		uint32 InArrayIndex,
		std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (Memory || bLive || !InProperty || !SourceValue
			|| !InProperty->CanCopyConstructValue() || !InProperty->CanDestroyValue())
		{
			if (!Property) Property = InProperty;
			return Fail(OutError, "CopyConstruct");
		}
		if (!Allocate(InProperty, InArrayIndex, OutError)) return false;
		if (!Property->CopyConstructValue(Value, SourceValue, OutError))
		{
			Reset();
			return false;
		}
		bLive = true;
		return true;
	}

	auto FReflectedValueStorage::CopyAssign(const void* SourceValue, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!bLive || !Property || !SourceValue || !Property->CanCopyAssignValue())
			return Fail(OutError, "CopyAssign");
		return Property->CopyAssignValue(Value, SourceValue, OutError);
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

	auto FReflectedValueStorage::Fail(std::string* OutError, std::string_view Operation) const -> bool
	{
		return ReportUnavailablePropertyOperation(Property, Operation, OutError);
	}

	auto ArePropertyValuesIdentical(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex) -> bool
	{
		return ArePropertyValuesIdenticalImpl(
			Property, LeftContainer, LeftArrayIndex, RightContainer, RightArrayIndex);
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
		bool bInIsObjectPtrWrapper
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass, bInIsObjectPtrWrapper)
	{
		ClassPrivate = StaticClass();
	}

	auto FObjectProperty::GetObjectPropertyValue(const void* Container, uint32 ArrayIndex) const -> DObject*
	{
		if (IsObjectPtrWrapper())
		{
			const FObjectPtr* ValuePtr = ContainerPtrToValuePtr<FObjectPtr>(Container, ArrayIndex);
			return ValuePtr ? ValuePtr->Get() : nullptr;
		}

		DObject* const* ValuePtr = ContainerPtrToValuePtr<DObject*>(Container, ArrayIndex);
		return ValuePtr ? *ValuePtr : nullptr;
	}

	auto FObjectProperty::SetObjectPropertyValue(void* Container, DObject* Value, uint32 ArrayIndex) const -> void
	{
		if (IsObjectPtrWrapper())
		{
			FObjectPtr* ValuePtr = ContainerPtrToValuePtr<FObjectPtr>(Container, ArrayIndex);
			if (ValuePtr)
			{
				ValuePtr->SetObject(Value);
			}
			return;
		}

		DObject** ValuePtr = ContainerPtrToValuePtr<DObject*>(Container, ArrayIndex);
		if (ValuePtr)
		{
			*ValuePtr = Value;
		}
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
		uint16 InElementSize,
		DurinCodeGen::EPropertyGenFlags InKind,
		DStruct* InStruct
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, nullptr)
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
		const DurinCodeGen::FArrayPropertyHelper* InArrayHelper
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
		, ArrayHelper(InArrayHelper)
	{
		ClassPrivate = StaticClass();
	}

	auto FArrayProperty::Num(const void* Container, uint32 ArrayIndex) const -> uint64
	{
		return ArrayHelper ? ArrayHelper->Num(GetValuePtr(Container, ArrayIndex)) : 0;
	}

	auto FArrayProperty::GetElementPtr(const void* Container, uint64 Index, uint32 ArrayIndex) const -> const void*
	{
		return ArrayHelper ? ArrayHelper->GetElement(GetValuePtr(Container, ArrayIndex), Index) : nullptr;
	}

	auto FArrayProperty::GetMutableElementPtr(void* Container, uint64 Index, uint32 ArrayIndex) const -> void*
	{
		return ArrayHelper ? ArrayHelper->GetMutableElement(GetValuePtr(Container, ArrayIndex), Index) : nullptr;
	}

	auto FArrayProperty::Resize(void* Container, uint64 Num, uint32 ArrayIndex, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!ArrayHelper || !Container || !Inner)
		{
			return ReportUnavailablePropertyOperation(Inner, "DefaultConstruct", OutError);
		}
		const uint64 CurrentNum = this->Num(Container, ArrayIndex);
		if (Num < CurrentNum && !Inner->CanDestroyValue())
			return ReportUnavailablePropertyOperation(Inner, "Destroy", OutError);
		if (Num > CurrentNum && (!Inner->CanDefaultConstructValue() || !Inner->CanDestroyValue()))
			return ReportUnavailablePropertyOperation(Inner, "DefaultConstruct", OutError);
		if (!ArrayHelper->Resize(GetValuePtr(Container, ArrayIndex), Num))
			return ReportUnavailablePropertyOperation(Inner, "DefaultConstruct", OutError);
		return true;
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
		const DurinCodeGen::FMapPropertyHelper* InMapHelper
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
		, MapHelper(InMapHelper)
	{
		ClassPrivate = StaticClass();
	}

	auto FMapProperty::Num(const void* Container, uint32 ArrayIndex) const -> uint64 { return MapHelper ? MapHelper->Num(GetValuePtr(Container, ArrayIndex)) : 0; }
	auto FMapProperty::GetKeyPtr(const void* Container, uint64 Index, uint32 ArrayIndex) const -> const void* { return MapHelper ? MapHelper->GetKey(GetValuePtr(Container, ArrayIndex), Index) : nullptr; }
	auto FMapProperty::GetMappedValuePtr(const void* Container, uint64 Index, uint32 ArrayIndex) const -> const void* { return MapHelper ? MapHelper->GetValue(GetValuePtr(Container, ArrayIndex), Index) : nullptr; }
	auto FMapProperty::GetMutableMappedValuePtr(void* Container, uint64 Index, uint32 ArrayIndex) const -> void* { return MapHelper ? MapHelper->GetMutableValue(GetValuePtr(Container, ArrayIndex), Index) : nullptr; }
	auto FMapProperty::Clear(void* Container, uint32 ArrayIndex) const -> void { if (MapHelper) MapHelper->Clear(GetValuePtr(Container, ArrayIndex)); }
	auto FMapProperty::CreateKey() const -> void* { return MapHelper ? MapHelper->CreateKey() : nullptr; }
	auto FMapProperty::CreateKeyCopy(const void* Key) const -> void* { return MapHelper ? MapHelper->CreateKeyCopy(Key) : nullptr; }
	auto FMapProperty::DestroyKey(void* Key) const -> void { if (MapHelper) MapHelper->DestroyKey(Key); }
	auto FMapProperty::CreateValue() const -> void* { return MapHelper ? MapHelper->CreateValue() : nullptr; }
	auto FMapProperty::DestroyValue(void* Value) const -> void { if (MapHelper) MapHelper->DestroyValue(Value); }
	auto FMapProperty::Insert(
		void* Container,
		const void* Key,
		const void* Value,
		uint32 ArrayIndex,
		std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!MapHelper || !Container || !Key || !Value || !KeyProp || !ValueProp) return false;
		if (GetPropertyStruct(KeyProp) && !KeyProp->CanCopyConstructValue())
			return ReportUnavailablePropertyOperation(KeyProp, "CopyConstruct", OutError);
		if (GetPropertyStruct(ValueProp)
			&& (!ValueProp->CanCopyConstructValue() || !ValueProp->CanCopyAssignValue()))
			return ReportUnavailablePropertyOperation(ValueProp, "CopyConstruct/CopyAssign", OutError);
		if (!MapHelper->Insert(GetValuePtr(Container, ArrayIndex), Key, Value))
			return ReportUnavailablePropertyOperation(ValueProp, "CopyConstruct/CopyAssign", OutError);
		return true;
	}
	auto FMapProperty::Contains(const void* Container, const void* Key, uint32 ArrayIndex) const -> bool { return MapHelper && MapHelper->Contains(GetValuePtr(Container, ArrayIndex), Key); }
	auto FMapProperty::RenameKey(
		void* Container,
		const void* OldKey,
		const void* NewKey,
		uint32 ArrayIndex,
		std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!MapHelper || !Container || !OldKey || !NewKey || !KeyProp) return false;
		if (GetPropertyStruct(KeyProp)
			&& (!KeyProp->CanCopyConstructValue() || !KeyProp->CanCopyAssignValue()))
			return ReportUnavailablePropertyOperation(KeyProp, "CopyConstruct/CopyAssign", OutError);
		if (!MapHelper->RenameKey(GetValuePtr(Container, ArrayIndex), OldKey, NewKey)) return false;
		return true;
	}
	auto FMapProperty::Remove(void* Container, const void* Key, uint32 ArrayIndex) const -> bool { return MapHelper && MapHelper->Remove(GetValuePtr(Container, ArrayIndex), Key); }

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
				}, false);
			}
		}
	}
}
