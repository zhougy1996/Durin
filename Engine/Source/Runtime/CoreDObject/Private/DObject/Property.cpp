#include "DObject/DurinPropertyTypes.h"

#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	IMPLEMENT_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FNumericProperty, FProperty, EClassCastFlags::FNumericProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FBoolProperty, FProperty, EClassCastFlags::FBoolProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FStringProperty, FProperty, EClassCastFlags::FStringProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FEnumProperty, FProperty, EClassCastFlags::FEnumProperty, COREDOBJECT_API)
	IMPLEMENT_FIELD(FObjectProperty, FProperty, EClassCastFlags::FObjectProperty, COREDOBJECT_API)
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
			return ValuePtr ? ValuePtr->GetObject() : nullptr;
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
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
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
	}
}
