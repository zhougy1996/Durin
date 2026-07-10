#include "DObject/DObjectArray.h"

#include "DObject/ObjectHandle.h"

namespace Durin
{
	FDObjectArray GDObjectArray;

	auto FDObjectArray::Remove(DObject* ObjToRemove) -> void
	{
		for (DObject*& Object : Objs)
		{
			if (Object == ObjToRemove)
			{
				Object = nullptr;
				return;
			}
		}
	}

	auto FDObjectArray::Contains(const DObject* Object) const -> bool
	{
		return Object != nullptr && std::ranges::any_of(Objs, [Object](const DObject* Entry) {
			return Entry == Object;
		});
	}

	auto FDObjectArray::Compact() -> void
	{
		Objs.erase(
			std::remove(Objs.begin(), Objs.end(), nullptr),
			Objs.end()
		);
	}

	auto ResolveObjectHandle(FObjectHandle Handle) -> DObject*
	{
#if DURIN_WITH_OBJECT_HANDLE
		DObject* Object = Handle.Object;
#else
		DObject* Object = Handle;
#endif
		return GDObjectArray.Contains(Object) ? Object : nullptr;
	}
}
