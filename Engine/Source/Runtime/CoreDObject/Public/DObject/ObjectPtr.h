#pragma once

#include "DObject/Object.h"
#include "DObject/ObjectHandle.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	COREDOBJECT_API auto ConditionallyMarkAsReachable(DObject* Object) -> void;

	// Stores a garbage-collection-aware object handle and marks assigned objects reachable.
	class FObjectPtr
	{
	public:
		FObjectPtr() = default;
		FObjectPtr(std::nullptr_t) {}
		explicit FObjectPtr(DObject* InObject) { SetObject(InObject); }

		auto Get() const -> DObject* { return ResolveObjectHandle(Handle); }
		auto SetObject(DObject* InObject) -> void
		{
			Handle = MakeObjectHandle(InObject);
			ConditionallyMarkAsReachable(InObject);
		}

		auto GetKey() const -> FObjectKey
		{
			FObjectKey Key;
			Key.ObjectIndex = Handle.ObjectIndex;
			Key.ObjectSerialNumber = Handle.ObjectSerialNumber;
			return Key;
		}
		auto IsValid() const -> bool { DObject* Object = Get(); return Object && !Object->IsPendingKill(); }
		auto Reset() -> void { SetObject(nullptr); }
		explicit operator bool() const { return Get() != nullptr; }

	private:
		FObjectHandle Handle = nullptr;
	};

	// Provides typed access over FObjectPtr while supporting forward-declared DObject types.
	template<typename T>
	class TObjectPtr
	{
	public:
		TObjectPtr() = default;
		TObjectPtr(std::nullptr_t) {}
		TObjectPtr(T* InObject) : ObjectPtr(ToDObject(InObject)) {}

		auto Get() const -> T* { return FromDObject(ObjectPtr.Get()); }
		auto Reset() -> void { ObjectPtr.Reset(); }
		auto GetKey() const -> FObjectKey { return ObjectPtr.GetKey(); }
		auto IsValid() const -> bool { return ObjectPtr.IsValid(); }

		auto operator=(std::nullptr_t) -> TObjectPtr&
		{
			ObjectPtr.Reset();
			return *this;
		}

		auto operator=(T* InObject) -> TObjectPtr&
		{
			ObjectPtr.SetObject(ToDObject(InObject));
			return *this;
		}

		auto operator->() const -> T* { return Get(); }
		auto operator*() const -> T& { return *Get(); }
		operator T*() const { return Get(); }
		explicit operator bool() const { return Get() != nullptr; }

	private:
		static auto ToDObject(T* InObject) -> DObject*
		{
			if constexpr (TIsCompleteType<T>::value)
			{
				static_assert(std::is_base_of_v<DObject, T>, "TObjectPtr<T> requires T to derive from DObject");
				return static_cast<DObject*>(InObject);
			}
			else
			{
				return reinterpret_cast<DObject*>(InObject);
			}
		}

		static auto FromDObject(DObject* InObject) -> T*
		{
			if constexpr (TIsCompleteType<T>::value)
			{
				static_assert(std::is_base_of_v<DObject, T>, "TObjectPtr<T> requires T to derive from DObject");
				return static_cast<T*>(InObject);
			}
			else
			{
				return reinterpret_cast<T*>(InObject);
			}
		}

		FObjectPtr ObjectPtr;
	};
}
