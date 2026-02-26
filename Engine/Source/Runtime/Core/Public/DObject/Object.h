#pragma once

#include "DObject/ObjectMacros.h"
#include "DObject/DObjectGlobals.h"

namespace Doge
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
		const U8Char* Name;
		FClassRegistrationInfo* Info;
	};

	CORE_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;

	class DObject
	{
		DECLARE_CLASS(DObject, DObject, Z_Construct_DClass_DObject_NoRegister)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(DObject)

	public:
		CORE_API DObject();

		CORE_API DObject(const FObjectInitializer& ObjectInitializer);

		CORE_API DObject(DClass* InClass, DObject* InOuter, FName InName);

		// Internal use only for statically-created objects, should not be called directly
		CORE_API DObject(EStaticConstructor, EObjectFlags InFlags);

		virtual ~DObject() = default;

		auto Rename(FName InName) -> void { NamePrivate = InName; }

		auto GetFName() const -> FName { return NamePrivate; }

		auto GetName() const -> FString { return NamePrivate.ToString(); }

		auto GetClass() const -> DClass* { return ClassPrivate; }

		static void IntrinsicClassInit(DClass* Class);

		/**
		 * This is called to register the class with the object system
		 * Add the objec
		 */
		CORE_API auto Register(FClassRegisterFunc InStaticClassFn, const CharT* InPackageName, const CharT* InName) -> void;

		/**
		 * Convert a bootstrap registered class into a fully registered class, adding it to the object array
		 *
		 * InDClassStaticClass is actually DClass::StaticClass()
		 */
		CORE_API auto DeferredRegister(DClass* InDClassStaticClass, const CharT* InPackageName, const CharT* InName) -> void;


	private:
		static auto GetPrivateStaticClass() -> DClass*;

		/**
		 * Add a newly created object to the object array
		 * The name of the object is set here
		 */
		CORE_API auto AddObject(FName InName) -> void;

		FName NamePrivate;

		EObjectFlags ObjectFlags = EObjectFlags::NoFlags;

		DObject* OuterPrivate = nullptr;

		DClass* ClassPrivate = nullptr;

		friend CORE_API auto DObjectForceRegistration(DObject* Object) -> void;

		friend CORE_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;
	};

	/**
	 *  Process all auto-registered DObjects
	 *  Add them to the DObject array in the order they were registered
	 */
	auto DObjectProcessRegistrants() -> void;

	/**
	 *  Force a pending registrant to register now instead of in the natural order
	 */
	CORE_API auto DObjectForceRegistration(DObject* Object) -> void;

	CORE_API auto RegisterCompiledInInfo(FClassRegisterFunc InOuterRegister, FClassRegisterFunc InInnerRegister, const U8Char* InName, FClassRegistrationInfo& InInfo) -> void;

	CORE_API auto RegisterCompiledInInfo(const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo) -> void;

	CORE_API auto ProcessNewlyLoadedDObjects() -> void;

	struct FRegisterCompiledInInfo
	{
		template<typename... ArgTypes>
		FRegisterCompiledInInfo(ArgTypes&&... Args)
		{
			RegisterCompiledInInfo(std::forward<ArgTypes>(Args)...);
		}
	};
}