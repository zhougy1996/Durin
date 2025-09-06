#pragma once

#include "DObject/DogeTypes.h"
#include "DObject/ObjectMacros.h"

#include "TestDHT.gen.h"

DCLASS(AAA, bbb = ccc)
class LEVELEDITOR_API TestDHT : DObject
{
	GENERATED_BODY()

public:
	DFUNCTION()
	void Func() {};

private:
	DPROPERTY(bbb = ccc)
	const std::vector<const DObject*> a;

	DPROPERTY(zzz)
	int32 a2;
};
