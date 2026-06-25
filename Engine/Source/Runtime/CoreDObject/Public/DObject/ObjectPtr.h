#pragma once

#include "DObject/Object.h"

namespace Durin
{
	class FObjectPtrBase
	{
	public:
		auto GetObject() const -> DObject* { return Object; }
		auto SetObject(DObject* InObject) -> void { Object = InObject; }

	protected:
		DObject* Object = nullptr;
	};

	template<typename T>
	class TObjectPtr : public FObjectPtrBase
	{
	public:
		TObjectPtr() = default;
		TObjectPtr(std::nullptr_t) {}
		TObjectPtr(T* InObject) { Object = ToDObject(InObject); }

		auto Get() const -> T* { return static_cast<T*>(Object); }
		auto Reset() -> void { Object = nullptr; }

		auto operator=(std::nullptr_t) -> TObjectPtr&
		{
			Object = nullptr;
			return *this;
		}

		auto operator=(T* InObject) -> TObjectPtr&
		{
			Object = ToDObject(InObject);
			return *this;
		}

		auto operator->() const -> T* { return Get(); }
		operator T*() const { return Get(); }
		explicit operator bool() const { return Object != nullptr; }

	private:
		static auto ToDObject(T* InObject) -> DObject*
		{
			return InObject;
		}
	};
}
