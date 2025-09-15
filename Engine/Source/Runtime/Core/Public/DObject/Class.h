#pragma once

#include "DObject/Object.h"

class FObjectInitializer;

class DClass : public DObject
{
public:

	using StaticClassFunctionType = DClass* (*)();
	using ClassConstructorType = void (*)(const FObjectInitializer&);

	// the Outer is nullptr now , it will be set to a package later maybe
	DClass
	(
		FName InName,
		ClassConstructorType InClassConstructor
	)
		: DObject(this, nullptr, InName)
		, ClassConstructor(InClassConstructor)
	{
	}

	ClassConstructorType ClassConstructor;
};

template<class T>
void InternalConstructor(const FObjectInitializer& X)
{
	T::__DefaultConstructor(X);
}

CORE_API auto GetPrivateStaticClassBody(
	const UTF8Char* Name,
	DClass::ClassConstructorType InClassConstructor
) -> DClass*;

