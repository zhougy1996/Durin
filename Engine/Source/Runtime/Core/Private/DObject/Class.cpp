#include "DObject/Class.h"

auto GetPrivateStaticClassBody(
	const UTF8Char* Name
) -> DClass*
{
	DClass* Class = new DClass();
	Class->Rename(FName(Name));
	return Class;
}
