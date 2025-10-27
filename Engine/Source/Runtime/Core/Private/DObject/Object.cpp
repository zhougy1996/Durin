#include "DObject/Object.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/DeferredRegistry.h"

IMPLEMENT_INTRINSIC_CLASS(DObject, CORE_API, DObject, CORE_API, {})

DObject::DObject()
{
}
DObject::DObject(const FObjectInitializer& ObjectInitializer)
{
}

DObject::DObject(DClass* InClass, DObject* InOuter, FName InName)
	: ClassPrivate(InClass)
	, OuterPrivate(InOuter)
	, NamePrivate(InName)
{
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
