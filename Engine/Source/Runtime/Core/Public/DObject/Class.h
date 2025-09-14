#pragma once

#include "DObject/Object.h"

class DClass : public DObject
{
public:
	using StaticClassFunctionType = DClass* (*)();
};

CORE_API auto GetPrivateStaticClassBody(
	const UTF8Char* Name
) -> DClass*;

