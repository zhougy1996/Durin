#include "DObject/DObjectGlobals.h"

#include "DObject/Class.h"

auto DogeCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
{
	const UTF8Char* ClassName = Params.ClassName;
	DClass* Class = Params.ClassNoRegisterFunc();

	DObjectForceRegistration(Class);
	return Class;
}
