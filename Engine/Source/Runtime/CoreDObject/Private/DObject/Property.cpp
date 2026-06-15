#include "DObject/DurinPropertyTypes.h"

#include "DObject/Object.h"

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
		DClass* InReferencedClass
	)
		: FField(InOwner, InName, InObjectFlags)
		, PropertyFlags(InPropertyFlags)
		, ArrayDim(InArrayDim)
		, Offset(InOffset)
		, ElementSize(InElementSize)
		, Kind(InKind)
		, ReferencedClass(InReferencedClass)
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
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
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
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
	}

	auto FObjectProperty::GetObjectPropertyValue(const void* Container, uint32 ArrayIndex) const -> DObject*
	{
		DObject* const* ValuePtr = ContainerPtrToValuePtr<DObject*>(Container, ArrayIndex);
		return ValuePtr ? *ValuePtr : nullptr;
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
		DClass* InReferencedClass
	)
		: FProperty(InOwner, InName, InObjectFlags, InPropertyFlags, InArrayDim, InOffset, InElementSize, InKind, InReferencedClass)
	{
		ClassPrivate = StaticClass();
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
}
