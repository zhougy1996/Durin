#pragma once

#include <cstddef>

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;

#ifndef DURIN_WITH_OBJECT_HANDLE
#if defined(DURIN_BUILD_SHIPPING) && DURIN_BUILD_SHIPPING
#define DURIN_WITH_OBJECT_HANDLE 0
#else
#define DURIN_WITH_OBJECT_HANDLE 1
#endif
#endif

#if DURIN_WITH_OBJECT_HANDLE

	struct FObjectHandle
	{
		FObjectHandle() = default;
		FObjectHandle(std::nullptr_t) {}
		explicit FObjectHandle(DObject* InObject)
			: Object(InObject)
		{
		}

		DObject* Object = nullptr;
	};

	inline auto MakeObjectHandle(DObject* Object) -> FObjectHandle
	{
		return FObjectHandle(Object);
	}

	inline auto IsObjectHandleNull(FObjectHandle Handle) -> bool
	{
		return Handle.Object == nullptr;
	}

	inline auto ResolveObjectHandle(FObjectHandle Handle) -> DObject*
	{
		return Handle.Object;
	}

#else

	using FObjectHandle = DObject*;

	inline auto MakeObjectHandle(DObject* Object) -> FObjectHandle
	{
		return Object;
	}

	inline auto IsObjectHandleNull(FObjectHandle Handle) -> bool
	{
		return Handle == nullptr;
	}

	inline auto ResolveObjectHandle(FObjectHandle Handle) -> DObject*
	{
		return Handle;
	}

#endif
}
