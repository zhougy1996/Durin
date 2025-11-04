#include "DObject/Class.h"

IMPLEMENT_INTRINSIC_CLASS(DStructure, CORE_API, DObject, CORE_API, {})

IMPLEMENT_INTRINSIC_CLASS(DClass, CORE_API, DObject, CORE_API, {})

auto DStructure::RegisterDependencies() -> void
{
	if (SuperStructure)
	{
		SuperStructure->RegisterDependencies();
	}
}

auto GetPrivateStaticClassBody(
	const UTF8Char* PackageName,
	const UTF8Char* Name,
	DClass*& ReturnClass,
	void(*RegisterNativeFunc)(),
	uint32 InSize,
	uint32 InAlignment,
	EClassFlags InClassFlags,
	DClass::ClassConstructorType InClassConstructor,
	DClass::StaticClassFunctionType InSuperClassFn
) -> DClass*
{
	auto* Class = new DClass(
		EC_StaticConstructor,
		FName(Name),
		InSize,
		InAlignment,
		EObjectFlags::NoFlags,
		InClassFlags,
		EClassCastFlags::DClass,
		InClassConstructor
	);

	ReturnClass = Class; // assign before setting superclass to handle circular dependencies

	DClass* SuperClass = InSuperClassFn ? InSuperClassFn() : nullptr;
	Class->SetSuperStructure(SuperClass);

	Class->Register(DClass::StaticClass, PackageName, Name);

	return Class;
}
