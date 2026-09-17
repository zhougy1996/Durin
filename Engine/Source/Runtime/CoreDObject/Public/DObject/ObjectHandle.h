#pragma once

#include <cstddef>
#include <limits>

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;

	// Identifies an object-array slot and object serial number so stale handles fail resolution.
	struct FObjectHandle
	{
		static constexpr uint32 InvalidIndex = std::numeric_limits<uint32>::max();

		FObjectHandle() = default;
		FObjectHandle(std::nullptr_t) {}
		FObjectHandle(uint32 InObjectIndex, uint32 InObjectSerialNumber)
			: ObjectIndex(InObjectIndex), ObjectSerialNumber(InObjectSerialNumber)
		{
		}

		uint32 ObjectIndex = InvalidIndex;
		uint32 ObjectSerialNumber = 0;

		friend auto operator==(const FObjectHandle&, const FObjectHandle&) -> bool = default;
	};

	static_assert(sizeof(FObjectHandle) == sizeof(uint64));

	COREDOBJECT_API auto MakeObjectHandle(DObject* Object) -> FObjectHandle;

	inline auto IsObjectHandleNull(FObjectHandle Handle) -> bool
	{
		return Handle.ObjectIndex == FObjectHandle::InvalidIndex;
	}

	COREDOBJECT_API auto ResolveObjectHandle(FObjectHandle Handle) -> DObject*;
}
