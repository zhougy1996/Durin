#pragma once

#include "CoreDObjectAPI.h"
#include "Field.h"

namespace Doge
{
	class FProperty : public FField
	{
		DECLARE_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	public:
	};
}
