#pragma once

#define DCLASS(...)
#define DPROPERTY(...)
#define DFUNCTION(...)

#define BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
#define BODY_MACRO_COMBINE(A, B, C, D) BODY_MACRO_COMBINE_INNER(A, B, C, D)

#define NO_API

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
	DStructure = 1 << 1,
	DClass = 1 << 2,
	DStruct = 1 << 3,
	DFunction = 1 << 4,

	FField = 1 << 11,
	FProperty = 1 << 12,
	FNumericProperty = 1 << 13,
	FInt8Property = 1 << 14,
	FIntProperty = 1 << 15,
	FStructProperty = 1 << 16,
};

ENUM_CLASS_FLAGS(EObjectFlags)

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

#define DECLARE_CLASS_INTRINSIC(TClass, TSuperClass) \
	RELAY_OBJECT_INITIALIZER_CONSTRUCTOR(TClass, TSuperClass) \
	NO_API static DClass* GetPrivateStaticClass(); \
	DECLARE_CLASS(TClass, TSuperClass, TClass::GetPrivateStaticClass) \
	DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(TClass) \
	static void IntrinsicClassInit(DClass* Class);

#define IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(TClass) \
	FClassRegistrationInfo Z_Registration_Info_DClass_##TClass; \
	DClass* TClass::GetPrivateStaticClass() \
	{ \
		if (!Z_Registration_Info_DClass_##TClass.InnerSingleton) \
		{ /* this could be handled with templates, but we want it external to avoid code bloat */ \
			GetPrivateStaticClassBody( \
				STR("") , /* PackageName */ \
				STR(#TClass), \
				nullptr, /* RegisterNativeFunc */ \
				sizeof(TClass), \
				alignof(TClass), \
				EClassFlags::None, \
				(DClass::ClassConstructorType)InternalConstructor<TClass> \
			); \
		} \
		return Z_Registration_Info_DClass_##TClass.InnerSingleton; \
	}

#define IMPLEMENT_CLASS(TClass) \
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(TClass) \
	static FClassRegisterCompiledInInfo Z_AutoRegister_##TClass( \
		&Z_Construct_DClass_##TClass, \
		&TClass::StaticClass, \
		STR(#TClass), \
		&Z_Registration_Info_DClass_##TClass \
	);

#define IMPLEMENT_INTRINSIC_CLASS(TClass, TRequiredAPI, TSuperClass, TSuperRequiredAPI, InitCode) \
	TRequiredAPI DClass* Z_Construct_DClass_##TClass(); \
	extern FClassRegistrationInfo Z_Registration_Info_DClass_##TClass; \
	struct Z_Construct_DClass_##TClass##_Statics \
	{ \
		static DClass* Construct() \
		{ \
			extern TSuperRequiredAPI DClass* Z_Construct_DClass_##TSuperClass(); \
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
	IMPLEMENT_CLASS(TClass)

