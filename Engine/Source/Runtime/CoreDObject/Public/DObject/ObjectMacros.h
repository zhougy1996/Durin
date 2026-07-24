#pragma once

#define DHT_DEBUG_BEGIN()
#define DHT_DEBUG_END()

#define DCLASS(...)
#define DSTRUCT(...)
#define DENUM(...)
#define DMETA(...)
#define DPROPERTY(...)
#define DFUNCTION(...)

#define BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
#define BODY_MACRO_COMBINE(A, B, C, D) BODY_MACRO_COMBINE_INNER(A, B, C, D)

#define NO_API

namespace Durin
{
	// clang-format off
	enum EStaticConstructor { EC_StaticConstructor };
	enum EInternal { EC_InternalUseOnlyConstructor };
	// clang-format on

	enum class EObjectFlags
	{
		NoFlags = 0,
		Intrinsic = 1 << 0,
		Transient = 1 << 1,
	};

	enum class EClassFlags
	{
		None = 0,

		// Class is abstract and can't be instantiated directly
		Abstract = 1,

		// Class was declared in C++ and has no generated code
		Native = 1 << 1,
	};
	ENUM_CLASS_FLAGS(EClassFlags);

	enum class EClassCastFlags : uint64
	{
		None = 0,

		DObject = 1 << 0,
		DType = 1 << 1,
		DStructBase = 1 << 2,
		DClass = 1 << 3,
		DEnum = 1 << 4,
		DStruct = 1 << 5,
		DFunction = 1 << 6,

		FField = 1 << 11,
		FProperty = 1 << 12,
		FNumericProperty = 1 << 13,
		FInt8Property = 1 << 14,
		FIntProperty = 1 << 15,
		FStructProperty = 1 << 16,
		FBoolProperty = 1 << 17,
		FStringProperty = 1 << 18,
		FEnumProperty = 1 << 19,
		FObjectProperty = 1 << 20,
		FArrayProperty = 1 << 21,
		FMapProperty = 1 << 22,
		FNameProperty = 1 << 23,
		FGuidProperty = 1 << 24,
	};

	enum class EPropertyFlags
	{
		None = 0,

		Edit = 1 << 0,
		Transient = 1 << 1,
		ReadOnly = 1 << 2,
	};

	enum class EObjectInternalFlags
	{
		None = 0,
		RootSet = 1 << 0,
		Reachable = 1 << 1,
		BeginDestroyed = 1 << 2,
		Garbage = 1 << 3,
		FinishDestroyed = 1 << 4,
	};

	ENUM_CLASS_FLAGS(EObjectFlags)
	ENUM_CLASS_FLAGS(EPropertyFlags)
	ENUM_CLASS_FLAGS(EObjectInternalFlags)
}

#define GENERATED_BODY(...) BODY_MACRO_COMBINE(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)

#define DECLARE_CLASS(TClass, TSuperClass, TPrivateAccessor) \
private: \
	TClass& operator=(TClass&&) = delete; \
	TClass& operator=(const TClass&) = delete; \
\
public: \
	using Super = TSuperClass; \
	inline static DClass* StaticClass() { return TPrivateAccessor(); }

#define RELAY_OBJECT_INITIALIZER_CONSTRUCTOR(TClass, TSuperClass) \
	explicit TClass(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get()) : TSuperClass(ObjectInitializer) {}

#define DEFINE_DEFAULT_CONSTRUCTOR_CALL(TClass) \
	static void __DefaultConstructor(const FObjectInitializer& X) { new (X.GetObj()) TClass(); }

#define DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(TClass) \
	static void __DefaultConstructor(const FObjectInitializer& X) { new (X.GetObj()) TClass(X); }

#define DECLARE_CLASS_INTRINSIC_API(TClass, TSuperClass, TRequiredAPI) \
	RELAY_OBJECT_INITIALIZER_CONSTRUCTOR(TClass, TSuperClass) \
	TRequiredAPI static DClass* GetPrivateStaticClass(); \
	DECLARE_CLASS(TClass, TSuperClass, TClass::GetPrivateStaticClass) \
	DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(TClass) \
	static void IntrinsicClassInit(DClass* Class);

#define DECLARE_CLASS_INTRINSIC(TClass, TSuperClass) \
	DECLARE_CLASS_INTRINSIC_API(TClass, TSuperClass, NO_API)

#define IMPLEMENT_CLASS_NO_AUTO_REGISTRATION_API(TClass, TRequiredAPI) \
	FClassRegistrationInfo Z_Registration_Info_DClass_##TClass; \
	TRequiredAPI DClass* TClass::GetPrivateStaticClass() \
	{ \
		if (!Z_Registration_Info_DClass_##TClass.InnerSingleton) \
		{ /* this could be handled with templates, but we want it external to avoid code bloat */ \
			 GetPrivateStaticClassBody( \
				STR("") , /* PackageName */ \
				STR(#TClass), \
				Z_Registration_Info_DClass_##TClass.InnerSingleton, \
				nullptr, /* RegisterNativeFunc */ \
				sizeof(TClass), \
				alignof(TClass), \
				EClassFlags::None, \
				(DClass::ClassConstructorType)InternalConstructor<TClass>, \
				&TClass::Super::StaticClass \
			); \
		} \
		return Z_Registration_Info_DClass_##TClass.InnerSingleton; \
	}

#define IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(TClass) \
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION_API(TClass, NO_API)

#define IMPLEMENT_CLASS_API(TClass, TRequiredAPI) \
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION_API(TClass, TRequiredAPI) \
	static FClassRegisterCompiledInInfo Z_AutoRegister_##TClass( \
		&Z_Construct_DClass_##TClass, \
		&TClass::StaticClass, \
		STR(#TClass), \
		&Z_Registration_Info_DClass_##TClass \
	);

#define IMPLEMENT_CLASS(TClass) \
	IMPLEMENT_CLASS_API(TClass, NO_API)

#define IMPLEMENT_INTRINSIC_CLASS(TClass, TRequiredAPI, TSuperClass, TSuperRequiredAPI, InitCode) \
	TRequiredAPI DClass* Z_Construct_DClass_##TClass(); \
	extern FClassRegistrationInfo Z_Registration_Info_DClass_##TClass; \
	extern TSuperRequiredAPI DClass* Z_Construct_DClass_##TSuperClass(); \
	struct Z_Construct_DClass_##TClass##_Statics \
	{ \
		static DClass* Construct() \
		{ \
			DClass* SuperClass = Z_Construct_DClass_##TSuperClass(); \
			DClass* Class = TClass::StaticClass(); \
			DObjectForceRegistration(Class); \
			/*check(Class->GetSuperClass() == SuperClass);*/ \
			TClass::IntrinsicClassInit(Class); \
			return Class; \
		} \
	}; \
	void TClass::IntrinsicClassInit(DClass* Class){ \
		InitCode \
	} \
	DClass* Z_Construct_DClass_##TClass() \
	{ \
		if (!Z_Registration_Info_DClass_##TClass.OuterSingleton) \
		{ \
			Z_Registration_Info_DClass_##TClass.OuterSingleton = Z_Construct_DClass_##TClass##_Statics::Construct(); \
		} \
		check(Z_Registration_Info_DClass_##TClass.OuterSingleton->GetClass()); \
		return Z_Registration_Info_DClass_##TClass.OuterSingleton; \
	}\
	IMPLEMENT_CLASS_API(TClass, TRequiredAPI)
