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
