#pragma once

#include "DObject/Property.h"

namespace Durin
{
	class FNumericProperty : public FProperty
	{
		DECLARE_FIELD(FNumericProperty, FProperty, EClassCastFlags::FNumericProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FNumericProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FNumericProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);
	};

	class FBoolProperty : public FProperty
	{
		DECLARE_FIELD(FBoolProperty, FProperty, EClassCastFlags::FBoolProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FBoolProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FBoolProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);
	};

	class FStringProperty : public FProperty
	{
		DECLARE_FIELD(FStringProperty, FProperty, EClassCastFlags::FStringProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FStringProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FStringProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);

		auto GetStringValuePtr(void* Container, uint32 ArrayIndex = 0) const -> std::string*
		{
			return ContainerPtrToValuePtr<std::string>(Container, ArrayIndex);
		}

		auto GetStringValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const std::string*
		{
			return ContainerPtrToValuePtr<std::string>(Container, ArrayIndex);
		}
	};

	class FEnumProperty : public FProperty
	{
		DECLARE_FIELD(FEnumProperty, FProperty, EClassCastFlags::FEnumProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FEnumProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FEnumProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);
	};

	class FObjectProperty : public FProperty
	{
		DECLARE_FIELD(FObjectProperty, FProperty, EClassCastFlags::FObjectProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FObjectProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FObjectProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);

		COREDOBJECT_API auto GetObjectPropertyValue(const void* Container, uint32 ArrayIndex = 0) const -> DObject*;
	};

	class FArrayProperty : public FProperty
	{
		DECLARE_FIELD(FArrayProperty, FProperty, EClassCastFlags::FArrayProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FArrayProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FArrayProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);

		auto SetInner(FProperty* InInner) -> void { Inner = InInner; }
		auto GetInner() const -> FProperty* { return Inner; }
		auto GetContainerPtr(void* Container) const -> void* { return GetValuePtr(Container); }
		auto GetContainerPtr(const void* Container) const -> const void* { return GetValuePtr(Container); }

	private:
		FProperty* Inner = nullptr;
	};

	class FMapProperty : public FProperty
	{
		DECLARE_FIELD(FMapProperty, FProperty, EClassCastFlags::FMapProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FMapProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FMapProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);

		auto SetKeyProp(FProperty* InKeyProp) -> void { KeyProp = InKeyProp; }
		auto SetValueProp(FProperty* InValueProp) -> void { ValueProp = InValueProp; }
		auto GetKeyProp() const -> FProperty* { return KeyProp; }
		auto GetValueProp() const -> FProperty* { return ValueProp; }
		auto GetContainerPtr(void* Container) const -> void* { return GetValuePtr(Container); }
		auto GetContainerPtr(const void* Container) const -> const void* { return GetValuePtr(Container); }

	private:
		FProperty* KeyProp = nullptr;
		FProperty* ValueProp = nullptr;
	};
}
