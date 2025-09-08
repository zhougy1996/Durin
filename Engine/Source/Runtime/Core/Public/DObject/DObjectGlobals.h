#pragma once

#include "DObject/Class.h"

namespace DogeCodeGen
{
struct FClassParams
{
	DClass::StaticClassFunctionType ClassNoRegisterFunc;
	const UTF8Char* ClassName;
};

CORE_API auto ConstructDClass(const FClassParams& Params) -> DClass*;
} // namespace DObjectGlobals