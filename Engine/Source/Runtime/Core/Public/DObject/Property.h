#pragma once

#include "DObject/Field.h"

namespace Doge
{
	class FProperty : public FField
	{
		DECLARE_FIELD(FProperty, FField, EClassCastFlags::FProperty, CORE_API)
	public:
	};
}
