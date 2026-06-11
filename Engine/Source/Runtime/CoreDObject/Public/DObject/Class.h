#pragma once

#include "Object.h"

namespace Durin
{
	class FField;
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
		{
		}

		ClassConstructorType ClassConstructor = nullptr;

		auto GetSuperClass() const -> DClass* { return static_cast<DClass*>(GetSuperStructBase()); }
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
}
