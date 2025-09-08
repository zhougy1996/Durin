#include "DObject/Class.h"

auto GetPrivateStaticClassBody(
	const UTF8Char* Name
) -> DClass*
{
	DClass* Class = new DClass();
	Class->SetName(FName(Name));
	return Class;
}
