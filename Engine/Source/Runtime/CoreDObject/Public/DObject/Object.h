#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/ObjectMacros.h"
#include "DObjectGlobals.h"

namespace Durin
{
	class FObjectInitializer;

	using FClassRegisterFunc = DClass* (*)();

	template<typename T>
	struct FRegistrationInfo
	{
		using TType = T;

		TType* InnerSingleton = nullptr;
		TType* OuterSingleton = nullptr;
	};

	using FClassRegistrationInfo = FRegistrationInfo<DClass>;

	struct FClassRegisterCompiledInInfo
	{
		DClass* (*OuterRegister)();
		DClass* (*InnerRegister)();
		const char* Name;
		FClassRegistrationInfo* Info;
	};

	COREDOBJECT_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;

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

		auto Rename(FName InName) -> void { NamePrivate = InName; }

		auto GetFName() const -> FName { return NamePrivate; }

		auto GetName() const -> std::string { return NamePrivate.ToString(); }

		auto GetClass() const -> DClass* { return ClassPrivate; }

		COREDOBJECT_API auto IsA(const DClass* InClass) const -> bool;

		template<typename T>
		auto IsA() const -> bool
		{
			return IsA(T::StaticClass());
		}

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
		static auto GetPrivateStaticClass() -> DClass*;

		/**
		 * Add a newly created object to the object array
		 * The name of the object is set here
		 */
		COREDOBJECT_API auto AddObject(FName InName) -> void;

		FName NamePrivate;

		EObjectFlags ObjectFlags = EObjectFlags::NoFlags;

		DObject* OuterPrivate = nullptr;

		DClass* ClassPrivate = nullptr;

		friend COREDOBJECT_API auto DObjectForceRegistration(DObject* Object) -> void;

		friend COREDOBJECT_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;
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
