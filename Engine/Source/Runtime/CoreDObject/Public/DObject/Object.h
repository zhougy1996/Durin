#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/ObjectMacros.h"
#include "DObject/AuthoredOverrideLedger.h"
#include "DObject/PropertyChange.h"
#include "DObjectGlobals.h"
#include "Misc/Guid.h"

namespace Durin
{
	class FObjectInitializer;
	class FArchive;
	class FReferenceCollector;
	class DPackage;

	using FClassRegisterFunc = DClass* (*)();
	using FEnumRegisterFunc = DEnum* (*)();

	// Suppresses package dirty/revision mutation for observational derived-state reconciliation.
	class FScopedPackageDirtySuppression final
	{
	public:
		COREDOBJECT_API FScopedPackageDirtySuppression();
		COREDOBJECT_API ~FScopedPackageDirtySuppression();
		FScopedPackageDirtySuppression(const FScopedPackageDirtySuppression&) = delete;
		auto operator=(const FScopedPackageDirtySuppression&)
			-> FScopedPackageDirtySuppression& = delete;
	};

	template<typename T>
	struct FRegistrationInfo
	{
		using TType = T;

		TType* InnerSingleton = nullptr;
		TType* OuterSingleton = nullptr;
	};

	using FClassRegistrationInfo = FRegistrationInfo<DClass>;
	using FEnumRegistrationInfo = FRegistrationInfo<DEnum>;

	struct FClassRegisterCompiledInInfo
	{
		DClass* (*OuterRegister)();
		DClass* (*InnerRegister)();
		const char* Name;
		FClassRegistrationInfo* Info;
	};

	struct FEnumRegisterCompiledInInfo
	{
		DEnum* (*OuterRegister)();
		DEnum* (*InnerRegister)();
		const char* Name;
		FEnumRegistrationInfo* Info;
	};

	COREDOBJECT_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;
	COREDOBJECT_API auto Z_Construct_DClass_DObject() -> DClass*;

	// Provides reflected identity, Outer ownership, serialization, and destruction hooks for managed objects.
	class DObject
	{
		DECLARE_CLASS(DObject, DObject, Z_Construct_DClass_DObject_NoRegister)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(DObject)

	public:
		COREDOBJECT_API DObject();

		COREDOBJECT_API DObject(const FObjectInitializer& ObjectInitializer);

		COREDOBJECT_API DObject(DClass* InClass, DObject* InOuter, FName InName);

		// Internal use only for statically-created objects, should not be called directly
		COREDOBJECT_API DObject(EStaticConstructor, EObjectFlags InFlags);

		virtual ~DObject() = default;

		COREDOBJECT_API auto Rename(FName InName) -> void;

		auto GetFName() const -> FName { return NamePrivate; }

		auto GetName() const -> std::string { return NamePrivate.ToString(); }

		auto GetClass() const -> DClass* { return ClassPrivate; }

		auto GetOuter() const -> DObject* { return OuterPrivate; }
		COREDOBJECT_API auto GetOutermost() const -> DObject*;
		COREDOBJECT_API auto GetPackage() const -> DPackage*;
		COREDOBJECT_API auto GetObjectPath() const -> std::string;
		COREDOBJECT_API auto MarkPackageDirty() -> void;

		auto GetObjectFlags() const -> EObjectFlags { return ObjectFlags; }
		auto HasAnyObjectFlags(EObjectFlags InFlags) const -> bool { return EnumHasAnyFlags(ObjectFlags, InFlags); }
		auto IsClassDefaultObject() const -> bool { return HasAnyObjectFlags(EObjectFlags::ClassDefaultObject); }
		auto IsTemplateObject() const -> bool
		{
			return HasAnyObjectFlags(EObjectFlags::ClassDefaultObject | EObjectFlags::DefaultSubobject);
		}
		auto GetConstructionPurpose() const -> EObjectConstructionPurpose { return ConstructionPurpose; }

		auto GetInternalFlags() const -> EObjectInternalFlags { return InternalFlags; }

		auto HasAnyInternalFlags(EObjectInternalFlags InFlags) const -> bool { return EnumHasAnyFlags(InternalFlags, InFlags); }
		auto IsGarbage() const -> bool { return HasAnyInternalFlags(EObjectInternalFlags::Garbage); }
		auto IsPendingKill() const -> bool { return HasAnyInternalFlags(EObjectInternalFlags::Garbage | EObjectInternalFlags::BeginDestroyed); }

		COREDOBJECT_API auto IsA(const DClass* InClass) const -> bool;

		template<typename T>
		auto IsA() const -> bool
		{
			return IsA(T::StaticClass());
		}

		// Loading Archives may update object state. Saving, discovery, and duplication
		// Archives must not modify persistent object semantics; transient diagnostics or
		// caches may change only when they cannot affect emitted bytes or later passes.
		COREDOBJECT_API virtual auto Serialize(FArchive& Ar) -> void;

		// Projects target-specific persistent state when a cooked Archive explicitly
		// selects it. The default preserves the ordinary serialization contract.
		COREDOBJECT_API virtual auto SerializeCooked(FArchive& Ar) -> void;

		COREDOBJECT_API virtual auto AddReferencedObjects(FReferenceCollector& Collector) -> void;

		COREDOBJECT_API virtual auto BeginDestroy() -> void;

		COREDOBJECT_API virtual auto IsReadyForFinishDestroy() -> bool;

		COREDOBJECT_API virtual auto FinishDestroy() -> void;

		COREDOBJECT_API virtual auto PostLoad(std::string& OutError) -> bool;

		// Exposes source-package versions only while authored PostLoad migration runs.
		COREDOBJECT_API auto GetLoadedCustomVersion(const FGuid& Key) const -> std::optional<int32>;
		COREDOBJECT_API auto SetLoadedCustomVersions(std::span<const std::pair<FGuid, int32>> Versions) -> void;
		COREDOBJECT_API auto ClearLoadedCustomVersions() -> void;

		// Validates or normalizes detached reflected storage before a live write.
		COREDOBJECT_API virtual auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool;

		// Editor mutation state stays outside DObject; this synchronous hook only
		// lets the object refresh state derived from a successfully changed value.
		COREDOBJECT_API virtual auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void;

		COREDOBJECT_API auto SetAuthoredOverride(
			const FAuthoredOverridePath& Path,
			EAuthoredOverrideProvenance Provenance,
			FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
		COREDOBJECT_API auto ReplaceAuthoredOverrides(
			std::span<const FAuthoredOverrideEntry> Entries,
			FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
		COREDOBJECT_API auto ClearAuthoredOverride(const FAuthoredOverridePath& Path) -> bool;
		COREDOBJECT_API auto ClearAuthoredOverrideSubtree(const FAuthoredOverridePath& Path) -> uint64;
		COREDOBJECT_API auto ResetAuthoredOverrides() -> void;
		COREDOBJECT_API auto GetAuthoredOverrideEntries() const -> std::vector<FAuthoredOverrideEntry>;
		COREDOBJECT_API auto HasAllocatedAuthoredOverrideLedger() const -> bool;
		COREDOBJECT_API auto CopyAuthoredOverridesFrom(
			const DObject& Source,
			FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;

		static void IntrinsicClassInit(DClass* Class);

		/**
		 * This is called to register the class with the object system
		 * Add the objec
		 */
		COREDOBJECT_API auto Register(FClassRegisterFunc InStaticClassFn, const char* InPackageName, const char* InName) -> void;

		/**
		 * Convert a bootstrap registered class into a fully registered class, adding it to the object array
		 *
		 * InDClassStaticClass is actually DClass::StaticClass()
		 */
		COREDOBJECT_API auto DeferredRegister(DClass* InDClassStaticClass, const char* InPackageName, const char* InName) -> void;


	private:
		COREDOBJECT_API static auto GetPrivateStaticClass() -> DClass*;

		/**
		 * Add a newly created object to the object array
		 * The name of the object is set here
		 */
		COREDOBJECT_API auto AddObject(FName InName) -> void;

		FName NamePrivate;

		EObjectFlags ObjectFlags = EObjectFlags::NoFlags;

		EObjectConstructionPurpose ConstructionPurpose = EObjectConstructionPurpose::RuntimeObject;

		DObject* OuterPrivate = nullptr;

		DClass* ClassPrivate = nullptr;

		EObjectInternalFlags InternalFlags = EObjectInternalFlags::None;

		uint32 RootReferenceCount = 0;

		std::shared_ptr<const FAuthoredOverrideLedger> AuthoredOverrideLedger;
		std::vector<std::pair<FGuid, int32>> LoadedCustomVersions;

	public:
		auto SetInternalFlags(EObjectInternalFlags InFlags) -> void { InternalFlags |= InFlags; }
		auto ClearInternalFlags(EObjectInternalFlags InFlags) -> void { InternalFlags &= ~InFlags; }
		COREDOBJECT_API auto SetOuterPrivate(DObject* NewOuter) -> void;

	private:
		friend COREDOBJECT_API auto DObjectForceRegistration(DObject* Object) -> void;

		friend COREDOBJECT_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;
		friend COREDOBJECT_API auto AddToRoot(DObject* Object) -> void;
		friend COREDOBJECT_API auto RemoveFromRoot(DObject* Object) -> void;
		friend COREDOBJECT_API auto MarkAsGarbage(DObject* Object) -> void;
		friend COREDOBJECT_API auto CollectGarbage() -> void;
		friend COREDOBJECT_API auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void;
		friend COREDOBJECT_API auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*;
		friend class DPackage;
		friend class FDObjectArray;
	};

	template<typename T>
	auto Cast(DObject* Object) -> T*
	{
		static_assert(std::is_base_of_v<DObject, T>, "T must be derived from DObject");
		return Object && Object->IsA(T::StaticClass()) ? static_cast<T*>(Object) : nullptr;
	}

	template<typename T>
	auto Cast(const DObject* Object) -> const T*
	{
		static_assert(std::is_base_of_v<DObject, T>, "T must be derived from DObject");
		return Object && Object->IsA(T::StaticClass()) ? static_cast<const T*>(Object) : nullptr;
	}

	/**
	 *  Process all auto-registered DObjects
	 *  Add them to the DObject array in the order they were registered
	 */
	auto DObjectProcessRegistrants() -> void;

	/**
	 *  Force a pending registrant to register now instead of in the natural order
	 */
	COREDOBJECT_API auto DObjectForceRegistration(DObject* Object) -> void;

	COREDOBJECT_API auto RegisterCompiledInInfo(FClassRegisterFunc InOuterRegister, FClassRegisterFunc InInnerRegister, const char* InName, FClassRegistrationInfo& InInfo) -> void;

	COREDOBJECT_API auto RegisterCompiledInInfo(const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo) -> void;

	COREDOBJECT_API auto RegisterCompiledInInfo(FEnumRegisterFunc InOuterRegister, FEnumRegisterFunc InInnerRegister, const char* InName, FEnumRegistrationInfo& InInfo) -> void;

	COREDOBJECT_API auto RegisterCompiledInInfo(const FEnumRegisterCompiledInInfo* EnumInfo, size_t NumEnumInfo) -> void;

	COREDOBJECT_API auto ProcessNewlyLoadedDObjects() -> void;

	struct FRegisterCompiledInInfo
	{
		template<typename... ArgTypes>
		FRegisterCompiledInInfo(ArgTypes&&... Args)
		{
			RegisterCompiledInInfo(std::forward<ArgTypes>(Args)...);
		}
	};
}
