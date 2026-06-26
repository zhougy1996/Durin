#pragma once

#include "DObject/Object.h"
#include "DObject/ObjectHandle.h"

namespace Durin
{
	COREDOBJECT_API auto ConditionallyMarkAsReachable(DObject* Object) -> void;

	class FObjectPtr
	{
	public:
		FObjectPtr() = default;
		FObjectPtr(std::nullptr_t) {}
		explicit FObjectPtr(DObject* InObject) { SetObject(InObject); }

		auto GetObject() const -> DObject* { return ResolveObjectHandle(Handle); }
		auto SetObject(DObject* InObject) -> void
		{
			Handle = InObject;
			ConditionallyMarkAsReachable(InObject);
		}

		auto GetHandle() const -> FObjectHandle { return Handle; }
		auto Reset() -> void { SetObject(nullptr); }
		explicit operator bool() const { return GetObject() != nullptr; }

	private:
		FObjectHandle Handle = nullptr;
	};

	template<typename T>
	class TObjectPtr
	{
	public:
		static_assert(std::is_base_of_v<DObject, T>, "TObjectPtr<T> requires T to derive from DObject");

		TObjectPtr() = default;
		TObjectPtr(std::nullptr_t) {}
		TObjectPtr(T* InObject) : ObjectPtr(ToDObject(InObject)) {}

		auto Get() const -> T* { return static_cast<T*>(ObjectPtr.GetObject()); }
		auto Reset() -> void { ObjectPtr.Reset(); }
		auto GetHandle() const -> FObjectHandle { return ObjectPtr.GetHandle(); }

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
			return InObject;
		}

		FObjectPtr ObjectPtr;
	};
}
