#include "DObject/WeakObjectPtr.h"

#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto CheckWeakObjectThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}
	}

	FWeakObjectPtr::FWeakObjectPtr(DObject* InObject)
	{
		SetObject(InObject);
	}

	auto FWeakObjectPtr::Get() const -> DObject*
	{
		CheckWeakObjectThread();
		DObject* Object = ResolveObjectHandle(Handle);
		return Object && !Object->IsPendingKill() ? Object : nullptr;
	}

	auto FWeakObjectPtr::SetObject(DObject* InObject) -> void
	{
		CheckWeakObjectThread();
		Handle = MakeObjectHandle(InObject);
	}

	auto FWeakObjectPtr::IsValid() const -> bool
	{
		return Get() != nullptr;
	}

	auto FWeakObjectPtr::operator=(DObject* InObject) -> FWeakObjectPtr&
	{
		SetObject(InObject);
		return *this;
	}
}
