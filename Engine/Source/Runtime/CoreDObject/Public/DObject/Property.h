#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectGlobals.h"
#include "Field.h"

namespace Durin
{
	// Describes one reflected field's storage, flags, referenced type, and value lifecycle.
	class FProperty : public FField
	{
		DECLARE_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FProperty(
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

		auto GetPropertyFlags() const -> EPropertyFlags { return PropertyFlags; }
		auto GetArrayDim() const -> uint16 { return ArrayDim; }
		auto GetOffset() const -> uint16 { return Offset; }
		auto GetElementSize() const -> uint16 { return ElementSize; }
		auto GetKind() const -> DurinCodeGen::EPropertyGenFlags { return Kind; }
		auto GetReferencedClass() const -> DClass* { return ReferencedClass; }
		auto IsObjectPtrWrapper() const -> bool { return bIsObjectPtrWrapper; }
		auto GetValueSize() const -> uint32 { return ValueSize; }
		auto GetValueAlignment() const -> uint32 { return ValueAlignment; }
		auto HasValueLifecycle() const -> bool { return InitializeValueFunction != nullptr && DestroyValueFunction != nullptr; }
		auto HasValueAccessors() const -> bool { return MutableValueAccessor != nullptr || ConstValueAccessor != nullptr; }
		auto GetOwnerProperty() const -> FProperty* { return static_cast<FProperty*>(Owner.ToField()); }
		auto HasAnyPropertyFlags(EPropertyFlags InFlags) const -> bool { return EnumHasAnyFlags(PropertyFlags, InFlags); }

		auto GetValuePtr(void* Container, uint32 ArrayIndex = 0) const -> void*
		{
			if (MutableValueAccessor) return MutableValueAccessor(Container, ArrayIndex);
			return static_cast<uint8*>(Container) + Offset + ElementSize * ArrayIndex;
		}

		auto GetValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const void*
		{
			if (ConstValueAccessor) return ConstValueAccessor(Container, ArrayIndex);
			return static_cast<const uint8*>(Container) + Offset + ElementSize * ArrayIndex;
		}

		auto SetValueAccessors(
			void* (*InMutableAccessor)(void*, uint32),
			const void* (*InConstAccessor)(const void*, uint32)
		) -> void
		{
			MutableValueAccessor = InMutableAccessor;
			ConstValueAccessor = InConstAccessor;
		}

		auto SetValueLifecycle(
			uint32 InValueSize,
			uint32 InValueAlignment,
			void (*InInitializeValue)(void*),
			void (*InDestroyValue)(void*)
		) -> void
		{
			ValueSize = InValueSize;
			ValueAlignment = InValueAlignment;
			InitializeValueFunction = InInitializeValue;
			DestroyValueFunction = InDestroyValue;
		}

		auto InitializeValue(void* Memory) const -> bool
		{
			if (!InitializeValueFunction) return false;
			InitializeValueFunction(Memory);
			return true;
		}

		auto DestroyValue(void* Memory) const -> void
		{
			if (DestroyValueFunction) DestroyValueFunction(Memory);
		}

		template<typename T>
		auto ContainerPtrToValuePtr(void* Container, uint32 ArrayIndex = 0) const -> T*
		{
			return static_cast<T*>(GetValuePtr(Container, ArrayIndex));
		}

		template<typename T>
		auto ContainerPtrToValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const T*
		{
			return static_cast<const T*>(GetValuePtr(Container, ArrayIndex));
		}

	private:
		EPropertyFlags PropertyFlags = EPropertyFlags::None;
		uint16 ArrayDim = 1;
		uint16 Offset = 0;
		uint16 ElementSize = 0;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		DClass* ReferencedClass = nullptr;
		bool bIsObjectPtrWrapper = false;
		uint32 ValueSize = 0;
		uint32 ValueAlignment = 0;
		void (*InitializeValueFunction)(void*) = nullptr;
		void (*DestroyValueFunction)(void*) = nullptr;
		void* (*MutableValueAccessor)(void*, uint32) = nullptr;
		const void* (*ConstValueAccessor)(const void*, uint32) = nullptr;
	};
}
