#pragma once

#include <memory>

namespace Doge
{
	template<typename OtherType>
	FORCEINLINE auto SharedThis(OtherType* ThisPtr) -> std::shared_ptr<OtherType>
	{
		return std::static_pointer_cast<OtherType>(ThisPtr->shared_from_this());
	}
}