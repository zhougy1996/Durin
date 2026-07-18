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
		COREDOBJECT_API auto GetObjectsWithOuter(const DObject* Outer, bool bIncludeGarbage = false) const -> std::vector<DObject*>;

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
		// This is a non-owning query index. OuterPrivate remains the hierarchy source
		// of truth, including for top-level objects indexed under nullptr.
		std::unordered_map<const DObject*, std::vector<DObject*>> OuterToObjects;
		uint64 GarbageObjectCount = 0;

		auto ReparentObject(DObject* Object, DObject* NewOuter) -> void;
		auto AddToOuterIndex(DObject* Object, const DObject* Outer) -> void;
		auto RemoveFromOuterIndex(DObject* Object, const DObject* Outer) -> void;

		friend class DObject;
	};

	extern COREDOBJECT_API FDObjectArray GDObjectArray;
}
