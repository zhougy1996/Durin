#pragma once

#include "DObject/Object.h"

class FObjectInitializer;

class DClass : public DObject
{
public:
	// the Outer is nullptr now , it will be set to a package later maybe
	DClass(FName InName)
		: DObject(this, nullptr, InName)
	{
	}

	template<class T>
	void InternalConstructor(const FObjectInitializer& X)
	{
		T::__DefaultConstructor(X);
	}

	using StaticClassFunctionType = DClass* (*)();
};

CORE_API auto GetPrivateStaticClassBody(
	const UTF8Char* Name
) -> DClass*;

