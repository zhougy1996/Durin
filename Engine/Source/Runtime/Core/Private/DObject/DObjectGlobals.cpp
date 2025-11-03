#include "DObject/DObjectGlobals.h"

#include "DObject/Class.h"

auto FObjectInitializer::Get() -> const FObjectInitializer&
{
	static thread_local FObjectInitializer Instance;
	return Instance;
}

auto StaticAllocateObject(DClass* Class, DObject* Outer, FName Name, size_t Size) -> DObject*
{
	// Allocate memory and zero it out
	DObject* Obj = nullptr;
	Obj = static_cast<DObject*>(std::malloc(Size));
	assert(Obj && "Memory allocation failed");
	new (Obj) DObject(Class, Outer, Name);

	return Obj;
}

auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*
{
	DObject* Obj = StaticAllocateObject(Params.Class, Params.Outer, Params.Name, Params.Size);

	DClass* InClass = Params.Class;

	assert(InClass && InClass->ClassConstructor);

	FObjectInitializer ObjectInitializer;
	ObjectInitializer.Obj = Obj;

	InClass->ClassConstructor(ObjectInitializer);

	return Obj;
}

auto DogeCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
{
	DClass* Class = Params.ClassNoRegisterFunc();

	DObjectForceRegistration(Class);
	return Class;
}
