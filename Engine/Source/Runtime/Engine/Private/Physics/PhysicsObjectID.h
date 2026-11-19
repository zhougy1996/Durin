#pragma once

#include "CoreMinimal.h"
#include "DObject/DObjectArray.h"

namespace Durin::PhysicsPrivate
{
	// Physics compares these process-local values without depending on CoreDObject.
	inline auto MakeObjectID(const DObject* Object) -> uint64
	{
		const FObjectHandle Handle = GDObjectArray.MakeHandle(Object);
		if (IsObjectHandleNull(Handle)) return 0;
		return (static_cast<uint64>(Handle.ObjectSerialNumber) << 32) | Handle.ObjectIndex;
	}

	inline auto ResolveObjectID(uint64 ObjectID) -> DObject*
	{
		return ResolveObjectHandle({static_cast<uint32>(ObjectID), static_cast<uint32>(ObjectID >> 32)});
	}
}
