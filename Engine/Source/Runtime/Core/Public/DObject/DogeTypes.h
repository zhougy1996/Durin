#pragma once

#include "DObject/Property.h"

#define UE_API CORE_API

class FNumericProperty : public FProperty
{
	DECLARE_FIELD(FNumericProperty, FProperty, CASTCLASS_FNumericProperty, UE_API)
};
