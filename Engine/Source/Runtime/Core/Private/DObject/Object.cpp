#include "DObject/Object.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/DeferredRegistry.h"

struct FPendingRegistrantInfo
{
	const CharT* Name;
	const CharT* PackageName;
	FClassRegisterFunc StaticClassFn;

	FPendingRegistrantInfo(FClassRegisterFunc InStaticClassFn, const CharT* InPackageName, const CharT* InName)
		: Name(InName)
		, PackageName(InPackageName)
		, StaticClassFn(InStaticClassFn)
	{
	}

	static auto GetMap() -> std::unordered_map<DObject*, FPendingRegistrantInfo>&
	{
		static std::unordered_map<DObject*, FPendingRegistrantInfo> PendingRegistrantInfo;
		return PendingRegistrantInfo;
	}
};

struct FPendingRegistrant
{
	DObject* Object;
	FPendingRegistrant* NextAutoRegister;

	FPendingRegistrant(DObject* InObject)
		: Object(InObject)
		, NextAutoRegister(nullptr)
	{
	}
};

static FPendingRegistrant* GFirstPendingRegistrant = nullptr;
static FPendingRegistrant* GLastPendingRegistrant = nullptr;

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
DObject::DObject(EStaticConstructor, EObjectFlags InFlags)
	: ObjectFlags(InFlags)
{
}


auto DObject::Register(FClassRegisterFunc InStaticClassFn, const CharT* InPackageName, const CharT* InName) -> void
{
	// Add FPendingRegistrantInfo
	FPendingRegistrantInfo::GetMap().emplace(this, FPendingRegistrantInfo(InStaticClassFn, InPackageName, InName));

	// Add FPendingRegistrant to the linked list
	FPendingRegistrant* NewRegistrant = new FPendingRegistrant(this);
	if (GLastPendingRegistrant)
	{
		GLastPendingRegistrant->NextAutoRegister = NewRegistrant;
	}
	else
	{
		GFirstPendingRegistrant = NewRegistrant;
	}
	GLastPendingRegistrant = NewRegistrant;
}

auto DObject::DeferredRegister(DClass* InDClassStaticClass, const CharT* InPackageName, const CharT* InName) -> void
{
	check(!OuterPrivate);
	OuterPrivate = nullptr; // Packages are not implemented yet

	check(InDClassStaticClass);
	check(!ClassPrivate && "Class is already set for this class, which is unexpected in DeferredRegister");

	ClassPrivate = InDClassStaticClass;

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

static auto DequeuePendingAutoRegistrants(std::vector<FPendingRegistrant>& OutPendingRegistrants) -> void
{
	FPendingRegistrant* Current = GFirstPendingRegistrant;
	GFirstPendingRegistrant = nullptr;
	GLastPendingRegistrant = nullptr;

	while (Current)
	{
		OutPendingRegistrants.push_back(*Current);
		FPendingRegistrant* Next = Current->NextAutoRegister;
		delete Current;
		Current = Next;
	}
}

auto DObjectProcessRegistrants() -> void
{
	std::vector<FPendingRegistrant> PendingRegistrants;
	DequeuePendingAutoRegistrants(PendingRegistrants);
	for (size_t i = 0; i < PendingRegistrants.size(); ++i)
	{
		const FPendingRegistrant& Registrant = PendingRegistrants[i];

		// May enqueue more pending registrants, so process one at a time
		DObjectForceRegistration(Registrant.Object);

		// Should have been set by DeferredRegister()
		check(Registrant.Object->GetClass());

		// Register may have resulted in new pending registrants being enqueued, so dequeue those.
		DequeuePendingAutoRegistrants(PendingRegistrants);
	}
}

auto DObjectForceRegistration(DObject* Object) -> void
{
	std::unordered_map<DObject*, FPendingRegistrantInfo>& PendingRegistrants = FPendingRegistrantInfo::GetMap();
	// See if this object is pending registration
	auto It = PendingRegistrants.find(Object);
	if (It != PendingRegistrants.end())
	{
		const FPendingRegistrantInfo& Info = It->second;

		DClass* StaticClass = Info.StaticClassFn();
		const CharT* PackageName = Info.PackageName;
		const CharT* Name = Info.Name;
		Object->DeferredRegister(StaticClass, PackageName, Name);

		// Remove from pending registrants
		PendingRegistrants.erase(It);
	}
}

auto RegisterCompiledInInfo(FClassRegisterFunc InOuterRegister, FClassRegisterFunc InInnerRegister, const U8Char* InName, FClassRegistrationInfo& InInfo) -> void
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
