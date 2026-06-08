#include "DObject/Class.h"

namespace Durin
{
	COREDOBJECT_API DClass* Z_Construct_DClass_DObject();
	COREDOBJECT_API DClass* Z_Construct_DClass_DStructure();
	COREDOBJECT_API DClass* Z_Construct_DClass_DClass();
}

namespace Durin
{
	IMPLEMENT_INTRINSIC_CLASS(DStructure, COREDOBJECT_API, DObject, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DClass, COREDOBJECT_API, DObject, COREDOBJECT_API, {})

	auto DStructure::RegisterDependencies() -> void
	{
		if (SuperStructure)
		{
			SuperStructure->RegisterDependencies();
		}
	}

	auto GetPrivateStaticClassBody(
		const char* PackageName,
		const char* Name,
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
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DObject()
{
	return Durin::Z_Construct_DClass_DObject();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DStructure()
{
	return Durin::Z_Construct_DClass_DStructure();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DClass()
{
	return Durin::Z_Construct_DClass_DClass();
}
