#pragma once

class FField;

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

#define DECLARE_FIELD(TClass, TSuperClass, TStaticFlags, TRequiredAPI) \
public: \
	TClass& operator=(TClass&&) = delete; \
	TClass& operator=(const TClass&) = delete; \
	\
	using Super = DOGE_REMOVE_OPTIONAL_PARENS(TSuperClass); \
	static TRequiredAPI FFieldClass* StaticClass(); \
	/* Internal ClassCastFlags without super class flags */ \
	inline static constexpr uint64 StaticClassCastFlagsPrivate() { return static_cast<uint64>(TStaticFlags); } \
	/* ClassCastFlags including super class flags, also used as id in FFieldClass */ \
	inline static constexpr uint64 StaticClassCastFlags() { return static_cast<uint64>(TStaticFlags) | Super::StaticClassCastFlags(); } \


class FField
{
public:
	DOGE_NONCOPYABLE(FField)

	inline static constexpr uint64 StaticClassCastFlagsPrivate()
	{
		return static_cast<uint64>(EClassCastFlags::FField);
	}

	inline static constexpr uint64 StaticClassCastFlags()
	{
		return static_cast<uint64>(EClassCastFlags::FField);
	}

	FFieldClass* ClassPrivate;

	FField* Next;

	FName NamePrivate;
};

