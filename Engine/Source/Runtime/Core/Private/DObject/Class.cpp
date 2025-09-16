#include "DObject/Class.h"

IMPLEMENT_INTRINSIC_CLASS(DClass, CORE_API, DObject, CORE_API, {})

DClass::DClass(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

auto GetPrivateStaticClassBody(
	const UTF8Char* Name,
	DClass::ClassConstructorType InClassConstructor
) -> DClass*
{
	DClass* Class = new DClass(Name, InClassConstructor);
	return Class;
}


