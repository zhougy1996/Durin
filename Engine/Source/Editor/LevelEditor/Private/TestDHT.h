#pragma once

#include "DObject/DogeTypes.h"
#include "DObject/ObjectMacros.h"

#include "TestDHT.gen.h"

DCLASS(AAA, bbb=ccc)
class LEVELEDITOR_API TestDHT : public DObject
{
private:
	DPROPERTY(bbb = ccc)
	int a;

	DPROPERTY(zzz)
	float a2;
};

DCLASS(bbb = ccc)
class LEVELEDITOR_API TestDHT2 : public DObject
{
private:
	DPROPERTY(bbb = ccc)
	int a;

	DPROPERTY(zzz)
	float a2;
};


