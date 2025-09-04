#pragma once

#include "DObject/DogeTypes.h"
#include "DObject/ObjectMacros.h"

#include "TestDHT.gen.h"

DCLASS(AAA, bbb=ccc)
class LEVELEDITOR_API TestDHT : public DObject
{
private:
	DPROPERTY()
	int32 a;
};

