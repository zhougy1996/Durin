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
			DClass* InReferencedClass,
			DEnum* InReferencedEnum
		);

		auto GetEnum() const -> DEnum* { return ReferencedEnum; }
		COREDOBJECT_API auto GetUnderlyingType() const -> DurinCodeGen::EEnumUnderlyingType;

		template<typename T>
		auto GetEnumValuePtr(void* Container, uint32 ArrayIndex = 0) const -> T*
		{
			return ContainerPtrToValuePtr<T>(Container, ArrayIndex);
		}

		template<typename T>
		auto GetEnumValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const T*
		{
			return ContainerPtrToValuePtr<T>(Container, ArrayIndex);
		}

	private:
		DEnum* ReferencedEnum = nullptr;
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
			DClass* InReferencedClass,
			bool bInIsObjectPtrWrapper = false
		);

		COREDOBJECT_API auto GetObjectPropertyValue(const void* Container, uint32 ArrayIndex = 0) const -> DObject*;
		COREDOBJECT_API auto SetObjectPropertyValue(void* Container, DObject* Value, uint32 ArrayIndex = 0) const -> void;
	};

	class FStructProperty : public FProperty
	{
		DECLARE_FIELD(FStructProperty, FProperty, EClassCastFlags::FStructProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FStructProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FStructProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DStruct* InStruct
		);

		auto GetStruct() const -> DStruct* { return Struct; }

	private:
		DStruct* Struct = nullptr;
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
			DClass* InReferencedClass,
			const DurinCodeGen::FArrayPropertyHelper* InArrayHelper = nullptr
		);

		auto SetInner(FProperty* InInner) -> void { Inner = InInner; }
		auto GetInner() const -> FProperty* { return Inner; }
		auto GetContainerPtr(void* Container) const -> void* { return GetValuePtr(Container); }
		auto GetContainerPtr(const void* Container) const -> const void* { return GetValuePtr(Container); }
		COREDOBJECT_API auto Num(const void* Container, uint32 ArrayIndex = 0) const -> uint64;
		COREDOBJECT_API auto GetElementPtr(const void* Container, uint64 Index, uint32 ArrayIndex = 0) const -> const void*;
		COREDOBJECT_API auto GetMutableElementPtr(void* Container, uint64 Index, uint32 ArrayIndex = 0) const -> void*;
		COREDOBJECT_API auto Resize(void* Container, uint64 Num, uint32 ArrayIndex = 0) const -> void;
		auto HasArrayHelper() const -> bool { return ArrayHelper != nullptr; }

	private:
		FProperty* Inner = nullptr;
		const DurinCodeGen::FArrayPropertyHelper* ArrayHelper = nullptr;
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
			DClass* InReferencedClass,
			const DurinCodeGen::FMapPropertyHelper* InMapHelper = nullptr
		);

		auto SetKeyProp(FProperty* InKeyProp) -> void { KeyProp = InKeyProp; }
		auto SetValueProp(FProperty* InValueProp) -> void { ValueProp = InValueProp; }
		auto GetKeyProp() const -> FProperty* { return KeyProp; }
		auto GetValueProp() const -> FProperty* { return ValueProp; }
		auto GetContainerPtr(void* Container) const -> void* { return GetValuePtr(Container); }
		auto GetContainerPtr(const void* Container) const -> const void* { return GetValuePtr(Container); }
		COREDOBJECT_API auto Num(const void* Container, uint32 ArrayIndex = 0) const -> uint64;
		COREDOBJECT_API auto GetKeyPtr(const void* Container, uint64 Index, uint32 ArrayIndex = 0) const -> const void*;
		COREDOBJECT_API auto GetMappedValuePtr(const void* Container, uint64 Index, uint32 ArrayIndex = 0) const -> const void*;
		COREDOBJECT_API auto Clear(void* Container, uint32 ArrayIndex = 0) const -> void;
		COREDOBJECT_API auto CreateKey() const -> void*;
		COREDOBJECT_API auto DestroyKey(void* Key) const -> void;
		COREDOBJECT_API auto CreateValue() const -> void*;
		COREDOBJECT_API auto DestroyValue(void* Value) const -> void;
		COREDOBJECT_API auto Insert(void* Container, const void* Key, const void* Value, uint32 ArrayIndex = 0) const -> void;
		auto HasMapHelper() const -> bool { return MapHelper != nullptr; }

	private:
		FProperty* KeyProp = nullptr;
		FProperty* ValueProp = nullptr;
		const DurinCodeGen::FMapPropertyHelper* MapHelper = nullptr;
	};

	COREDOBJECT_API auto ForEachNestedProperty(FProperty* Property, const std::function<void(FProperty*)>& Visitor) -> void;
} // namespace Durin
