#pragma once

#include "Object.h"

namespace Durin
{
	class FField;
	class FObjectInitializer;

	// Base class for all DObject types that contain properties
	class DStructure : public DObject
	{
	public:
		DECLARE_CLASS_INTRINSIC(DStructure, DObject)

		DStructure(EStaticConstructor, uint32 InSize, uint32 InMinAlignment, EObjectFlags InFlags)
			: DObject(EC_StaticConstructor, InFlags)
			, PropertiesSize(InSize)
			, MinAlignment(InMinAlignment)
		{
		}

		FField* ChildProperties = nullptr;

		uint32 PropertiesSize = 0;

		uint32 MinAlignment = 0;

	private:
		DStructure* SuperStructure = nullptr;

	public:
		auto SetSuperStructure(DStructure* InSuperStructure) -> void { SuperStructure = InSuperStructure; }

		auto RegisterDependencies() -> void;
	};

	// Describe a class
	class DClass : public DStructure
	{
		DECLARE_CLASS_INTRINSIC(DClass, DStructure)
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
			: DStructure(EC_StaticConstructor, InSize, InAlignment, InFlags)
			, ClassConstructor(InClassConstructor)
		{
		}

		ClassConstructorType ClassConstructor = nullptr;
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