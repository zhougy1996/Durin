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
	auto GetObj() -> DObject* { return Obj; }

	static CORE_API auto Get() -> const FObjectInitializer&;

	DObject* Obj;

	FStaticConstructObjectParameters Params;
};
	
template <typename T>
auto NewObject(DObject* Outer, FName Name) -> T*
{
	static_assert(std::is_base_of<DObject, T>::value, "T must be derived from DObject");

	// Allocate memory and zero it out
	DObject* Obj = nullptr;
	Obj = (DObject*)std::malloc(sizeof(T));
	std::memset(Obj, 0, sizeof(T));

	FObjectInitializer ObjectInitializer;
	ObjectInitializer.Obj = Obj;
	ObjectInitializer.Params = FStaticConstructObjectParameters{T::StaticClass(), Outer, Name};

	new (Obj) DObject(ObjectInitializer);

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
