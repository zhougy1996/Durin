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
			std::string* OutError
		) -> bool
		{
			if (!OutError) return false;
			if (DStruct* Struct = GetPropertyStruct(Property))
			{
				*OutError = std::format(
					"DStructOperationUnavailable: {} is unavailable for '{}'.",
					Operation,
					Struct->GetQualifiedName().ToString()
				);
			}
			else
			{
				*OutError = std::format(
					"ReflectedValueOperationUnavailable: {} is unavailable for property '{}'.",
					Operation,
					Property ? Property->NamePrivate.ToString() : std::string("<null>")
				);
			}
			return false;
		}

		auto ArePropertyValuesIdenticalImpl(
			const FProperty* Property,
			const void* LeftContainer,
			uint32 LeftArrayIndex,
			const void* RightContainer,
			uint32 RightArrayIndex
		) -> bool;

		struct FMapIdenticalContext
		{
			const FMapProperty* Map = nullptr;
			const FProperty* ValueProperty = nullptr;
			const void* RightContainer = nullptr;
			uint32 RightArrayIndex = 0;
			bool bIdentical = true;
		};

		auto VisitMapIdenticalEntry(void* RawContext, const void* Key, const void* LeftValue) -> bool
		{
			auto& Context = *static_cast<FMapIdenticalContext*>(RawContext);
			const void* RightValue = nullptr;
			if (Context.Map->FindValue(Context.RightContainer, Key, &RightValue, Context.RightArrayIndex) != EContainerOpResult::Success
				|| !ArePropertyValuesIdenticalImpl(Context.ValueProperty, LeftValue, 0, RightValue, 0))
			{
				Context.bIdentical = false;
				return false;
			}
			return true;
		}

		auto ArePropertyValuesIdenticalImpl(
			const FProperty* Property,
			const void* LeftContainer,
			uint32 LeftArrayIndex,
			const void* RightContainer,
			uint32 RightArrayIndex
		) -> bool
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
						   Property->GetElementSize()
					   )
					   == 0;
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
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				{
					const auto* SoftProperty = static_cast<const FSoftObjectProperty*>(Property);
					const FSoftObjectPtr* Left = SoftProperty->GetSoftObjectPtr(LeftContainer, LeftArrayIndex);
					const FSoftObjectPtr* Right = SoftProperty->GetSoftObjectPtr(RightContainer, RightArrayIndex);
					return Left && Right && *Left == *Right;
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
					},
											false);
					return bIdentical;
				}
			case DurinCodeGen::EPropertyGenFlags::Array:
				{
					const auto* ArrayProperty = static_cast<const FArrayProperty*>(Property);
					FProperty* Inner = ArrayProperty->GetInner();
					if (!Inner || !ArrayProperty->HasArrayOps()) return false;
					const uint64 LeftNum = ArrayProperty->Num(LeftContainer, LeftArrayIndex);
					if (LeftNum != ArrayProperty->Num(RightContainer, RightArrayIndex)) return false;
					for (uint64 Index = 0; Index < LeftNum; ++Index)
					{
						if (!ArePropertyValuesIdenticalImpl(
								Inner, ArrayProperty->GetElementPtr(LeftContainer, Index, LeftArrayIndex), 0,
								ArrayProperty->GetElementPtr(RightContainer, Index, RightArrayIndex), 0
							)) return false;
					}
					return true;
				}
			case DurinCodeGen::EPropertyGenFlags::Map:
				{
					const auto* MapProperty = static_cast<const FMapProperty*>(Property);
					FProperty* KeyProperty = MapProperty->GetKeyProp();
					FProperty* ValueProperty = MapProperty->GetValueProp();
					if (!KeyProperty || !ValueProperty || !MapProperty->HasMapOps()) return false;
					const uint64 LeftNum = MapProperty->Num(LeftContainer, LeftArrayIndex);
					if (LeftNum != MapProperty->Num(RightContainer, RightArrayIndex)) return false;
					if (!MapProperty->HasCapability(EMapOpsFlags::Lookup)) return false;
					FMapIdenticalContext Context{MapProperty, ValueProperty, RightContainer, RightArrayIndex};
					return MapProperty->VisitEntries(LeftContainer, &VisitMapIdenticalEntry, &Context, LeftArrayIndex)
							   == EContainerOpResult::Success
						   && Context.bIdentical;
				}
			default:
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
		return CopyConstructValueFunction != nullptr;
	}

	auto FProperty::CanCopyAssignValue() const -> bool
	{
		if (DStruct* Struct = GetPropertyStruct(this)) return Struct->CanCopyAssign();
		return CopyAssignValueFunction != nullptr;
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
				if (bIsObjectPtrWrapper)
					std::construct_at(static_cast<FObjectPtr*>(Memory));
				else
					std::construct_at(static_cast<DObject**>(Memory), nullptr);
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
		if (DStruct* Struct = GetPropertyStruct(this))
			Struct->GetOps().CopyConstruct(Destination, Source);
		else
			CopyConstructValueFunction(Destination, Source);
		return true;
	}

	auto FProperty::CopyAssignValue(void* Destination, const void* Source, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Destination || !Source || !CanCopyAssignValue())
			return ReportUnavailablePropertyOperation(this, "CopyAssign", OutError);
		if (DStruct* Struct = GetPropertyStruct(this))
			Struct->GetOps().CopyAssign(Destination, Source);
		else
			CopyAssignValueFunction(Destination, Source);
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
		std::string* OutError
	) -> bool
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
		const size_t Size = std::max<size_t>(1, static_cast<size_t>(InProperty->GetOffset()) + static_cast<size_t>(InProperty->GetElementSize()) * static_cast<size_t>(InArrayIndex) + static_cast<size_t>(InProperty->GetValueSize()));
		Memory = ::operator new(Size, std::align_val_t(Alignment));
		Value = InProperty->GetValuePtr(Memory, InArrayIndex);
		return true;
	}

	auto FReflectedValueStorage::DefaultConstruct(
		const FProperty* InProperty,
		uint32 InArrayIndex,
		std::string* OutError
	) -> bool
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
		std::string* OutError
	) -> bool
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
		uint32 RightArrayIndex
	) -> bool
	{
		return ArePropertyValuesIdenticalImpl(
			Property, LeftContainer, LeftArrayIndex, RightContainer, RightArrayIndex
		);
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

	auto FArrayProperty::Resize(void* Container, uint64 Num, uint32 ArrayIndex, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Container || !Inner)
		{
			return ReportUnavailablePropertyOperation(Inner, "DefaultConstruct", OutError);
		}
		const uint64 CurrentNum = this->Num(Container, ArrayIndex);
		if (Num < CurrentNum && !Inner->CanDestroyValue())
			return ReportUnavailablePropertyOperation(Inner, "Destroy", OutError);
		if (Num > CurrentNum && (!Inner->CanDefaultConstructValue() || !Inner->CanDestroyValue()))
			return ReportUnavailablePropertyOperation(Inner, "DefaultConstruct", OutError);
		if (ResizeChecked(Container, Num, ArrayIndex) != EContainerOpResult::Success)
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
	auto FMapProperty::Insert(
		void* Container,
		const void* Key,
		const void* Value,
		uint32 ArrayIndex,
		std::string* OutError
	) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Container || !Key || !Value || !KeyProp || !ValueProp) return false;
		if (GetPropertyStruct(KeyProp) && !KeyProp->CanCopyConstructValue())
			return ReportUnavailablePropertyOperation(KeyProp, "CopyConstruct", OutError);
		if (GetPropertyStruct(ValueProp)
			&& (!ValueProp->CanCopyConstructValue() || !ValueProp->CanCopyAssignValue()))
			return ReportUnavailablePropertyOperation(ValueProp, "CopyConstruct/CopyAssign", OutError);
		if (InsertChecked(Container, Key, Value, ArrayIndex) != EContainerOpResult::Success)
			return ReportUnavailablePropertyOperation(ValueProp, "CopyConstruct/CopyAssign", OutError);
		return true;
	}
	auto FMapProperty::Contains(const void* Container, const void* Key, uint32 ArrayIndex) const -> bool
	{
		const void* Value = nullptr;
		return FindValue(Container, Key, &Value, ArrayIndex) == EContainerOpResult::Success;
	}
	auto FMapProperty::RenameKey(
		void* Container,
		const void* OldKey,
		const void* NewKey,
		uint32 ArrayIndex,
		std::string* OutError
	) const -> bool
	{
		if (OutError) OutError->clear();
		if (!Container || !OldKey || !NewKey || !KeyProp) return false;
		if (GetPropertyStruct(KeyProp)
			&& (!KeyProp->CanCopyConstructValue() || !KeyProp->CanCopyAssignValue()))
			return ReportUnavailablePropertyOperation(KeyProp, "CopyConstruct/CopyAssign", OutError);
		return RenameKeyChecked(Container, OldKey, NewKey, ArrayIndex) == EContainerOpResult::Success;
	}
	auto FMapProperty::Remove(void* Container, const void* Key, uint32 ArrayIndex) const -> bool { return RemoveChecked(Container, Key, ArrayIndex) == EContainerOpResult::Success; }

	namespace
	{
		template<std::unsigned_integral T>
		auto AppendBigEndian(std::vector<uint8>& Token, T Value) -> void
		{
			for (size_t Index = sizeof(T); Index > 0; --Index)
				Token.push_back(static_cast<uint8>(Value >> ((Index - 1) * 8)));
		}

		template<std::integral T>
		auto AppendSortableInteger(std::vector<uint8>& Token, const void* Value) -> void
		{
			using U = std::make_unsigned_t<T>;
			U Bits = 0;
			std::memcpy(&Bits, Value, sizeof(Bits));
			if constexpr (std::is_signed_v<T>) Bits ^= U(1) << (sizeof(U) * 8 - 1);
			AppendBigEndian(Token, Bits);
		}

		template<std::floating_point T>
		auto AppendSortableFloat(std::vector<uint8>& Token, const void* Value) -> void
		{
			using U = std::conditional_t<sizeof(T) == 4, uint32, uint64>;
			U Bits = 0;
			std::memcpy(&Bits, Value, sizeof(Bits));
			constexpr U Sign = U(1) << (sizeof(U) * 8 - 1);
			if ((Bits & ~Sign) == 0) Bits = 0;
			Bits = (Bits & Sign) ? ~Bits : (Bits ^ Sign);
			AppendBigEndian(Token, Bits);
		}

		auto FailCanonicalToken(std::string_view Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}
	} // namespace

	auto BuildCanonicalMapKeyToken(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		std::vector<uint8>& OutToken,
		std::string* OutError
	) -> bool
	{
		if (OutError) OutError->clear();
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim())
			return FailCanonicalToken("CanonicalMapKeyInvalidInput: property, value, or array index is invalid.", OutError);

		OutToken.push_back(static_cast<uint8>(Property->GetKind()));
		const void* Value = Property->GetValuePtr(Container, ArrayIndex);
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Bool: OutToken.push_back(*static_cast<const bool*>(Value) ? 1 : 0); return true;
		case DurinCodeGen::EPropertyGenFlags::Int8: AppendSortableInteger<int8>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::Int16: AppendSortableInteger<int16>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::Int32: AppendSortableInteger<int32>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::Int64: AppendSortableInteger<int64>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::UInt8: AppendSortableInteger<uint8>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::UInt16: AppendSortableInteger<uint16>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::UInt32: AppendSortableInteger<uint32>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::UInt64: AppendSortableInteger<uint64>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::Float: AppendSortableFloat<float>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::Double: AppendSortableFloat<double>(OutToken, Value); return true;
		case DurinCodeGen::EPropertyGenFlags::Enum:
			{
				const auto* Enum = static_cast<const FEnumProperty*>(Property);
				const uint64 Raw = Enum->GetValueAsUInt64(Container, ArrayIndex);
				switch (Enum->GetUnderlyingType())
				{
				case DurinCodeGen::EEnumUnderlyingType::Int8:
					{
						const int8 V = static_cast<int8>(Raw);
						AppendSortableInteger<int8>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::Int16:
					{
						const int16 V = static_cast<int16>(Raw);
						AppendSortableInteger<int16>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::Int32:
					{
						const int32 V = static_cast<int32>(Raw);
						AppendSortableInteger<int32>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::Int64:
					{
						const int64 V = static_cast<int64>(Raw);
						AppendSortableInteger<int64>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::UInt8:
					{
						const uint8 V = static_cast<uint8>(Raw);
						AppendSortableInteger<uint8>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::UInt16:
					{
						const uint16 V = static_cast<uint16>(Raw);
						AppendSortableInteger<uint16>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::UInt32:
					{
						const uint32 V = static_cast<uint32>(Raw);
						AppendSortableInteger<uint32>(OutToken, &V);
						return true;
					}
				case DurinCodeGen::EEnumUnderlyingType::UInt64: AppendSortableInteger<uint64>(OutToken, &Raw); return true;
				default: return FailCanonicalToken("CanonicalMapKeyUnsupported: enum underlying type is unknown.", OutError);
				}
			}
		case DurinCodeGen::EPropertyGenFlags::String:
			{
				const auto& Text = *static_cast<const FStringProperty*>(Property)->GetStringValuePtr(Container, ArrayIndex);
				AppendBigEndian(OutToken, static_cast<uint64>(Text.size()));
				OutToken.insert(OutToken.end(), Text.begin(), Text.end());
				return true;
			}
		case DurinCodeGen::EPropertyGenFlags::Name:
			{
				const FName& Name = *static_cast<const FNameProperty*>(Property)->GetNameValuePtr(Container, ArrayIndex);
				const std::string Text = Name.GetComparisonNameEntry()->GetPlainNameString();
				AppendBigEndian(OutToken, static_cast<uint64>(Text.size()));
				OutToken.insert(OutToken.end(), Text.begin(), Text.end());
				AppendBigEndian(OutToken, Name.GetNumber());
				return true;
			}
		case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				const FGuid& Guid = *static_cast<const FGuidProperty*>(Property)->GetGuidValuePtr(Container, ArrayIndex);
				AppendBigEndian(OutToken, Guid.A);
				AppendBigEndian(OutToken, Guid.B);
				AppendBigEndian(OutToken, Guid.C);
				AppendBigEndian(OutToken, Guid.D);
				return true;
			}
		case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* StructProperty = static_cast<const FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct || !Struct->HasCompleteAuthoredFields() || Struct->HasIdentical() || Struct->HasSerializer())
					return FailCanonicalToken("CanonicalMapKeyUnsupported: struct key lacks complete reflected equality semantics.", OutError);
				uint32 Ordinal = 0;
				bool bSuccess = true;
				Struct->ForEachProperty([&](FProperty* Field) {
					const uint32 FieldOrdinal = Ordinal++;
					if (!bSuccess || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim() && bSuccess; ++FieldIndex)
					{
						AppendBigEndian(OutToken, FieldOrdinal);
						AppendBigEndian(OutToken, FieldIndex);
						bSuccess = BuildCanonicalMapKeyToken(Field, Value, FieldIndex, OutToken, OutError);
					}
				},
										false);
				return bSuccess;
			}
		default:
			return FailCanonicalToken("CanonicalMapKeyUnsupported: object and container keys are not canonicalizable.", OutError);
		}
	}

	auto ValidateCanonicalMapKeyProperty(const FProperty* Property, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!Property) return FailCanonicalToken("CanonicalMapKeyInvalidInput: key property is null.", OutError);
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
			return true;
		case DurinCodeGen::EPropertyGenFlags::Enum:
			return static_cast<const FEnumProperty*>(Property)->GetUnderlyingType()
					   != DurinCodeGen::EEnumUnderlyingType::Unknown
				   || FailCanonicalToken("CanonicalMapKeyUnsupported: enum underlying type is unknown.", OutError);
		case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
				if (!Struct || !Struct->HasCompleteAuthoredFields() || Struct->HasIdentical() || Struct->HasSerializer())
					return FailCanonicalToken("CanonicalMapKeyUnsupported: struct key lacks complete reflected equality semantics.", OutError);
				bool bSupported = true;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (bSupported && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
						bSupported = ValidateCanonicalMapKeyProperty(Field, OutError);
				},
										false);
				return bSupported;
			}
		default:
			return FailCanonicalToken("CanonicalMapKeyUnsupported: object and container keys are not canonicalizable.", OutError);
		}
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
