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
#include "Logging/LogMacros.h"

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DObject();

namespace Durin
{
	namespace
	{
		thread_local uint32 PackageDirtySuppressionDepth = 0;
	}

	FScopedPackageDirtySuppression::FScopedPackageDirtySuppression()
	{
		++PackageDirtySuppressionDepth;
	}

	FScopedPackageDirtySuppression::~FScopedPackageDirtySuppression()
	{
		require(PackageDirtySuppressionDepth > 0);
		--PackageDirtySuppressionDepth;
	}

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
				EClassFlags::Intrinsic,
				(DClass::ClassConstructorType)InternalConstructor<DObject>,
				nullptr
			);
		}
		return Z_Registration_Info_DClass_DObject.InnerSingleton;
	}

	static FClassRegisterCompiledInInfo Z_AutoRegister_DObject(
		&::Z_Construct_DClass_Durin_DObject,
		&DObject::StaticClass,
		STR("DObject"),
		&Z_Registration_Info_DClass_DObject
	);

	DObject::DObject()
	{
	}

	DObject::DObject(const FObjectInitializer& ObjectInitializer)
		: NamePrivate(ObjectInitializer.Name)
		, ObjectFlags(ObjectInitializer.Purpose == EObjectConstructionPurpose::ClassDefaultObject
			? EObjectFlags::ClassDefaultObject | EObjectFlags::Transient
			: ObjectInitializer.Purpose == EObjectConstructionPurpose::ClassDefaultSubobject
				? EObjectFlags::DefaultSubobject | EObjectFlags::Transient
				: ObjectInitializer.Purpose == EObjectConstructionPurpose::Generated
					? EObjectFlags::Transient
				: EObjectFlags::NoFlags)
		, ConstructionPurpose(ObjectInitializer.Purpose)
		, OuterPrivate(ObjectInitializer.Outer)
		, ClassPrivate(ObjectInitializer.Class)
	{
	}

	DObject::DObject(DClass* InClass, DObject* InOuter, FName InName)
		: NamePrivate(InName)
		, OuterPrivate(InOuter)
		, ClassPrivate(InClass)
	{
	}
	DObject::DObject(EStaticConstructor, EObjectFlags InFlags)
		: ObjectFlags(InFlags)
	{
	}

	auto DObject::Rename(FName InName) -> void
	{
		if (IsTemplateObject()) return;
		NamePrivate = InName;
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
		if (IsTemplateObject() && GDObjectArray.Contains(this) && !IsGarbage()) return;

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
		SerializeDObjectProperties(Ar, *this);
	}

	auto DObject::PostLoad(std::string& OutError) -> bool
	{
		(void)OutError;
		return true;
	}

	auto DObject::GetLoadedCustomVersion(const FGuid& Key) const -> std::optional<int32>
	{
		const auto It = std::ranges::find(LoadedCustomVersions, Key,
			[](const auto& Entry) { return Entry.first; });
		return It == LoadedCustomVersions.end() ? std::nullopt : std::optional<int32>(It->second);
	}

	auto DObject::SetLoadedCustomVersions(std::span<const std::pair<FGuid, int32>> Versions) -> void
	{
		LoadedCustomVersions.assign(Versions.begin(), Versions.end());
	}

	auto DObject::ClearLoadedCustomVersions() -> void
	{
		LoadedCustomVersions.clear();
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
		if (IsTemplateObject() || PackageDirtySuppressionDepth > 0) return;
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

	auto Private::CreateClassDefaultObjectsForBatch(std::span<DClass* const> Classes) -> bool
	{
		std::vector<DClass*> BatchClasses;
		std::unordered_set<DClass*> AddedClasses;
		for (DClass* Class : Classes)
		{
			for (DClass* CurrentClass = Class; CurrentClass != nullptr; CurrentClass = CurrentClass->GetSuperClass())
			{
				if (AddedClasses.insert(CurrentClass).second)
				{
					BatchClasses.push_back(CurrentClass);
				}
			}
		}
		auto ClassDepth = [](const DClass* Class) {
			uint32 Depth = 0;
			for (; Class; Class = Class->GetSuperClass()) ++Depth;
			return Depth;
		};
		std::ranges::sort(BatchClasses, [&](const DClass* Left, const DClass* Right) {
			const uint32 LeftDepth = ClassDepth(Left);
			const uint32 RightDepth = ClassDepth(Right);
			if (LeftDepth != RightDepth) return LeftDepth < RightDepth;
			return Left->GetQualifiedName().ToString() < Right->GetQualifiedName().ToString();
		});

		std::vector<DClass*> EligibleClasses;
		for (DClass* Class : BatchClasses)
		{
			if (Class->ResolveDefaultObjectEligibility()) EligibleClasses.push_back(Class);
		}

		std::vector<DClass*> ConstructedClasses;
		auto FailBatch = [&](DClass* FailedClass, EClassDefaultObjectReason Reason) {
			DURIN_ERROR(STR("Class-default batch failed at '{}' with reason {}."),
				FailedClass ? FailedClass->GetQualifiedName().ToString() : std::string("<null>"),
				static_cast<uint32>(Reason));
			for (auto It = ConstructedClasses.rbegin(); It != ConstructedClasses.rend(); ++It)
			{
				DClass* Class = *It;
				DObject* Object = Class->FailDefaultObjectConstruction(
					Class == FailedClass ? Reason : EClassDefaultObjectReason::ConstructionFailed);
				Private::MarkTemplateObjectHierarchyAsGarbage(Object);
			}
			for (DClass* Class : EligibleClasses)
			{
				if (Class->GetDefaultObjectState() == EClassDefaultObjectState::Uninitialized)
				{
					Class->FailDefaultObjectConstruction(
						Class == FailedClass ? Reason : EClassDefaultObjectReason::ConstructionFailed);
				}
			}
			CollectGarbage();
		};

		for (DClass* Class : EligibleClasses)
		{
			if (const DClass* SuperClass = Class->GetSuperClass())
			{
				const EClassDefaultObjectState SuperState = SuperClass->GetDefaultObjectState();
				const bool bConstructedInThisBatch = std::ranges::find(ConstructedClasses, SuperClass)
					!= ConstructedClasses.end();
				if (SuperState == EClassDefaultObjectState::Uninitialized
					|| (SuperState == EClassDefaultObjectState::Constructing && !bConstructedInThisBatch)
					|| SuperState == EClassDefaultObjectState::Failed)
				{
					FailBatch(Class, EClassDefaultObjectReason::MissingSuperclassDisposition);
					return false;
				}
			}
			if (!Class->BeginDefaultObjectConstruction())
			{
				FailBatch(Class, EClassDefaultObjectReason::ConstructionFailed);
				return false;
			}

			FStaticConstructObjectParameters Params;
			Params.Class = Class;
			Params.Outer = Class;
			const std::string& ShortName = Class->GetShortName();
			Params.Name = FName(std::format(
				"Default__{}", ShortName.empty() ? Class->GetQualifiedName().ToString() : ShortName));
			Params.Size = Class->PropertiesSize;
			Params.Purpose = EObjectConstructionPurpose::ClassDefaultObject;
			DObject* Object = StaticConstructObject(Params);
			Class->SetPendingDefaultObject(Object);
			ConstructedClasses.push_back(Class);
			const auto RecursiveIt = std::ranges::find_if(ConstructedClasses, [](const DClass* ConstructedClass) {
				return ConstructedClass->bRecursiveDefaultObjectAccess.load(std::memory_order_relaxed);
			});
			if (RecursiveIt != ConstructedClasses.end())
			{
				FailBatch(*RecursiveIt, EClassDefaultObjectReason::RecursiveConstruction);
				return false;
			}
		}

		for (DClass* Class : ConstructedClasses) Class->PublishDefaultObject();
		return true;
	}

	auto ProcessNewlyLoadedDObjects() -> void
	{
		FClassDeferredRegistry& ClassRegistry = FClassDeferredRegistry::Get();
		FEnumDeferredRegistry& EnumRegistry = FEnumDeferredRegistry::Get();

		Private::BeginDStructRegistrationBatch();
		RegisterAllCompiledInClasses();
		RegisterAllCompiledInEnums();
		LoadAllCompiledInDefaultProperties();
		LoadAllCompiledInEnumValues();
		ProcessRegisteredCppPackages();
		std::vector<DStruct*> BatchStructs;
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
		{
			if (auto* Struct = Cast<DStruct>(Object)) BatchStructs.push_back(Struct);
		}
		(void)Private::CreateDStructDefaultsForBatch(BatchStructs);
		Private::EndDStructRegistrationBatch();

		std::vector<DClass*> BatchClasses;
		const std::array IntrinsicClasses{
			DObject::StaticClass(),
			DType::StaticClass(),
			DStructBase::StaticClass(),
			DClass::StaticClass(),
			DStruct::StaticClass(),
			DEnum::StaticClass()};
		BatchClasses.insert(BatchClasses.end(), IntrinsicClasses.begin(), IntrinsicClasses.end());
		for (const FClassDeferredRegistry::FRegistrant& Registrant : ClassRegistry.GetRegistrations())
		{
			if (Registrant.Info->OuterSingleton) BatchClasses.push_back(Registrant.Info->OuterSingleton);
		}
		(void)Private::CreateClassDefaultObjectsForBatch(BatchClasses);

		ClassRegistry.ClearRegistrations();
		EnumRegistry.ClearRegistrations();
	}
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DObject()
{
	if (!Durin::Z_Registration_Info_DClass_DObject.OuterSingleton)
	{
		Durin::Z_Registration_Info_DClass_DObject.OuterSingleton = Durin::Z_Construct_DClass_DObject_Statics::Construct();
	}
	check(Durin::Z_Registration_Info_DClass_DObject.OuterSingleton->GetClass());
	return Durin::Z_Registration_Info_DClass_DObject.OuterSingleton;
}
