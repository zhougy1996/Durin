#include "DObject/DObjectGlobals.h"

#include "DObject/Class.h"

auto FObjectInitializer::Get() -> const FObjectInitializer&
{
	static thread_local FObjectInitializer Instance;
	return Instance;
}

auto DogeCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
{
	const UTF8Char* ClassName = Params.ClassName;
	DClass* Class = Params.ClassNoRegisterFunc();

	DObjectForceRegistration(Class);
	return Class;
}
