#pragma once

class DObject
{
public:
	CORE_API DObject();

	CORE_API virtual ~DObject() = default;

};

class DObjectManager
{
public:
	CORE_API static DObjectManager* Get();

	CORE_API auto Destroy(DObject* Object) -> void;

	CORE_API auto DestroyPendingObjects() -> void;

private:
	TArray<DObject*> PendingDestroyObjects_;
};

extern CORE_API DObjectManager* GObjectManager;
