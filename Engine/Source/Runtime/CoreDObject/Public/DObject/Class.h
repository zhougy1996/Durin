#pragma once

#include "Object.h"

namespace Durin
{
	class FField;
	class FProperty;
	class FObjectInitializer;
	class FReferenceCollector;
	class DStruct;
	COREDOBJECT_API auto ReleaseClassDefaultObjects() -> void;
	COREDOBJECT_API auto ReleaseClassDefaultObjectsForModule(FName ModuleName) -> bool;
	COREDOBJECT_API auto ReleaseDStructDefaults() -> void;
	COREDOBJECT_API auto ReleaseDStructDefaultsForModule(FName ModuleName) -> void;
	namespace Private
	{
		class FGCReferenceSchema;
		class FGCReferenceSchemaRegistry;
		COREDOBJECT_API auto CreateClassDefaultObjectsForBatch(std::span<DClass* const> Classes) -> bool;
		COREDOBJECT_API auto CreateDStructDefaultsForBatch(std::span<DStruct* const> Structs) -> bool;
		auto BeginDStructRegistrationBatch() -> void;
		auto EndDStructRegistrationBatch() -> void;
		auto IsDStructRegistrationBatchActive() -> bool;
		auto ReleaseDStructDefaultOwnership(DStruct* Struct) -> void;
		auto ReleaseClassDefaultObjectOwnership(DClass* Class) -> DObject*;
	}

	// Describes the publication state of a class-owned immutable default object.
	enum class EClassDefaultObjectState : uint8
	{
		Uninitialized,
		Constructing,
		Ready,
		Ineligible,
		Failed,
	};

	// Provides a stable reason when a class has no ready default object.
	enum class EClassDefaultObjectReason : uint8
	{
		None,
		Abstract,
		Intrinsic,
		NoClassDefaultObject,
		MissingConstructor,
		InvalidLayout,
		MissingSuperclassDisposition,
		RecursiveConstruction,
		ConstructionFailed,
	};

	// Describes immutable type-default publication independently from class defaults.
	enum class EDStructDefaultState : uint8
	{
		Uninitialized,
		Constructing,
		Ready,
		Unavailable,
		Failed,
		Released,
	};

	// Stable registration-time reasons why a DStruct cannot publish a type default.
	enum class EDStructDefaultReason : uint8
	{
		None,
		InvalidLayout,
		MissingInitializedOps,
		MissingDefaultConstructor,
		MissingDestructor,
		IncompleteAuthoredFields,
		CustomSerializer,
		UnsupportedPropertyIdentity,
		RecursiveDependency,
		RecursiveConstruction,
		NonDeterministicConstruction,
		PublicationSideEffect,
		ConstructionFailed,
	};

	// Provides the common object identity for all reflected runtime metadata.
	class DType : public DObject
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DType, DObject, COREDOBJECT_API)

		DType(EStaticConstructor, EObjectFlags InFlags)
			: DObject(EC_StaticConstructor, InFlags)
		{
		}
	};

	// Describes reflected fields and memory layout shared by classes and value structs.
	class DStructBase : public DType
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DStructBase, DType, COREDOBJECT_API)

		DStructBase(EStaticConstructor, uint32 InSize, uint32 InMinAlignment, EObjectFlags InFlags)
			: DType(EC_StaticConstructor, InFlags)
			, PropertiesSize(InSize)
			, MinAlignment(InMinAlignment)
		{
		}

		FField* ChildProperties = nullptr;

		uint32 PropertiesSize = 0;

		uint32 MinAlignment = 0;

	private:
		DStructBase* SuperStructBase = nullptr;
		std::shared_ptr<const Private::FGCReferenceSchema> ReferenceSchema;

		friend class Private::FGCReferenceSchemaRegistry;

	public:
		auto SetSuperStructBase(DStructBase* InSuperStructBase) -> void { SuperStructBase = InSuperStructBase; }

		auto GetSuperStructBase() const -> DStructBase* { return SuperStructBase; }

		auto RegisterDependencies() -> void;

		COREDOBJECT_API auto ForEachProperty(const std::function<void(FProperty*)>& Visitor, bool bIncludeSuper = true) const -> void;

		COREDOBJECT_API auto FindPropertyByName(FName InName, bool bIncludeSuper = true) const -> FProperty*;
	};

	// Describes a reflected DObject class, its inheritance, constructor, and presentation names.
	class DClass : public DStructBase
	{
		DECLARE_CLASS_INTRINSIC_API(DClass, DStructBase, COREDOBJECT_API)
	public:
		using StaticClassFunctionType = DClass* (*)();
		using ClassConstructorType = void (*)(const FObjectInitializer&);

		// the Outer is nullptr now , it will be set to a package later maybe
		DClass(
			EStaticConstructor,
			FName InName,
			uint32 InSize,
			uint32 InAlignment,
			EObjectFlags InFlags,
			EClassFlags InClassFlags,
			EClassCastFlags InClassCastFlags,
			ClassConstructorType InClassConstructor
		)
			: DStructBase(EC_StaticConstructor, InSize, InAlignment, InFlags)
			, ClassConstructor(InClassConstructor)
			, ClassFlags(InClassFlags)
			, QualifiedName(InName)
		{
		}

		ClassConstructorType ClassConstructor = nullptr;

		auto GetSuperClass() const -> DClass* { return static_cast<DClass*>(GetSuperStructBase()); }
		COREDOBJECT_API auto IsChildOf(const DClass* InClass) const -> bool;
		auto GetClassFlags() const -> EClassFlags { return ClassFlags; }
		auto HasAnyClassFlags(EClassFlags InFlags) const -> bool { return EnumHasAnyFlags(ClassFlags, InFlags); }
		auto GetQualifiedName() const -> FName { return QualifiedName; }
		COREDOBJECT_API auto SetQualifiedName(FName InQualifiedName) -> void;
		auto GetShortName() const -> const std::string& { return ShortName; }
		auto GetDisplayName() const -> const std::string& { return DisplayName; }
		auto GetDefaultObjectName() const -> const std::string& { return DefaultObjectName; }
		COREDOBJECT_API auto SetTypeNames(std::string_view InShortName, std::string_view InDisplayName, std::string_view InDefaultObjectName) -> void;
		auto GetDefaultObjectState() const -> EClassDefaultObjectState
		{
			return DefaultObjectState.load(std::memory_order_acquire);
		}
		auto GetDefaultObjectReason() const -> EClassDefaultObjectReason
		{
			return DefaultObjectReason.load(std::memory_order_acquire);
		}
		COREDOBJECT_API auto GetDefaultObject() const -> const DObject*;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;

	private:
		auto ResolveDefaultObjectEligibility() -> bool;
		auto BeginDefaultObjectConstruction() -> bool;
		auto SetPendingDefaultObject(DObject* Object) -> void;
		auto PublishDefaultObject() -> void;
		auto FailDefaultObjectConstruction(EClassDefaultObjectReason Reason) -> DObject*;
		auto ReleaseDefaultObjectOwnership() -> DObject*;

		EClassFlags ClassFlags = EClassFlags::None;
		FName QualifiedName;
		std::string ShortName;
		std::string DisplayName;
		std::string DefaultObjectName;
		DObject* ClassDefaultObject = nullptr;
		DObject* PendingDefaultObject = nullptr;
		std::atomic<EClassDefaultObjectReason> DefaultObjectReason{EClassDefaultObjectReason::None};
		std::atomic<EClassDefaultObjectState> DefaultObjectState{EClassDefaultObjectState::Uninitialized};
		mutable std::atomic<bool> bRecursiveDefaultObjectAccess = false;

		friend COREDOBJECT_API auto ProcessNewlyLoadedDObjects() -> void;
		friend COREDOBJECT_API auto ReleaseClassDefaultObjects() -> void;
		friend COREDOBJECT_API auto ReleaseClassDefaultObjectsForModule(FName ModuleName) -> bool;
		friend COREDOBJECT_API auto Private::CreateClassDefaultObjectsForBatch(std::span<DClass* const> Classes) -> bool;
		friend auto Private::ReleaseClassDefaultObjectOwnership(DClass* Class) -> DObject*;
	};

	// Describes a reflected value struct and the operations required to manage its storage.
	class DStruct : public DStructBase
	{
		DECLARE_CLASS_INTRINSIC_API(DStruct, DStructBase, COREDOBJECT_API)
	public:
		DStruct(EStaticConstructor, FName InQualifiedName, FName InShortName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags)
			: DStructBase(EC_StaticConstructor, InSize, InAlignment, InFlags)
			, QualifiedName(InQualifiedName)
			, ShortName(InShortName)
		{
		}
		COREDOBJECT_API ~DStruct() override;

		auto GetQualifiedName() const -> FName { return QualifiedName; }
		auto GetShortName() const -> FName { return ShortName; }
		auto GetOps() const -> const FDStructOps& { return *Ops; }
		auto AreOpsInitialized() const -> bool { return bOpsInitialized; }
		auto CanDefaultConstruct() const -> bool { return HasOpsFlag(EDStructOpsFlags::DefaultConstruct); }
		auto CanDestroy() const -> bool
		{
			return HasOpsFlag(EDStructOpsFlags::TriviallyDestructible)
				|| HasOpsFlag(EDStructOpsFlags::Destroy);
		}
		auto NeedsDestroy() const -> bool { return HasOpsFlag(EDStructOpsFlags::Destroy); }
		auto CanCopyConstruct() const -> bool { return HasOpsFlag(EDStructOpsFlags::CopyConstruct); }
		auto CanCopyAssign() const -> bool { return HasOpsFlag(EDStructOpsFlags::CopyAssign); }
		auto CanZeroConstruct() const -> bool { return HasOpsFlag(EDStructOpsFlags::ZeroConstruct); }
		auto HasIdentical() const -> bool { return HasOpsFlag(EDStructOpsFlags::Identical); }
		auto HasSerializer() const -> bool { return HasOpsFlag(EDStructOpsFlags::Serialize); }
		auto HasPostDeserialize() const -> bool { return HasOpsFlag(EDStructOpsFlags::PostDeserialize); }
		auto HasReferenceCollector() const -> bool { return HasOpsFlag(EDStructOpsFlags::CollectReferences); }
		auto HasCompleteAuthoredFields() const -> bool { return HasOpsFlag(EDStructOpsFlags::AuthoredFieldsComplete); }
		auto GetDefaultState() const -> EDStructDefaultState
		{
			return DefaultState.load(std::memory_order_acquire);
		}
		auto GetDefaultReason() const -> EDStructDefaultReason
		{
			return DefaultReason.load(std::memory_order_acquire);
		}
		COREDOBJECT_API auto GetDefaultValue() const -> const void*;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;

		auto InitializeOps(const FDStructOps* InOps) -> void
		{
			const FDStructOps* ResolvedOps = InOps ? InOps : &GetEmptyDStructOps();
			check(IsValidDStructOps(*ResolvedOps));
			if (bOpsInitialized)
			{
				check(Ops == ResolvedOps && "A DStruct operation table is immutable after registration.");
				return;
			}
			Ops = ResolvedOps;
			bOpsInitialized = true;
		}

	private:
		auto HasOpsFlag(EDStructOpsFlags Flag) const -> bool
		{
			return EnumHasAnyFlags(Ops->Flags, Flag);
		}

		FName QualifiedName;
		FName ShortName;
		const FDStructOps* Ops = &GetEmptyDStructOps();
		bool bOpsInitialized = false;
		void* DefaultValue = nullptr;
		void* PendingDefaultValue = nullptr;
		std::atomic<EDStructDefaultReason> DefaultReason{EDStructDefaultReason::None};
		std::atomic<EDStructDefaultState> DefaultState{EDStructDefaultState::Uninitialized};
		mutable std::atomic<bool> bRecursiveDefaultAccess = false;

		auto ResolveDefaultEligibility() -> bool;
		auto BeginDefaultConstruction() -> bool;
		auto SetPendingDefaultValue(void* Value) -> void;
		auto PublishDefaultValue() -> void;
		auto FailDefaultConstruction(EDStructDefaultReason Reason) -> void*;
		auto ReleaseDefaultValue() -> void*;
		auto DestroyDefaultStorage(void* Value) const -> void;

		friend COREDOBJECT_API auto Private::CreateDStructDefaultsForBatch(std::span<DStruct* const> Structs) -> bool;
		friend auto Private::ReleaseDStructDefaultOwnership(DStruct* Struct) -> void;
		friend COREDOBJECT_API auto ReleaseDStructDefaults() -> void;
		friend COREDOBJECT_API auto ReleaseDStructDefaultsForModule(FName ModuleName) -> void;
	};

	// Stores one reflected enum value with its stable code name and editor label.
	struct FEnumValue
	{
		FName Name;
		uint64 Value = 0;
		std::string DisplayName;
	};

	// Describes a reflected enum's representation and ordered value records.
	class DEnum : public DType
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DEnum, DType, COREDOBJECT_API)

		COREDOBJECT_API DEnum(
			EStaticConstructor,
			FName InName,
			FName InQualifiedName,
			FName InShortName,
			std::string_view InDisplayName,
			bool bInIsScoped,
			DurinCodeGen::EEnumUnderlyingType InUnderlyingType,
			uint16 InUnderlyingSize,
			std::vector<FEnumValue> InValues,
			EObjectFlags InFlags
		);

		auto GetQualifiedName() const -> FName { return QualifiedName; }
		auto GetShortName() const -> FName { return ShortName; }
		auto GetDisplayName() const -> std::string_view { return DisplayName; }
		auto IsScoped() const -> bool { return bIsScoped; }
		auto GetUnderlyingType() const -> DurinCodeGen::EEnumUnderlyingType { return UnderlyingType; }
		auto GetUnderlyingSize() const -> uint16 { return UnderlyingSize; }
		auto GetValues() const -> const std::vector<FEnumValue>& { return Values; }

		COREDOBJECT_API auto FindValueRecordByName(FName InName) const -> const FEnumValue*;
		COREDOBJECT_API auto FindValueRecordByValue(uint64 InValue) const -> const FEnumValue*;
		COREDOBJECT_API auto FindValueByName(FName InName, uint64& OutValue) const -> bool;
		COREDOBJECT_API auto FindNameByValue(uint64 InValue, FName& OutName) const -> bool;
		COREDOBJECT_API auto ForEachValue(const std::function<void(const FEnumValue&)>& Visitor) const -> void;

	private:
		FName QualifiedName;
		FName ShortName;
		std::string DisplayName;
		bool bIsScoped = false;
		DurinCodeGen::EEnumUnderlyingType UnderlyingType = DurinCodeGen::EEnumUnderlyingType::Unknown;
		uint16 UnderlyingSize = 0;
		std::vector<FEnumValue> Values;
	};

	template<class T>
	void InternalConstructor(const FObjectInitializer& X)
	{
		T::__DefaultConstructor(X);
	}

	COREDOBJECT_API auto GetPrivateStaticClassBody(
		const char* PackageName,
		const char* Name,
		DClass*& ReturnClass,
		void (*RegisterNativeFunc)(),
		uint32 InSize,
		uint32 InAlignment,
		EClassFlags InClassFlags,
		DClass::ClassConstructorType InClassConstructor,
		DClass::StaticClassFunctionType InSuperClassFn
	) -> DClass*;

	COREDOBJECT_API auto FindClassByQualifiedName(FName QualifiedName) -> DClass*;
	// Resolves a serialized class identity through current names and read-only legacy aliases.
	COREDOBJECT_API auto FindClassBySerializedName(FName SerializedName) -> DClass*;
	COREDOBJECT_API auto GetDerivedClasses(const DClass* BaseClass, bool bIncludeBase = false) -> std::vector<DClass*>;
	COREDOBJECT_API auto FindStructByQualifiedName(FName QualifiedName) -> DStruct*;
	// Resolves a serialized struct identity through current names and read-only legacy aliases.
	COREDOBJECT_API auto FindStructBySerializedName(FName SerializedName) -> DStruct*;
	COREDOBJECT_API auto FindEnumByQualifiedName(FName QualifiedName) -> DEnum*;
	// Resolves a serialized enum identity through current names and read-only legacy aliases.
	COREDOBJECT_API auto FindEnumBySerializedName(FName SerializedName) -> DEnum*;
	COREDOBJECT_API auto FindClassByPath(std::string_view ObjectPath) -> DClass*;
	COREDOBJECT_API auto FindStructByPath(std::string_view ObjectPath) -> DStruct*;
	COREDOBJECT_API auto FindEnumByPath(std::string_view ObjectPath) -> DEnum*;
}
