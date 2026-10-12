#pragma once

#include "CoreMinimal.h"


namespace Durin
{
	// Cache capacity is also used for request admission backpressure.
	class FRHIResourceCapacityError : public std::runtime_error
	{
	public:
		explicit FRHIResourceCapacityError(const char* Message) : std::runtime_error(Message) {}
	};
}
