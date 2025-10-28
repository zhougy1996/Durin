#include "DObject/Class.h"

IMPLEMENT_INTRINSIC_CLASS(DStructure, CORE_API, DObject, CORE_API, {})

IMPLEMENT_INTRINSIC_CLASS(DClass, CORE_API, DObject, CORE_API, {})

auto GetPrivateStaticClassBody(
	const UTF8Char* PackageName,
	const UTF8Char* Name,
	void(*RegisterNativeFunc)(),
	uint32 InSize,
	uint32 InAlignment,
	EClassFlags InClassFlags,
	DClass::ClassConstructorType InClassConstructor
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
	return Class;
}
