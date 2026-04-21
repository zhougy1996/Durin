#pragma once

#include "CoreDObject/API.h"

namespace Doge
{
	class DObject;

	class FDObjectArray
	{
	public:

		auto Add(DObject* ObjToAdd) -> void { Objs.push_back(ObjToAdd); }

		auto GetNum() const -> uint64 { return Objs.size(); }

		auto GetAll() -> const std::vector<DObject*>& { return Objs; }

	private:
		std::vector<DObject*> Objs;
	};

	extern COREDOBJECT_API FDObjectArray GDObjectArray;
}