#pragma once

#include "DObject/ObjectPtr.h"

namespace Durin
{
	/**
	 * A non-owning object handle that may be copied across threads, but may only
	 * be assigned from or resolved to a DObject on the game thread.
	 *
	 * Cross-thread users must publish independent copies and must not mutate the
	 * same weak pointer instance concurrently.
	 */
	class FWeakObjectPtr
	{
	public:
		FWeakObjectPtr() = default;
		FWeakObjectPtr(std::nullptr_t) {}
		COREDOBJECT_API explicit FWeakObjectPtr(DObject* InObject);

		COREDOBJECT_API auto Get() const -> DObject*;
		COREDOBJECT_API auto SetObject(DObject* InObject) -> void;
		COREDOBJECT_API auto IsValid() const -> bool;

		auto Reset() -> void { Handle = nullptr; }
		auto GetHandle() const -> FObjectHandle { return Handle; }

		auto operator=(std::nullptr_t) -> FWeakObjectPtr&
		{
			Reset();
			return *this;
		}

		COREDOBJECT_API auto operator=(DObject* InObject) -> FWeakObjectPtr&;

	private:
		FObjectHandle Handle = nullptr;
	};

	static_assert(sizeof(FWeakObjectPtr) == sizeof(FObjectHandle));
	static_assert(std::is_trivially_copyable_v<FWeakObjectPtr>);

	template<typename T>
	class TWeakObjectPtr
	{
	public:
		TWeakObjectPtr() = default;
		TWeakObjectPtr(std::nullptr_t) {}
		TWeakObjectPtr(T* InObject)
			: WeakPtr(ToDObject(InObject))
		{
		}

		auto Get() const -> T* { return FromDObject(WeakPtr.Get()); }
		auto IsValid() const -> bool { return WeakPtr.IsValid(); }
		auto Reset() -> void { WeakPtr.Reset(); }
		auto GetHandle() const -> FObjectHandle { return WeakPtr.GetHandle(); }

		auto operator=(std::nullptr_t) -> TWeakObjectPtr&
		{
			WeakPtr.Reset();
			return *this;
		}

		auto operator=(T* InObject) -> TWeakObjectPtr&
		{
			WeakPtr.SetObject(ToDObject(InObject));
			return *this;
		}

	private:
		static auto ToDObject(T* InObject) -> DObject*
		{
			if constexpr (TIsCompleteType<T>::value)
			{
				static_assert(std::is_base_of_v<DObject, T>, "TWeakObjectPtr<T> requires T to derive from DObject");
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
				static_assert(std::is_base_of_v<DObject, T>, "TWeakObjectPtr<T> requires T to derive from DObject");
				return static_cast<T*>(InObject);
			}
			else
			{
				return reinterpret_cast<T*>(InObject);
			}
		}

		FWeakObjectPtr WeakPtr;
	};
}
