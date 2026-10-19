#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"

namespace Durin
{
	class DObject;

	// Runtime object capture boundary. Editor owns the current recording's lifetime.
	class ITransaction
	{
	public:
		virtual ~ITransaction() = default;
		virtual auto SaveObject(DObject* Object) -> void = 0;
		virtual auto ContainsObject(const DObject* Object) const -> bool = 0;
	};
}
