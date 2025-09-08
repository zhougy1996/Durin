#include "DObject/DObject.h"

#include "DObject/ObjectMacros.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"

CORE_API auto Z_Construct_DClass_DObject() -> DClass*;
CORE_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;

FClassRegistrationInfo Z_Registration_Info_DClass_DObject;

auto DObject::GetPrivateStaticClass() -> DClass*
{
	DClass*& Singleton = Z_Registration_Info_DClass_DObject.InnerSingleton;
	if (!Singleton)
	{
		Singleton = GetPrivateStaticClassBody("DObject");
	}
	return Singleton;
}

auto DObject::Register(FName InName) -> void
{
	AddObject(InName);
}

auto DObject::AddObject(FName InName) -> void
{
	NamePrivate = InName;
	GDObjectArray.Add(this);
}

struct Z_Construct_DClass_DObject_Statics
{
	static const DogeCodeGen::FClassParams ClassParams;
};

const DogeCodeGen::FClassParams Z_Construct_DClass_DObject_Statics::ClassParams = {
	&DObject::StaticClass,
	"DObject",
};


auto Z_Construct_DClass_DObject() -> DClass*
{
	DClass*& Singleton = Z_Registration_Info_DClass_DObject.OuterSingleton;
	if (!Singleton)
	{
		Singleton = DogeCodeGen::ConstructDClass(Z_Construct_DClass_DObject_Statics::ClassParams);
	}
	return Singleton;
}

auto Z_Construct_DClass_DObject_NoRegister() -> DClass*
{
	return DObject::GetPrivateStaticClass();
}

void DObjectForceRegistration(DObject* Object)
{
	Object->Register(Object->NamePrivate);
}
