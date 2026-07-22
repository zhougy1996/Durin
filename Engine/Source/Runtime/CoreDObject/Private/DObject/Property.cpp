#include "DObject/DurinPropertyTypes.h"

#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

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

	auto FArrayProperty::Resize(void* Container, uint64 Num, uint32 ArrayIndex) const -> void
	{
		if (ArrayHelper)
		{
			ArrayHelper->Resize(GetValuePtr(Container, ArrayIndex), Num);
		}
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
	auto FMapProperty::Insert(void* Container, const void* Key, const void* Value, uint32 ArrayIndex) const -> void { if (MapHelper) MapHelper->Insert(GetValuePtr(Container, ArrayIndex), Key, Value); }
	auto FMapProperty::Contains(const void* Container, const void* Key, uint32 ArrayIndex) const -> bool { return MapHelper && MapHelper->Contains(GetValuePtr(Container, ArrayIndex), Key); }
	auto FMapProperty::RenameKey(void* Container, const void* OldKey, const void* NewKey, uint32 ArrayIndex) const -> bool { return MapHelper && MapHelper->RenameKey(GetValuePtr(Container, ArrayIndex), OldKey, NewKey); }
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
