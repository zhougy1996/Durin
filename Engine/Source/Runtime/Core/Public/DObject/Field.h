#pragma once

class FField;

enum EClassFlags
{
	CLASS_None = 0,

	// Class is abstract and can't be instantiated directly
	CLASS_Abstract = 1,

	// Class was declared in C++ and has no generated code
	CLASS_Native = 1 << 1,
};
ENUM_CLASS_FLAGS(EClassFlags);

enum EClassCastFlags : uint64
{
	CASTCLASS_None = 0,

	CASTCLASS_UField = 1,
	CASTCLASS_FInt8Property = 1 << 1,
	CASTCLASS_UEnum = 1 << 2,
	CASTCLASS_FNumericProperty = 1 << 3,
};

class FFieldClass
{
	/** Name of this field class */
	FName Name;
	/** Unique Id of this field class (for casting) */
	uint64 Id;
	/** Cast flags used for casting to other classes */
	uint64 CastFlags;
	/** Class flags */
	EClassFlags ClassFlags;
	/** Super of this class */
	FFieldClass* SuperClass;
	/** Default instance of this class */
	FField* DefaultObject;
};

class FField
{
public:
	DOGE_NONCOPYABLE(FField)

	FFieldClass* ClassPrivate;

	FField* Next;

	FName NamePrivate;
};

#define DECLARE_FIELD(TClass, TSuperClass, TStaticFlags, TRequiredAPI) \
public: \
	TClass& operator=(TClass&&) = delete; \
	TClass& operator=(const TClass&) = delete;

