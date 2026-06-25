#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"

namespace Durin
{
	class FReferenceCollector
	{
	public:
		virtual ~FReferenceCollector() = default;
		virtual auto AddReferencedObject(DObject*& Object) -> void = 0;
	};

	class FScopedObjectRoot
	{
	public:
		COREDOBJECT_API explicit FScopedObjectRoot(DObject* InObject);
		COREDOBJECT_API ~FScopedObjectRoot();

	private:
		DObject* Object = nullptr;
	};

	COREDOBJECT_API auto AddToRoot(DObject* Object) -> void;
	COREDOBJECT_API auto RemoveFromRoot(DObject* Object) -> void;
	COREDOBJECT_API auto DestroyObject(DObject* Object) -> void;
	COREDOBJECT_API auto CollectGarbage() -> void;
	COREDOBJECT_API auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void;
}
