#pragma once

#include "Object.h"

namespace Durin
{
	class FField;
	class FProperty;
	class FObjectInitializer;

	// Base class for all reflected runtime types
	class DType : public DObject
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DType, DObject, COREDOBJECT_API)

		DType(EStaticConstructor, EObjectFlags InFlags)
			: DObject(EC_StaticConstructor, InFlags)
		{
		}
	};

	// Base class for all reflected types that contain fields and memory layout
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

	public:
		auto SetSuperStructBase(DStructBase* InSuperStructBase) -> void { SuperStructBase = InSuperStructBase; }

		auto GetSuperStructBase() const -> DStructBase* { return SuperStructBase; }

		auto RegisterDependencies() -> void;

		COREDOBJECT_API auto ForEachProperty(const std::function<void(FProperty*)>& Visitor, bool bIncludeSuper = true) const -> void;

		COREDOBJECT_API auto FindPropertyByName(FName InName, bool bIncludeSuper = true) const -> FProperty*;
	};

	// Describe a class
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
		auto IsChildOf(const DClass* InClass) const -> bool;
		auto GetClassFlags() const -> EClassFlags { return ClassFlags; }
		auto HasAnyClassFlags(EClassFlags InFlags) const -> bool { return EnumHasAnyFlags(ClassFlags, InFlags); }
		auto GetQualifiedName() const -> FName { return QualifiedName; }
		COREDOBJECT_API auto SetQualifiedName(FName InQualifiedName) -> void;
		auto GetShortName() const -> const std::string& { return ShortName; }
		auto GetDisplayName() const -> const std::string& { return DisplayName; }
		auto GetDefaultObjectName() const -> const std::string& { return DefaultObjectName; }
		COREDOBJECT_API auto SetTypeNames(std::string_view InShortName, std::string_view InDisplayName, std::string_view InDefaultObjectName) -> void;

	private:
		EClassFlags ClassFlags = EClassFlags::None;
		FName QualifiedName;
		std::string ShortName;
		std::string DisplayName;
		std::string DefaultObjectName;
	};

	class DStruct : public DStructBase
	{
		DECLARE_CLASS_INTRINSIC_API(DStruct, DStructBase, COREDOBJECT_API)
	public:
		using InitializeFunction = void (*)(void* Memory);
		using DestroyFunction = void (*)(void* Memory);
		using CopyFunction = void (*)(void* Destination, const void* Source);

		DStruct(EStaticConstructor, FName InQualifiedName, FName InShortName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags)
			: DStructBase(EC_StaticConstructor, InSize, InAlignment, InFlags)
			, QualifiedName(InQualifiedName)
			, ShortName(InShortName)
		{
		}

		auto GetQualifiedName() const -> FName { return QualifiedName; }
		auto GetShortName() const -> FName { return ShortName; }
		auto SetCppOps(InitializeFunction InInitialize, DestroyFunction InDestroy, CopyFunction InCopy) -> void
		{
			Initialize = InInitialize;
			Destroy = InDestroy;
			Copy = InCopy;
		}
		auto InitializeValue(void* Memory) const -> void { if (Initialize) Initialize(Memory); }
		auto DestroyValue(void* Memory) const -> void { if (Destroy) Destroy(Memory); }
		auto CopyValue(void* Destination, const void* Source) const -> void { if (Copy) Copy(Destination, Source); }

	private:
		FName QualifiedName;
		FName ShortName;
		InitializeFunction Initialize = nullptr;
		DestroyFunction Destroy = nullptr;
		CopyFunction Copy = nullptr;
	};

	struct FEnumValue
	{
		FName Name;
		int64 Value = 0;
	};

	class DEnum : public DType
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DEnum, DType, COREDOBJECT_API)

		DEnum(
			EStaticConstructor,
			FName InName,
			FName InQualifiedName,
			FName InShortName,
			bool bInIsScoped,
			DurinCodeGen::EEnumUnderlyingType InUnderlyingType,
			uint16 InUnderlyingSize,
			std::vector<FEnumValue> InValues,
			EObjectFlags InFlags
		)
			: DType(EC_StaticConstructor, InFlags)
			, QualifiedName(InQualifiedName)
			, ShortName(InShortName)
			, bIsScoped(bInIsScoped)
			, UnderlyingType(InUnderlyingType)
			, UnderlyingSize(InUnderlyingSize)
			, Values(std::move(InValues))
		{
			(void)InName;
		}

		auto GetQualifiedName() const -> FName { return QualifiedName; }
		auto GetShortName() const -> FName { return ShortName; }
		auto IsScoped() const -> bool { return bIsScoped; }
		auto GetUnderlyingType() const -> DurinCodeGen::EEnumUnderlyingType { return UnderlyingType; }
		auto GetUnderlyingSize() const -> uint16 { return UnderlyingSize; }
		auto GetValues() const -> const std::vector<FEnumValue>& { return Values; }

		COREDOBJECT_API auto FindValueByName(FName InName, int64& OutValue) const -> bool;
		COREDOBJECT_API auto FindNameByValue(int64 InValue, FName& OutName) const -> bool;
		COREDOBJECT_API auto ForEachValue(const std::function<void(const FEnumValue&)>& Visitor) const -> void;

	private:
		FName QualifiedName;
		FName ShortName;
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
	COREDOBJECT_API auto GetDerivedClasses(const DClass* BaseClass, bool bIncludeBase = false) -> std::vector<DClass*>;
	COREDOBJECT_API auto FindStructByQualifiedName(FName QualifiedName) -> DStruct*;
	COREDOBJECT_API auto FindEnumByQualifiedName(FName QualifiedName) -> DEnum*;
	COREDOBJECT_API auto FindClassByPath(std::string_view ObjectPath) -> DClass*;
	COREDOBJECT_API auto FindStructByPath(std::string_view ObjectPath) -> DStruct*;
	COREDOBJECT_API auto FindEnumByPath(std::string_view ObjectPath) -> DEnum*;
}
