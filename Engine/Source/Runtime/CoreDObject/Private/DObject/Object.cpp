#include "DObject/Object.h"

#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "DeferredRegistry.h"

namespace Durin
{
	struct FPendingRegistrantInfo
	{
		std::string Name;
		std::string PackageName;
		FClassRegisterFunc StaticClassFn;

		FPendingRegistrantInfo(FClassRegisterFunc InStaticClassFn, const char* InPackageName, const char* InName)
			// Registration is intentionally deferred, so these names must not borrow
			// caller buffers that may disappear before DObjectForceRegistration runs.
			: Name(InName ? InName : "")
			, PackageName(InPackageName ? InPackageName : "")
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

	FClassRegistrationInfo Z_Registration_Info_DClass_DObject;

	struct Z_Construct_DClass_DObject_Statics
	{
		static DClass* Construct()
		{
			DClass* Class = DObject::StaticClass();
			DObjectForceRegistration(Class);
			DObject::IntrinsicClassInit(Class);
			return Class;
		}
	};

	void DObject::IntrinsicClassInit(DClass* Class)
	{
		(void)Class;
	}

	COREDOBJECT_API DClass* Z_Construct_DClass_DObject()
	{
		if (!Z_Registration_Info_DClass_DObject.OuterSingleton)
		{
			Z_Registration_Info_DClass_DObject.OuterSingleton = Z_Construct_DClass_DObject_Statics::Construct();
		}
		check(Z_Registration_Info_DClass_DObject.OuterSingleton->GetClass());
		return Z_Registration_Info_DClass_DObject.OuterSingleton;
	}

	COREDOBJECT_API DClass* DObject::GetPrivateStaticClass()
	{
		if (!Z_Registration_Info_DClass_DObject.InnerSingleton)
		{
			GetPrivateStaticClassBody(
				STR(""),
				STR("DObject"),
				Z_Registration_Info_DClass_DObject.InnerSingleton,
				nullptr,
				sizeof(DObject),
				alignof(DObject),
				EClassFlags::None,
				(DClass::ClassConstructorType)InternalConstructor<DObject>,
				nullptr
			);
		}
		return Z_Registration_Info_DClass_DObject.InnerSingleton;
	}

	static FClassRegisterCompiledInInfo Z_AutoRegister_DObject(
		&Z_Construct_DClass_DObject,
		&DObject::StaticClass,
		STR("DObject"),
		&Z_Registration_Info_DClass_DObject
	);

	DObject::DObject()
	{
	}

	DObject::DObject(const FObjectInitializer& ObjectInitializer)
		: ClassPrivate(ObjectInitializer.Class)
		, OuterPrivate(ObjectInitializer.Outer)
		, NamePrivate(ObjectInitializer.Name)
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


	auto DObject::Register(FClassRegisterFunc InStaticClassFn, const char* InPackageName, const char* InName) -> void
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

	auto DObject::DeferredRegister(DClass* InDClassStaticClass, const char* InPackageName, const char* InName) -> void
	{
		check(!OuterPrivate);
		check(InDClassStaticClass);
		check(!ClassPrivate && "Class is already set for this class, which is unexpected in DeferredRegister");

		ClassPrivate = InDClassStaticClass;

		AddObject(InName);
		if (InPackageName && InPackageName[0] != '\0')
		{
			constexpr std::string_view CppPrefix = "/Cpp/";
			const std::string_view PackageName(InPackageName);
			check(PackageName.starts_with(CppPrefix));
			SetOuterPrivate(FindOrCreateCppPackage(FName(PackageName.substr(CppPrefix.size()))));
		}
	}

	auto DObject::AddObject(FName InName) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		NamePrivate = InName;
		GDObjectArray.Add(this);
	}

	auto DObject::SetOuterPrivate(DObject* NewOuter) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (OuterPrivate == NewOuter) return;

		// Outer is structural hierarchy, so accepting a descendant would make every
		// outer-chain query and ownership-tree traversal non-terminating.
		for (const DObject* Current = NewOuter; Current; Current = Current->GetOuter())
		{
			check(Current != this);
		}

		if (GDObjectArray.Contains(this))
		{
			GDObjectArray.ReparentObject(this, NewOuter);
			return;
		}
		OuterPrivate = NewOuter;
	}

	auto DObject::GetOutermost() const -> DObject*
	{
		const DObject* Current = this;
		while (Current->GetOuter()) Current = Current->GetOuter();
		return const_cast<DObject*>(Current);
	}

	auto DObject::GetPackage() const -> DPackage*
	{
		return Cast<DPackage>(GetOutermost());
	}

	auto DObject::GetObjectPath() const -> std::string
	{
		const DPackage* Package = GetPackage();
		if (!Package) return GetName();
		if (this == Package) return Package->GetPackagePath();
		if (Package->IsCppPackage()) return Package->GetPackagePath() + "." + GetName();
		if (this == Package->GetAsset()) return Package->GetPackagePath();

		std::vector<std::string> Segments;
		for (const DObject* Current = this; Current && Current != Package->GetAsset() && Current != Package; Current = Current->GetOuter())
		{
			Segments.push_back(Current->GetName());
		}
		std::ranges::reverse(Segments);
		std::string Result = Package->GetPackagePath() + ":";
		for (size_t Index = 0; Index < Segments.size(); ++Index)
		{
			if (Index > 0) Result += ".";
			Result += Segments[Index];
		}
		return Result;
	}

	auto DObject::Serialize(FArchive& Ar) -> void
	{
		SerializeDObjectProperties(Ar, this);
	}

	auto DObject::PostLoad(std::string& OutError) -> bool
	{
		(void)OutError;
		return true;
	}

	auto DObject::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		(void)Proposal;
		(void)OutError;
		return true;
	}

	auto DObject::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		(void)Event;
	}

	auto DObject::MarkPackageDirty() -> void
	{
		if (DPackage* Package = GetPackage()) Package->MarkDirty();
	}

	auto DObject::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		ForEachObjectReference(this, Collector);
	}

	auto DObject::BeginDestroy() -> void
	{
	}

	auto DObject::IsReadyForFinishDestroy() -> bool
	{
		return true;
	}

	auto DObject::FinishDestroy() -> void
	{
	}

	auto DObject::IsA(const DClass* InClass) const -> bool
	{
		if (!InClass)
		{
			return false;
		}

		for (const DClass* Class = GetClass(); Class; Class = Class->GetSuperClass())
		{
			if (Class == InClass)
			{
				return true;
			}
		}
		return false;
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
			const char* PackageName = Info.PackageName.c_str();
			const char* Name = Info.Name.c_str();
			Object->DeferredRegister(StaticClass, PackageName, Name);

			// Remove from pending registrants
			PendingRegistrants.erase(It);
		}
	}

	auto RegisterCompiledInInfo(FClassRegisterFunc InOuterRegister, FClassRegisterFunc InInnerRegister, const char* InName, FClassRegistrationInfo& InInfo) -> void
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

	auto RegisterCompiledInInfo(FEnumRegisterFunc InOuterRegister, FEnumRegisterFunc InInnerRegister, const char* InName, FEnumRegistrationInfo& InInfo) -> void
	{
		check(InOuterRegister);
		check(InInnerRegister);
		FEnumDeferredRegistry::Get().AddRegistration(InOuterRegister, InInnerRegister, InName, InInfo);
	}

	auto RegisterCompiledInInfo(const FEnumRegisterCompiledInInfo* EnumInfo, size_t NumEnumInfo) -> void
	{
		for (size_t Index = 0; Index < NumEnumInfo; ++Index)
		{
			const FEnumRegisterCompiledInInfo& Info = EnumInfo[Index];
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

	static auto RegisterAllCompiledInEnums() -> void
	{
		std::vector<FEnumDeferredRegistry::FRegistrant>& Registrations = FEnumDeferredRegistry::Get().GetRegistrations();
		for (FEnumDeferredRegistry::FRegistrant& Registrant : Registrations)
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

	static auto LoadAllCompiledInEnumValues() -> void
	{
		std::vector<FEnumDeferredRegistry::FRegistrant>& Registrations = FEnumDeferredRegistry::Get().GetRegistrations();
		for (FEnumDeferredRegistry::FRegistrant& Registrant : Registrations)
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
		FEnumDeferredRegistry& EnumRegistry = FEnumDeferredRegistry::Get();

		RegisterAllCompiledInClasses();
		RegisterAllCompiledInEnums();
		LoadAllCompiledInDefaultProperties();
		LoadAllCompiledInEnumValues();
		ProcessRegisteredCppPackages();

		ClassRegistry.ClearRegistrations();
		EnumRegistry.ClearRegistrations();
	}
}
