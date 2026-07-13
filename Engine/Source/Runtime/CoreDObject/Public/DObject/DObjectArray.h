#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/ObjectHandle.h"

namespace Durin
{
	class DObject;

	class FDObjectArray
	{
	public:
		COREDOBJECT_API auto Add(DObject* ObjToAdd) -> void;
		COREDOBJECT_API auto Remove(DObject* ObjToRemove) -> void;
		COREDOBJECT_API auto Contains(const DObject* Object) const -> bool;
		COREDOBJECT_API auto MakeHandle(const DObject* Object) const -> FObjectHandle;
		COREDOBJECT_API auto Resolve(FObjectHandle Handle) const -> DObject*;

		auto GetNum() const -> uint64 { return static_cast<uint64>(Objects.size()); }
		auto GetGarbageNum() const -> uint64 { return GarbageObjectCount; }
		auto GetAll() const -> const std::vector<DObject*>& { return Objects; }
		auto Snapshot() const -> std::vector<DObject*> { return Objects; }

		COREDOBJECT_API auto NotifyObjectMarkedGarbage() -> void;

	private:
		struct FObjectSlot
		{
			DObject* Object = nullptr;
			uint32 Generation = 1;
			uint32 DenseIndex = 0;
		};

		std::vector<FObjectSlot> Slots;
		std::vector<uint32> FreeSlots;
		std::vector<DObject*> Objects;
		std::unordered_map<const DObject*, uint32> ObjectToSlot;
		uint64 GarbageObjectCount = 0;
	};

	extern COREDOBJECT_API FDObjectArray GDObjectArray;
}
