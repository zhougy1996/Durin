#pragma once

#include "DObject/ObjectPtr.h"

namespace Durin
{
	class FReferenceCollector;

	// Owns one collector-visible strong reference to an exact object generation.
	class FStrongObjectPtr
	{
	public:
		FStrongObjectPtr() = default;
		FStrongObjectPtr(std::nullptr_t) {}
		COREDOBJECT_API explicit FStrongObjectPtr(DObject* InObject);
		COREDOBJECT_API explicit FStrongObjectPtr(FObjectHandle InHandle);
		COREDOBJECT_API ~FStrongObjectPtr();
		COREDOBJECT_API FStrongObjectPtr(const FStrongObjectPtr& Other);
		COREDOBJECT_API auto operator=(const FStrongObjectPtr& Other) -> FStrongObjectPtr&;
		COREDOBJECT_API FStrongObjectPtr(FStrongObjectPtr&& Other) noexcept;
		COREDOBJECT_API auto operator=(FStrongObjectPtr&& Other) noexcept -> FStrongObjectPtr&;

		COREDOBJECT_API auto Get() const -> DObject*;
		auto GetHandle() const -> FObjectHandle { return Handle; }
		auto IsValid() const -> bool { return Get() != nullptr; }
		COREDOBJECT_API auto Reset() -> void;
		explicit operator bool() const { return IsValid(); }

	private:
		FObjectHandle Handle = nullptr;
	};

	// Provides typed access over an independently owned native strong reference.
	template<typename T>
	class TStrongObjectPtr
	{
	public:
		TStrongObjectPtr() = default;
		TStrongObjectPtr(std::nullptr_t) {}
		TStrongObjectPtr(T* InObject) : StrongPtr(ToDObject(InObject)) {}
		explicit TStrongObjectPtr(FObjectHandle InHandle) : StrongPtr(InHandle) {}

		auto Get() const -> T* { return FromDObject(StrongPtr.Get()); }
		auto GetHandle() const -> FObjectHandle { return StrongPtr.GetHandle(); }
		auto IsValid() const -> bool { return StrongPtr.IsValid(); }
		auto Reset() -> void { StrongPtr.Reset(); }

		auto operator=(std::nullptr_t) -> TStrongObjectPtr&
		{
			StrongPtr.Reset();
			return *this;
		}

		auto operator=(T* InObject) -> TStrongObjectPtr&
		{
			StrongPtr = FStrongObjectPtr(ToDObject(InObject));
			return *this;
		}

		auto operator->() const -> T* { return Get(); }
		auto operator*() const -> T& { return *Get(); }
		operator T*() const { return Get(); }
		explicit operator bool() const { return IsValid(); }

	private:
		static auto ToDObject(T* InObject) -> DObject*
		{
			if constexpr (TIsCompleteType<T>::value)
			{
				static_assert(std::is_base_of_v<DObject, T>, "TStrongObjectPtr<T> requires T to derive from DObject");
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
				static_assert(std::is_base_of_v<DObject, T>, "TStrongObjectPtr<T> requires T to derive from DObject");
				return static_cast<T*>(InObject);
			}
			else
			{
				return reinterpret_cast<T*>(InObject);
			}
		}

		FStrongObjectPtr StrongPtr;
	};

	namespace Private
	{
		COREDOBJECT_API auto AddStrongObjectReferences(FReferenceCollector& Collector) -> void;
	}
}
