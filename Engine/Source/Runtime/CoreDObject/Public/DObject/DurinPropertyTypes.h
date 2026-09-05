#pragma once

#include "DObject/Property.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Misc/Guid.h"

namespace Durin
{
	// Describes fixed-width integral and floating-point reflected storage.
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

	// Describes reflected Boolean storage and its generated accessor contract.
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

	// Describes reflected std::string storage and lifecycle.
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

	// Describes reflected interned-name storage and lifecycle.
	class FNameProperty : public FProperty
	{
		DECLARE_FIELD(FNameProperty, FProperty, EClassCastFlags::FNameProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FNameProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FNameProperty(
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

		auto GetNameValuePtr(void* Container, uint32 ArrayIndex = 0) const -> FName*
		{
			return ContainerPtrToValuePtr<FName>(Container, ArrayIndex);
		}

		auto GetNameValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const FName*
		{
			return ContainerPtrToValuePtr<FName>(Container, ArrayIndex);
		}
	};

	// Describes reflected GUID storage and lifecycle.
	class FGuidProperty : public FProperty
	{
		DECLARE_FIELD(FGuidProperty, FProperty, EClassCastFlags::FGuidProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FGuidProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FGuidProperty(
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

		auto GetGuidValuePtr(void* Container, uint32 ArrayIndex = 0) const -> FGuid*
		{
			return ContainerPtrToValuePtr<FGuid>(Container, ArrayIndex);
		}

		auto GetGuidValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const FGuid*
		{
			return ContainerPtrToValuePtr<FGuid>(Container, ArrayIndex);
		}
	};

	// Couples reflected enum storage to its DEnum metadata and underlying numeric property.
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
		COREDOBJECT_API auto GetValueAsUInt64(const void* Container, uint32 ArrayIndex = 0) const -> uint64;
		COREDOBJECT_API auto SetValueFromUInt64(void* Container, uint64 Value, uint32 ArrayIndex = 0) const -> void;

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

	// Describes a reflected object reference and its required DClass constraint.
	class FObjectProperty : public FProperty
	{
		DECLARE_FIELD(FObjectProperty, FProperty, EClassCastFlags::FObjectProperty, COREDOBJECT_API)
	public:
		using FReadObjectValue = DurinCodeGen::FObjectPropertyParams::FReadObjectValue;
		using FWriteObjectValue = DurinCodeGen::FObjectPropertyParams::FWriteObjectValue;

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
			bool bInIsObjectPtrWrapper,
			FReadObjectValue InReadObjectValue,
			FWriteObjectValue InWriteObjectValue
		);

		COREDOBJECT_API auto GetObjectPropertyValue(const void* Container, uint32 ArrayIndex = 0) const -> DObject*;
		COREDOBJECT_API auto SetObjectPropertyValue(void* Container, DObject* Value, uint32 ArrayIndex = 0) const -> void;

	private:
		FReadObjectValue ReadObjectValue = nullptr;
		FWriteObjectValue WriteObjectValue = nullptr;
	};

	// Describes a typed soft object reference. Its weak cache is never a GC edge.
	class FSoftObjectProperty : public FProperty
	{
		DECLARE_FIELD(FSoftObjectProperty, FProperty, EClassCastFlags::FSoftObjectProperty, COREDOBJECT_API)
	public:
		using FMutableSoftValueAccessor = DurinCodeGen::FSoftObjectPropertyParams::FMutableSoftValueAccessor;
		using FConstSoftValueAccessor = DurinCodeGen::FSoftObjectPropertyParams::FConstSoftValueAccessor;

		COREDOBJECT_API FSoftObjectProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FSoftObjectProperty(
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
		);

		auto GetExpectedClass() const -> DClass* { return GetReferencedClass(); }
		COREDOBJECT_API auto GetSoftObjectPtr(void* Container, uint32 ArrayIndex = 0) const -> FSoftObjectPtr*;
		COREDOBJECT_API auto GetSoftObjectPtr(const void* Container, uint32 ArrayIndex = 0) const -> const FSoftObjectPtr*;

	private:
		FMutableSoftValueAccessor MutableSoftValueAccessor = nullptr;
		FConstSoftValueAccessor ConstSoftValueAccessor = nullptr;
	};

	// Describes a typed non-owning object reference. It is never a GC edge.
	class FWeakObjectProperty : public FProperty
	{
		DECLARE_FIELD(FWeakObjectProperty, FProperty, EClassCastFlags::FWeakObjectProperty, COREDOBJECT_API)
	public:
		using FMutableWeakValueAccessor = DurinCodeGen::FWeakObjectPropertyParams::FMutableWeakValueAccessor;
		using FConstWeakValueAccessor = DurinCodeGen::FWeakObjectPropertyParams::FConstWeakValueAccessor;

		COREDOBJECT_API FWeakObjectProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);
		COREDOBJECT_API FWeakObjectProperty(
			FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags, uint16 InArrayDim, uint16 InOffset,
			uint16 InElementSize, DClass* InExpectedClass,
			FMutableWeakValueAccessor InMutableWeakValueAccessor,
			FConstWeakValueAccessor InConstWeakValueAccessor);

		auto GetExpectedClass() const -> DClass* { return GetReferencedClass(); }
		COREDOBJECT_API auto GetWeakObjectPtr(void* Container, uint32 ArrayIndex = 0) const -> FWeakObjectPtr*;
		COREDOBJECT_API auto GetWeakObjectPtr(const void* Container, uint32 ArrayIndex = 0) const -> const FWeakObjectPtr*;

	private:
		FMutableWeakValueAccessor MutableWeakValueAccessor = nullptr;
		FConstWeakValueAccessor ConstWeakValueAccessor = nullptr;
	};

	// Describes inline reflected value-struct storage managed through DStruct operations.
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
			DStruct* InStruct
		);

		auto GetStruct() const -> DStruct* { return Struct; }

	private:
		DStruct* Struct = nullptr;
	};

	// Describes reflected vector storage and the property metadata of each element.
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
			const FArrayOps* InOps
		);

		auto SetInner(FProperty* InInner) -> void { Inner = InInner; }
		auto GetInner() const -> FProperty* { return Inner; }
		auto GetContainerPtr(void* Container) const -> void* { return GetValuePtr(Container); }
		auto GetContainerPtr(const void* Container) const -> const void* { return GetValuePtr(Container); }
		auto GetOps() const -> const FArrayOps& { return *Ops; }
		auto HasArrayOps() const -> bool { return Ops != nullptr; }
		auto HasCapability(EArrayOpsFlags Flag) const -> bool { return EnumHasAllFlags(Ops->Flags, Flag); }
		COREDOBJECT_API auto GetNum(const void* Container, uint64& OutNum, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto VisitElements(const void* Container, FArrayConstVisitor Visitor, void* Context, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto VisitMutableElements(void* Container, FArrayMutableVisitor Visitor, void* Context, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto GetElement(const void* Container, uint64 Index, const void** OutElement, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto GetMutableElement(void* Container, uint64 Index, void** OutElement, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto ResizeChecked(void* Container, uint64 Num, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto Num(const void* Container, uint32 ArrayIndex = 0) const -> uint64;
		COREDOBJECT_API auto GetElementPtr(const void* Container, uint64 Index, uint32 ArrayIndex = 0) const -> const void*;
		COREDOBJECT_API auto GetMutableElementPtr(void* Container, uint64 Index, uint32 ArrayIndex = 0) const -> void*;
		COREDOBJECT_API auto Resize(
			void* Container, uint64 Num, uint32 ArrayIndex = 0, std::string* OutError = nullptr
		) const -> bool;

	private:
		FProperty* Inner = nullptr;
		const FArrayOps* Ops = nullptr;
	};

	// Describes reflected unordered-map storage and its key/value property metadata.
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
			const FMapOps* InOps
		);

		auto SetKeyProp(FProperty* InKeyProp) -> void { KeyProp = InKeyProp; }
		auto SetValueProp(FProperty* InValueProp) -> void { ValueProp = InValueProp; }
		auto GetKeyProp() const -> FProperty* { return KeyProp; }
		auto GetValueProp() const -> FProperty* { return ValueProp; }
		auto GetContainerPtr(void* Container) const -> void* { return GetValuePtr(Container); }
		auto GetContainerPtr(const void* Container) const -> const void* { return GetValuePtr(Container); }
		auto GetOps() const -> const FMapOps& { return *Ops; }
		auto HasMapOps() const -> bool { return Ops != nullptr; }
		auto HasCapability(EMapOpsFlags Flag) const -> bool { return EnumHasAllFlags(Ops->Flags, Flag); }
		COREDOBJECT_API auto GetNum(const void* Container, uint64& OutNum, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto VisitEntries(const void* Container, FMapConstVisitor Visitor, void* Context, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto VisitMutableEntries(void* Container, FMapMutableVisitor Visitor, void* Context, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto FindValue(const void* Container, const void* Key, const void** OutValue, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto FindMutableValue(void* Container, const void* Key, void** OutValue, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto ClearChecked(void* Container, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto InsertChecked(void* Container, const void* Key, const void* Value, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto RenameKeyChecked(void* Container, const void* OldKey, const void* NewKey, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto RemoveChecked(void* Container, const void* Key, uint32 ArrayIndex = 0) const -> EContainerOpResult;
		COREDOBJECT_API auto Num(const void* Container, uint32 ArrayIndex = 0) const -> uint64;
		COREDOBJECT_API auto Clear(void* Container, uint32 ArrayIndex = 0) const -> void;
		COREDOBJECT_API auto Insert(
			void* Container, const void* Key, const void* Value, uint32 ArrayIndex = 0, std::string* OutError = nullptr
		) const -> bool;
		COREDOBJECT_API auto Contains(const void* Container, const void* Key, uint32 ArrayIndex = 0) const -> bool;
		COREDOBJECT_API auto RenameKey(
			void* Container, const void* OldKey, const void* NewKey, uint32 ArrayIndex = 0, std::string* OutError = nullptr
		) const -> bool;
		COREDOBJECT_API auto Remove(void* Container, const void* Key, uint32 ArrayIndex = 0) const -> bool;

	private:
		FProperty* KeyProp = nullptr;
		FProperty* ValueProp = nullptr;
		const FMapOps* Ops = nullptr;
	};

	COREDOBJECT_API auto ForEachNestedProperty(FProperty* Property, const std::function<void(FProperty*)>& Visitor) -> void;

	// Builds the version-1 logical token used to order supported reflected Map keys.
	COREDOBJECT_API auto BuildCanonicalMapKeyToken(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		FByteBuffer& OutToken,
		std::string* OutError = nullptr
	) -> bool;
	COREDOBJECT_API auto ValidateCanonicalMapKeyProperty(
		const FProperty* Property,
		std::string* OutError = nullptr
	) -> bool;
} // namespace Durin
