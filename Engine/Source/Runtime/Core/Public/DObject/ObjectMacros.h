#pragma once

#define DCLASS(...)
#define DPROPERTY(...)
#define DFUNCTION(...)

#define BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
#define BODY_MACRO_COMBINE(A, B, C, D) BODY_MACRO_COMBINE_INNER(A, B, C, D)

#define NO_API

// clang-format off
enum EInternal { EC_InternalUseOnlyConstructor };
// clang-format on

#define GENERATED_BODY(...) BODY_MACRO_COMBINE(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)

#define DECLARE_CLASS(TClass, TSuperClass, TPrivateAccessor) \
private: \
	TClass& operator=(TClass&&); \
	TClass& operator=(const TClass&); \
\
public: \
	using Super = TSuperClass; \
	inline static DClass* StaticClass() { return TPrivateAccessor(); }

#define DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(TClass) \
	static void __DefaultConstructor(const FObjectInitializer& X) { new (X.GetObj()) TClass(X); }


#define DECLARE_CLASS_INTRINSIC(TClass, TSuperClass) \
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
				STR(#TClass), \
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

