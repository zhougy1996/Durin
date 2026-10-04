#pragma once

#include "CoreMinimal.h"

#include <Foundation/Foundation.hpp>

namespace Durin
{
	// Scope native temporaries on whichever worker or callback executes Metal work.
	class FMetalAutoreleasePool final
	{
	public:
		FMetalAutoreleasePool() : Pool(NS::TransferPtr(NS::AutoreleasePool::alloc()->init())) {}
		FMetalAutoreleasePool(const FMetalAutoreleasePool&) = delete;
		auto operator=(const FMetalAutoreleasePool&) -> FMetalAutoreleasePool& = delete;

	private:
		NS::SharedPtr<NS::AutoreleasePool> Pool;
	};
}
