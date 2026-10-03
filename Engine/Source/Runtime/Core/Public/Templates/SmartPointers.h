#pragma once

#include "HAL/Platform.h"

#include "Misc/CoreStd.h"

namespace Durin
{
	template<typename OtherType>
	FORCEINLINE auto SharedThis(OtherType* ThisPtr) -> std::shared_ptr<OtherType>
	{
		return std::static_pointer_cast<OtherType>(ThisPtr->shared_from_this());
	}
}