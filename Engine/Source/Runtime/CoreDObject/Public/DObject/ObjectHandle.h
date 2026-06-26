#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;

	using FObjectHandle = DObject*;

	inline auto IsObjectHandleNull(FObjectHandle Handle) -> bool
	{
		return Handle == nullptr;
	}

	inline auto ResolveObjectHandle(FObjectHandle Handle) -> DObject*
	{
		return Handle;
	}
}
