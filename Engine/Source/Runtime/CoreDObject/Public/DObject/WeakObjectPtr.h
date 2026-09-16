#pragma once

#include "DObject/ObjectKey.h"

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
		explicit FWeakObjectPtr(FObjectKey InKey) : Key(InKey) {}

		COREDOBJECT_API auto Get() const -> DObject*;
		COREDOBJECT_API auto SetObject(DObject* InObject) -> void;
		COREDOBJECT_API auto IsValid() const -> bool;

		auto Reset() -> void { Key = nullptr; }
		auto GetKey() const -> FObjectKey { return Key; }
		auto SetKey(FObjectKey InKey) -> void { Key = InKey; }
		friend auto operator==(const FWeakObjectPtr&, const FWeakObjectPtr&) -> bool = default;

		auto operator=(std::nullptr_t) -> FWeakObjectPtr&
		{
			Reset();
			return *this;
		}

		COREDOBJECT_API auto operator=(DObject* InObject) -> FWeakObjectPtr&;

	private:
		FObjectKey Key;
	};

	static_assert(sizeof(FWeakObjectPtr) == sizeof(uint64));
	static_assert(std::is_trivially_copyable_v<FWeakObjectPtr>);

	// Provides typed non-owning access with the same game-thread resolution contract as FWeakObjectPtr.
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
		auto GetKey() const -> FObjectKey { return WeakPtr.GetKey(); }
		auto GetBase() -> FWeakObjectPtr& { return WeakPtr; }
		auto GetBase() const -> const FWeakObjectPtr& { return WeakPtr; }

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
