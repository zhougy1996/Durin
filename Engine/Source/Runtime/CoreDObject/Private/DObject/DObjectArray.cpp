#include "DObject/DObjectArray.h"

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

	auto FDObjectArray::Compact() -> void
	{
		Objs.erase(
			std::remove(Objs.begin(), Objs.end(), nullptr),
			Objs.end()
		);
	}
}
