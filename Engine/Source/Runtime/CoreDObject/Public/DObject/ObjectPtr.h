#pragma once

#include "DObjectFwd.h"

namespace Durin
{
	class FObjectPtrBase
	{
	public:
		FObjectPtrBase() = default;
		FObjectPtrBase(DObject* InObject)
			: Object(InObject)
		{
		}

		auto GetObject() const -> DObject* { return Object; }
		auto SetObject(DObject* InObject) -> void { Object = InObject; }
		auto Reset() -> void { Object = nullptr; }

	private:
		DObject* Object = nullptr;
	};

	template<typename T>
	class TObjectPtr : public FObjectPtrBase
	{
	public:
		TObjectPtr() = default;
		TObjectPtr(T* InObject)
			: FObjectPtrBase(InObject)
		{
		}

		auto operator=(T* InObject) -> TObjectPtr&
		{
			SetObject(InObject);
			return *this;
		}

		auto Get() const -> T* { return static_cast<T*>(GetObject()); }
		auto operator->() const -> T* { return Get(); }
		auto operator*() const -> T& { return *Get(); }
		explicit operator bool() const { return Get() != nullptr; }
		operator T*() const { return Get(); }
	};
}
