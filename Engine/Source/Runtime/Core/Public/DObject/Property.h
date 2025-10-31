#pragma once

#include "DObject/Field.h"

class FProperty : public FField
{
	DECLARE_FIELD(FProperty, FField, EClassCastFlags::FProperty, CORE_API)
public:
};
