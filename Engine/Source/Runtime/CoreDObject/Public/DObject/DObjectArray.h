#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;

	class FDObjectArray
	{
	public:

		auto Add(DObject* ObjToAdd) -> void { Objs.push_back(ObjToAdd); }

		COREDOBJECT_API auto Remove(DObject* ObjToRemove) -> void;

		COREDOBJECT_API auto Compact() -> void;

		auto GetNum() const -> uint64 { return Objs.size(); }

		auto GetAll() const -> const std::vector<DObject*>& { return Objs; }

		auto Snapshot() const -> std::vector<DObject*> { return Objs; }

	private:
		std::vector<DObject*> Objs;
	};

	extern COREDOBJECT_API FDObjectArray GDObjectArray;
}
