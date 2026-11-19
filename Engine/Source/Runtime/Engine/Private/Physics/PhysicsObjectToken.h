#pragma once

#include "CoreMinimal.h"
#include "DObject/DObjectArray.h"

namespace Durin::PhysicsPrivate
{
	// Physics compares these process-local values without depending on CoreDObject.
	inline auto MakeObjectToken(const DObject* Object) -> uint64
	{
		const FObjectHandle Handle = GDObjectArray.MakeHandle(Object);
		if (IsObjectHandleNull(Handle)) return 0;
		return (static_cast<uint64>(Handle.ObjectSerialNumber) << 32) | Handle.ObjectIndex;
	}

	inline auto ResolveObjectToken(uint64 Token) -> DObject*
	{
		return ResolveObjectHandle({static_cast<uint32>(Token), static_cast<uint32>(Token >> 32)});
	}
}
