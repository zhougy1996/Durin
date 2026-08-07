#include "DObject/DObjectArray.h"

#include "DObject/Object.h"

namespace Durin
{
	FDObjectArray GDObjectArray;

	auto FDObjectArray::Add(DObject* ObjToAdd) -> void
	{
		check(ObjToAdd);
		check(!Contains(ObjToAdd));

		uint32 SlotIndex;
		if (FreeSlots.empty())
		{
			SlotIndex = static_cast<uint32>(Slots.size());
			Slots.emplace_back();
		}
		else
		{
			SlotIndex = FreeSlots.back();
			FreeSlots.pop_back();
		}

		FObjectSlot& Slot = Slots[SlotIndex];
		Slot.Object = ObjToAdd;
		Slot.DenseIndex = static_cast<uint32>(Objects.size());
		Objects.push_back(ObjToAdd);
		ObjectToSlot.emplace(ObjToAdd, SlotIndex);
		AddToOuterIndex(ObjToAdd, ObjToAdd->GetOuter());
	}

	auto FDObjectArray::Remove(DObject* ObjToRemove) -> void
	{
		if (!ObjToRemove) return;
		auto It = ObjectToSlot.find(ObjToRemove);
		if (It == ObjectToSlot.end()) return;

		const uint32 SlotIndex = It->second;
		FObjectSlot& Slot = Slots[SlotIndex];
		const auto ChildrenIt = OuterToObjects.find(ObjToRemove);
		check(ChildrenIt == OuterToObjects.end() || ChildrenIt->second.empty());
		RemoveFromOuterIndex(ObjToRemove, ObjToRemove->GetOuter());
		const uint32 DenseIndex = Slot.DenseIndex;
		DObject* MovedObject = Objects.back();
		Objects[DenseIndex] = MovedObject;
		Objects.pop_back();

		if (MovedObject != ObjToRemove)
		{
			const uint32 MovedSlotIndex = ObjectToSlot.find(MovedObject)->second;
			Slots[MovedSlotIndex].DenseIndex = DenseIndex;
		}

		if (ObjToRemove->IsGarbage())
		{
			check(GarbageObjectCount > 0);
			--GarbageObjectCount;
		}

		ObjectToSlot.erase(It);
		Slot.Object = nullptr;
		Slot.DenseIndex = 0;
		Slot.OuterIndex = std::numeric_limits<uint32>::max();
		++Slot.Generation;
		if (Slot.Generation == 0) Slot.Generation = 1;
		FreeSlots.push_back(SlotIndex);
	}

	auto FDObjectArray::Contains(const DObject* Object) const -> bool
	{
		return Object != nullptr && ObjectToSlot.contains(Object);
	}

	auto FDObjectArray::MakeHandle(const DObject* Object) const -> FObjectHandle
	{
		if (!Object) return nullptr;
		auto It = ObjectToSlot.find(Object);
		if (It == ObjectToSlot.end()) return nullptr;
		const uint32 SlotIndex = It->second;
		return FObjectHandle(SlotIndex, Slots[SlotIndex].Generation);
	}

	auto FDObjectArray::Resolve(FObjectHandle Handle) const -> DObject*
	{
		if (IsObjectHandleNull(Handle) || Handle.Index >= Slots.size()) return nullptr;
		const FObjectSlot& Slot = Slots[Handle.Index];
		return Slot.Object && Slot.Generation == Handle.Generation ? Slot.Object : nullptr;
	}

	auto FDObjectArray::GetAll(EObjectQueryScope Scope) const -> std::vector<DObject*>
	{
		if (Scope == EObjectQueryScope::IncludeTemplates) return Objects;

		std::vector<DObject*> Result;
		Result.reserve(Objects.size());
		for (DObject* Object : Objects)
		{
			if (!Object->IsTemplateObject()) Result.push_back(Object);
		}
		return Result;
	}

	auto FDObjectArray::GetObjectsWithOuter(
		const DObject* Outer,
		EObjectQueryScope Scope,
		bool bIncludeGarbage
	) const -> std::vector<DObject*>
	{
		const auto It = OuterToObjects.find(Outer);
		if (It == OuterToObjects.end()) return {};

		std::vector<DObject*> Result;
		Result.reserve(It->second.size());
		for (DObject* Object : It->second)
		{
			if ((bIncludeGarbage || !Object->IsGarbage())
				&& (Scope == EObjectQueryScope::IncludeTemplates || !Object->IsTemplateObject()))
			{
				Result.push_back(Object);
			}
		}
		return Result;
	}

	auto FDObjectArray::ReparentObject(DObject* Object, DObject* NewOuter) -> void
	{
		check(Object);
		check(Contains(Object));
		if (Object->OuterPrivate == NewOuter) return;

		RemoveFromOuterIndex(Object, Object->OuterPrivate);
		Object->OuterPrivate = NewOuter;
		AddToOuterIndex(Object, NewOuter);
	}

	auto FDObjectArray::AddToOuterIndex(DObject* Object, const DObject* Outer) -> void
	{
		check(Object);
		auto& ObjectsWithOuter = OuterToObjects[Outer];
		auto SlotIt = ObjectToSlot.find(Object);
		check(SlotIt != ObjectToSlot.end());
		FObjectSlot& Slot = Slots[SlotIt->second];
		check(Slot.OuterIndex == std::numeric_limits<uint32>::max());
		Slot.OuterIndex = static_cast<uint32>(ObjectsWithOuter.size());
		ObjectsWithOuter.push_back(Object);
	}

	auto FDObjectArray::RemoveFromOuterIndex(DObject* Object, const DObject* Outer) -> void
	{
		auto It = OuterToObjects.find(Outer);
		check(It != OuterToObjects.end());
		auto SlotIt = ObjectToSlot.find(Object);
		check(SlotIt != ObjectToSlot.end());
		FObjectSlot& Slot = Slots[SlotIt->second];
		auto& ObjectsWithOuter = It->second;
		check(Slot.OuterIndex < ObjectsWithOuter.size());
		check(ObjectsWithOuter[Slot.OuterIndex] == Object);

		const uint32 RemoveIndex = Slot.OuterIndex;
		DObject* MovedObject = ObjectsWithOuter.back();
		ObjectsWithOuter[RemoveIndex] = MovedObject;
		ObjectsWithOuter.pop_back();
		Slot.OuterIndex = std::numeric_limits<uint32>::max();

		if (MovedObject != Object)
		{
			auto MovedSlotIt = ObjectToSlot.find(MovedObject);
			check(MovedSlotIt != ObjectToSlot.end());
			Slots[MovedSlotIt->second].OuterIndex = RemoveIndex;
		}

		if (ObjectsWithOuter.empty()) OuterToObjects.erase(It);
	}

	auto FDObjectArray::NotifyObjectMarkedGarbage() -> void
	{
		++GarbageObjectCount;
	}

	auto MakeObjectHandle(DObject* Object) -> FObjectHandle
	{
		return GDObjectArray.MakeHandle(Object);
	}

	auto ResolveObjectHandle(FObjectHandle Handle) -> DObject*
	{
		return GDObjectArray.Resolve(Handle);
	}
}
