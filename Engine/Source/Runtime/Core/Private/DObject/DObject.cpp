#include "DObject/DObject.h"

DObject::DObject()
{
}

DObjectManager* GObjectManager = nullptr;

auto DObjectManager::Get() -> DObjectManager*
{
	static DObjectManager Instance;
	return &Instance;
}
auto DObjectManager::Destroy(DObject* Object) -> void
{
	if (Object)
	{
		PendingDestroyObjects_.push_back(Object);
	}
	else
	{
		DOGE_ERROR("Attempted to add a null object to pending destroy list.");
	}
}

auto DObjectManager::DestroyPendingObjects() -> void
{
	for (DObject* Object : PendingDestroyObjects_)
	{
		delete Object;
	}
	PendingDestroyObjects_.clear();
}

