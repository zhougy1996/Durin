#include "DObject/Class.h"

auto GetPrivateStaticClassBody(
	const UTF8Char* Name,
	DClass::ClassConstructorType InClassConstructor
) -> DClass*
{
	DClass* Class = new DClass(Name, InClassConstructor);
	return Class;
}
