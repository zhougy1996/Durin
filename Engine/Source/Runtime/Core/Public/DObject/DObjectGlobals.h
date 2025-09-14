#pragma once

#include "DObject/Class.h"

struct FStaticConstructObjectParameters
{
	DClass* Class = nullptr;
	DObject* Outer = nullptr;
	FName Name;
};

class FObjectInitializer
{
public:
	FStaticConstructObjectParameters Params;
};
	
template <typename T>
auto NewObject(DObject* Outer, FName Name) -> T*
{
	static_assert(std::is_base_of<DObject, T>::value, "T must be derived from DObject");

	FObjectInitializer ObjectInitializer;
	ObjectInitializer.Params = FStaticConstructObjectParameters{T::StaticClass(), Outer, Name};

	DObject* Obj = nullptr;
	Obj = (DObject*)std::malloc(sizeof(T));
	std::memset(Obj, 0, sizeof(T));
	new (Obj) DObject(ObjectInitializer); // placement new

	// Obj->Register(Name);
	return (T*)Obj;
}

namespace DogeCodeGen
{

struct FClassParams
{
	DClass::StaticClassFunctionType ClassNoRegisterFunc;
	const UTF8Char* ClassName;
};

CORE_API auto ConstructDClass(const FClassParams& Params) -> DClass*;

} // namespace DogeCodeGen
