#include "DObject/DObject.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/DeferredRegistry.h"

class DClass;

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

auto DObjectForceRegistration(DObject* Object) -> void
{
	Object->Register(Object->NamePrivate);
}

auto RegisterCompiledInInfo(FClassRegisterFunc InOuterRegister, FClassRegisterFunc InInnerRegister, const UTF8Char* InName, FClassRegistrationInfo& InInfo) -> void
{
	check(InOuterRegister);
	check(InInnerRegister);
	FClassDeferredRegistry::Get().AddRegistration(InOuterRegister, InInnerRegister, InName, InInfo);
}

auto RegisterCompiledInInfo(const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo) -> void
{
	for (size_t Index = 0; Index < NumClassInfo; ++Index)
	{
		const FClassRegisterCompiledInInfo& Info = ClassInfo[Index];
		RegisterCompiledInInfo(Info.OuterRegister, Info.InnerRegister, Info.Name, *Info.Info);
	}
}

static auto RegisterAllCompiledInClasses() -> void
{
	std::vector<FClassDeferredRegistry::FRegistrant>& Registrations = FClassDeferredRegistry::Get().GetRegistrations();
	for (FClassDeferredRegistry::FRegistrant& Registrant : Registrations)
	{
		if (!Registrant.Info->InnerSingleton)
		{
			Registrant.Info->InnerSingleton = Registrant.InnerRegister();
		}
	}
}

static auto LoadAllCompiledInDefaultProperties() -> void
{
	std::vector<FClassDeferredRegistry::FRegistrant>& Registrations = FClassDeferredRegistry::Get().GetRegistrations();
	for (FClassDeferredRegistry::FRegistrant& Registrant : Registrations)
	{
		if (!Registrant.Info->OuterSingleton)
		{
			Registrant.Info->OuterSingleton = Registrant.OuterRegister();
		}
	}
}

auto ProcessNewlyLoadedDObjects() -> void
{
	FClassDeferredRegistry& ClassRegistry = FClassDeferredRegistry::Get();

	RegisterAllCompiledInClasses();
	LoadAllCompiledInDefaultProperties();

	ClassRegistry.ClearRegistrations();
}

// Registration
static FRegisterCompiledInInfo Z_CompiledInDeferRegistration_DObject(
	&Z_Construct_DClass_DObject,
	&Z_Construct_DClass_DObject_NoRegister,
	"DObject",
	Z_Registration_Info_DClass_DObject
);
