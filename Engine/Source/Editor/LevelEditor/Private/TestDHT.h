#pragma once

#include "DObject/DogeTypes.h"
#include "DObject/ObjectMacros.h"


#include "TestDHT.gen.h"

namespace Doge
{
	DCLASS(AAA, bbb = ccc)
	class LEVELEDITOR_API TestDHT : DObject
	{
		GENERATED_BODY()

	public:
		DFUNCTION()
		void Func() {};


	private:
		const std::vector<const DObject*> a;

		DPROPERTY(zzz)
		uint16 a1;

		DPROPERTY(zzz)
		int32 a2;
	};
}